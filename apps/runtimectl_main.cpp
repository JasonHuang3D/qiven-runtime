// ============================================================================
// apps/runtimectl_main.cpp — qiven-runtimectl, the read-only operational
// client (ARCH section 6.4; MVP-2 local surface + MVP-3 IPC surface)
//
//   qiven-runtimectl cognition show [--root <checkout>]
//   qiven-runtimectl index rebuild --core <cognition-core.yaml>
//                                 --policy <cognition-activation-policy.yaml>
//                                 --request <checkout bindings json>
//                                 --root <workspace root> --generation <id>
//   qiven-runtimectl index status  [--root <workspace root>]
//   qiven-runtimectl profile show  [--profile <file>]
//   qiven-runtimectl status show    [--root <checkout>]   (authenticated IPC)
//   qiven-runtimectl doctor show    [--root <checkout>]   (authenticated IPC)
//
// An unreachable or uninstalled host is reported as such (exit 1) — never
// as "governed" (the ARCH section 15 MVP-4 row 5 discipline, applied
// early). Exit codes: 0 pass, 1 failure, 2 usage.
// ============================================================================

#include <qiven/crt_failure.hpp>
#include <qiven/hashing_sha256.hpp>
#include <qiven/runtime/cognition/activation_index.hpp>
#include <qiven/runtime/cognition/activation_policy.hpp>
#include <qiven/runtime/cognition/activation_receipt.hpp>
#include <qiven/runtime/cognition/activation_service.hpp>
#include <qiven/runtime/cognition/bundle.hpp>
#include <qiven/runtime/cognition/shadow_compare.hpp>
#include <qiven/runtime/cognition/source_lock.hpp>
#include <qiven/runtime/host/deployment_profile.hpp>
#include <qiven/runtime/ipc/framing.hpp>
#include <qiven/runtime/ipc/named_pipe_server.hpp>
#include <qiven/runtime/ipc/protocol.hpp>
#include <qiven/runtime/processx/process_runner.hpp>

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

namespace
{
constexpr int exit_ok    = 0;
constexpr int exit_fail  = 1;
constexpr int exit_usage = 2;

int usage()
{
    std::cerr << "usage: qiven-runtimectl cognition show [--root <qiven-context checkout>]\n"
              << "       qiven-runtimectl cognition activate --core <yaml> --policy <yaml>"
                 " --envelope <json> --root <dir> --generation <id>"
                 " --runtime-generation <id> --source-lock <sha256> [--profile <name>]\n"
              << "       qiven-runtimectl index rebuild --core <yaml> --policy <yaml>"
                 " --request <json> --workspace-lock <json> --root <dir> --generation <id>\n"
              << "       qiven-runtimectl index shadow-compare --core <yaml>"
                 " --request <json> --workspace-lock <json>\n"
              << "       qiven-runtimectl index status [--root <dir>]\n"
              << "       qiven-runtimectl profile show [--profile <profile file>]\n"
              << "       qiven-runtimectl status show [--root <qiven-context checkout>]\n"
              << "       qiven-runtimectl doctor show [--root <qiven-context checkout>]\n"
              << "       qiven-runtimectl host shutdown [--root <qiven-context checkout>] "
                 "[--grace-ms N]\n"
              << "       qiven-runtimectl host refresh [--root <qiven-context checkout>] "
                 "[--wait-ms N]\n";
    return exit_usage;
}

std::filesystem::path flag_value(const std::vector<std::string>& args, const std::string& flag)
{
    for (std::size_t i = 0; i + 1 < args.size(); ++i)
    {
        if (args[i] == flag)
        {
            return args[i + 1];
        }
    }
    return {};
}

int cognition_show(const std::vector<std::string>& args)
{
    std::filesystem::path repo_root = std::filesystem::current_path();
    if (const auto given = flag_value(args, "--root"); !given.empty())
    {
        repo_root = given;
    }
    const qiven::runtime::cognition::BundleStore store(repo_root / ".qiven" / "runtime");
    auto bundle = store.load_active();
    if (!bundle.is_ok())
    {
        std::cout << "cognition: NO VERIFIED ACTIVE BUNDLE (" << bundle.reason().detail << ")\n";
        return exit_fail;
    }
    const auto& manifest = bundle.value().manifest;
    std::cout << "cognition: ACTIVE bundle verified\n"
              << "  schema           : " << manifest.schema << "\n"
              << "  repository       : " << manifest.repository << "\n"
              << "  source_revision  : " << manifest.source_revision << "  (git commit oid)\n"
              << "  source_tree      : " << manifest.source_tree << "  (git tree oid)\n"
              << "  view             : " << manifest.view << "\n"
              << "  snapshot_format  : " << manifest.snapshot_format << "\n"
              << "  snapshot_sha256  : " << qiven::runtime::to_hex(manifest.snapshot_digest)
              << "  (content identity)\n"
              << "  policy_sha256    : " << qiven::runtime::to_hex(manifest.policy_digest)
              << "  (content identity)\n"
              << "  source_files     : " << manifest.source_files.size() << "\n"
              << "  publisher_build  : " << manifest.publisher_build << "\n"
              << "  published_at     : " << manifest.published_at << "\n"
              << "  bundle_dir       : " << bundle.value().dir.string() << "\n";
    return exit_ok;
}

int profile_show(const std::vector<std::string>& args)
{
    std::filesystem::path file = std::filesystem::current_path() /
                                 "config" / "profiles" / "zcode-jason-context-record-mvp.yaml";
    if (const auto given = flag_value(args, "--profile"); !given.empty())
    {
        file = given;
    }
    auto profile = qiven::runtime::host::load_profile_file(file);
    if (!profile.is_ok())
    {
        std::cout << "profile: NOT ACCEPTABLE (" << profile.reason().detail << ")\n";
        return exit_fail;
    }
    const auto& value = profile.value();
    std::cout << "profile: accepted\n"
              << "  profile_id    : " << value.profile_id << "\n"
              << "  revision      : " << value.revision << "\n"
              << "  control       : " << value.control_version << "\n"
              << "  resolver_reg  : " << value.resolver_registry_revision << "\n"
              << "  classifier    : " << value.classifier_contract_revision << "\n"
              << "  evidence      : " << value.conformance_evidence << "\n"
              << "  governed paths (" << value.governed_paths.size() << "):\n";
    for (const auto& path : value.governed_paths)
    {
        std::cout << "    " << path << "\n";
    }
    std::cout << "  cognition repository : " << value.cognition.repository << "\n"
              << "  authorized ref       : " << value.cognition.authorized_ref << "\n"
              << "  policy path          : " << value.cognition.policy_path << "\n"
              << "  policy sha256        : " << value.cognition.policy_sha256 << "\n"
              << "  source paths (" << value.cognition.source_paths.size() << ")\n"
              << "  git executable       : " << value.git_executable.string() << "\n";
    return exit_ok;
}

// One authenticated round-trip over the installation pipe. EVERY client
// read is bounded (host-server redesign LL-3: no operator tool may hang
// against the server); default budget 5000 ms.
qiven::Result<qiven::runtime::ipc::Reply> transact(
    const std::filesystem::path& runtime_root, const qiven::runtime::ipc::Request& request,
    qiven::u64 read_budget_ms = 5000)
{
    using ReplyResult = qiven::Result<qiven::runtime::ipc::Reply>;
    auto secret       = qiven::runtime::ipc::InstallationSecret::ensure(runtime_root);
    if (!secret.is_ok())
    {
        return ReplyResult::fail(secret.reason());
    }
    const qiven::runtime::ipc::FrameCodec codec(secret.value());

    const std::filesystem::path install_file = runtime_root / "install.id";
    if (!std::filesystem::exists(install_file))
    {
        return ReplyResult::fail(qiven::Error::make(qiven::error_category::unavailable, 62,
                                                    "no installed RuntimeHost (install.id absent)"));
    }
    std::ifstream in(install_file);
    std::string install_id((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char> {});
    while (!install_id.empty() &&
           (install_id.back() == '\n' || install_id.back() == '\r' || install_id.back() == ' '))
    {
        install_id.pop_back();
    }

    auto client =
        qiven::runtime::ipc::PipeClient::connect(qiven::runtime::ipc::pipe_name(install_id));
    if (!client.is_ok())
    {
        return ReplyResult::fail(client.reason());
    }
    qiven::runtime::ipc::FrameHeader header;
    header.request_id     = 1;
    header.connection_seq = 1;
    if (!client.value().write_bytes(codec.encode(
                                        header, qiven::runtime::ipc::encode_request_body(request)),
                                    read_budget_ms))
    {
        return ReplyResult::fail(
            qiven::Error::make(qiven::error_category::unavailable, 65, "request write failed"));
    }
    auto frame = client.value().read_frame(read_budget_ms);
    if (!frame.has_value())
    {
        return ReplyResult::fail(
            qiven::Error::make(qiven::error_category::unavailable, 65, "no reply from host"));
    }
    auto verified = codec.decode(frame.value());
    if (!verified.is_ok())
    {
        return ReplyResult::fail(verified.reason());
    }
    auto reply = qiven::runtime::ipc::decode_reply(verified.value().body);
    if (!reply.is_ok())
    {
        return ReplyResult::fail(qiven::Error::make(qiven::error_category::invalid_argument,
                                                    reply.reason().code, reply.reason().detail));
    }
    return ReplyResult(std::move(reply.value()));
}

int ipc_show(const std::vector<std::string>& args, bool doctor)
{
    std::filesystem::path repo_root = std::filesystem::current_path();
    if (const auto given = flag_value(args, "--root"); !given.empty())
    {
        repo_root = given;
    }
    qiven::runtime::ipc::Request request;
    request.kind       = doctor ? qiven::runtime::ipc::Request::Kind::Doctor
                                : qiven::runtime::ipc::Request::Kind::Status;
    request.request_id = 1;
    auto reply         = transact(repo_root / ".qiven" / "runtime", request);
    if (!reply.is_ok())
    {
        std::cout << (doctor ? "doctor" : "status") << ": HOST UNREACHABLE ("
                  << reply.reason().message << ") - nothing is reported governed\n";
        return exit_fail;
    }
    if (reply.value().kind == qiven::runtime::ipc::Reply::Kind::ErrorView)
    {
        std::cout << (doctor ? "doctor" : "status") << ": HOST DENIED ("
                  << reply.value().error_code << ": " << reply.value().error_detail << ")\n";
        return exit_fail;
    }
    if (doctor)
    {
        std::cout << "doctor: " << (reply.value().findings.empty() ? "healthy" : "findings")
                  << "\n"
                  << "  integrity_ok   : " << (reply.value().integrity_ok ? "yes" : "no") << "\n"
                  << "  audit_chain_ok : " << (reply.value().audit_chain_ok ? "yes" : "no") << "\n"
                  << "  bundle_active  : " << (reply.value().bundle_active_ok ? "yes" : "no")
                  << "\n";
        for (const auto& finding : reply.value().findings)
        {
            std::cout << "  - " << finding << "\n";
        }
        return reply.value().findings.empty() ? exit_ok : exit_fail;
    }
    std::cout << "status:\n"
              << "  state           : " << reply.value().state << "\n"
              << "  install_id      : " << reply.value().install_id << "\n"
              << "  boot_epoch      : " << reply.value().boot_epoch << "\n"
              << "  generation      : " << reply.value().generation << "\n"
              << "  bundle_revision : " << reply.value().bundle_revision << "\n"
              << "  journal_events  : " << reply.value().journal_events << "\n"
              << "  refresh_state   : " << reply.value().refresh_state << "\n"
              << "  last_refresh_ok : " << reply.value().last_refresh_ok_ms << " ms\n"
              << "  next_refresh_due: " << reply.value().next_refresh_due_ms << " ms\n"
              << "  quarantined     : " << (reply.value().quarantined ? "yes" : "no") << "\n";
    return exit_ok;
}
// The one non-read verb (MVP-4 H-4; batch design delta 1.4): an explicit,
// authenticated, audit-logged operator shutdown. The ack precedes the
// host's drain.
int host_shutdown(const std::vector<std::string>& args)
{
    std::filesystem::path repo_root = std::filesystem::current_path();
    if (const auto given = flag_value(args, "--root"); !given.empty())
    {
        repo_root = given;
    }
    qiven::u64 grace_ms = 3000;
    if (const auto given = flag_value(args, "--grace-ms"); !given.empty())
    {
        grace_ms = static_cast<qiven::u64>(std::strtoull(given.string().c_str(), nullptr, 10));
    }
    if (grace_ms == 0 || grace_ms > 30000)
    {
        std::cout << "shutdown: --grace-ms must be in (0, 30000]\n";
        return exit_usage;
    }
    qiven::runtime::ipc::Request request;
    request.kind       = qiven::runtime::ipc::Request::Kind::Shutdown;
    request.request_id = 1;
    request.grace_ms   = grace_ms;
    auto reply         = transact(repo_root / ".qiven" / "runtime", request);
    if (!reply.is_ok())
    {
        std::cout << "shutdown: HOST UNREACHABLE (" << reply.reason().message
                  << ") - nothing is reported governed\n";
        return exit_fail;
    }
    if (reply.value().kind == qiven::runtime::ipc::Reply::Kind::ShutdownAck)
    {
        std::cout << "shutdown: acknowledged; host is draining (grace " << grace_ms << " ms)\n";
        return exit_ok;
    }
    std::cout << "shutdown: HOST DENIED (" << reply.value().error_code << ": "
              << reply.value().error_detail << ")\n";
    return exit_fail;
}

// Host-server redesign §6: the operator refresh TRIGGER — authenticated,
// audit-logged, coalesced server-side, never blocking the server. With
// --wait-ms N the tool ALSO waits bounded for the worker's outcome (a
// bounded human-facing CLI poll of the status verb, never an LLM loop).
int host_refresh(const std::vector<std::string>& args)
{
    std::filesystem::path repo_root = std::filesystem::current_path();
    if (const auto given = flag_value(args, "--root"); !given.empty())
    {
        repo_root = given;
    }
    qiven::u64 wait_ms = 0;
    if (const auto given = flag_value(args, "--wait-ms"); !given.empty())
    {
        wait_ms = static_cast<qiven::u64>(std::strtoull(given.string().c_str(), nullptr, 10));
    }
    if (wait_ms > 60000)
    {
        std::cout << "refresh: --wait-ms must be <= 60000\n";
        return exit_usage;
    }
    qiven::runtime::ipc::Request request;
    request.kind       = qiven::runtime::ipc::Request::Kind::Refresh;
    request.request_id = 1;
    auto reply         = transact(repo_root / ".qiven" / "runtime", request);
    if (!reply.is_ok())
    {
        std::cout << "refresh: HOST UNREACHABLE (" << reply.reason().message
                  << ") - nothing is reported governed\n";
        return exit_fail;
    }
    if (reply.value().kind != qiven::runtime::ipc::Reply::Kind::RefreshAck)
    {
        std::cout << "refresh: HOST DENIED (" << reply.value().error_code << ": "
                  << reply.value().error_detail << ")\n";
        return exit_fail;
    }
    std::cout << "refresh: " << reply.value().refresh_result << " (state "
              << reply.value().refresh_state << ", next due "
              << reply.value().next_refresh_due_ms << " ms)\n";
    if (wait_ms == 0)
    {
        return exit_ok;
    }
    // Bounded outcome wait: the trigger reply's last_refresh_ok is the
    // PRE-attempt clock; the wait ends when status reports a NEWER one
    // (the attempt completed) or the budget ends.
    const qiven::u64 before_ms = reply.value().last_refresh_ok_ms;
    const auto deadline        = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(wait_ms);
    while (std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        qiven::runtime::ipc::Request poll;
        poll.kind       = qiven::runtime::ipc::Request::Kind::Status;
        poll.request_id = 2;
        auto state      = transact(repo_root / ".qiven" / "runtime", poll);
        if (state.is_ok() &&
            state.value().kind == qiven::runtime::ipc::Reply::Kind::StatusView &&
            state.value().last_refresh_ok_ms != before_ms)
        {
            std::cout << "refresh: outcome " << state.value().refresh_state
                      << " (last ok " << state.value().last_refresh_ok_ms << " ms)\n";
            return exit_ok;
        }
    }
    std::cout << "refresh: outcome not observed within " << wait_ms
              << " ms (the worker retries on its cadence)\n";
    return exit_fail;
}
} // namespace

// ---- CA-1: activation index verbs -----------------------------------------
// The lock request binds each corpus repository (cognition-core.yaml) to
// a LOCAL checkout + exact ref: [{"repository": "...", "checkout": "...",
// "ref": "..."}]. Minimal strict JSON extraction — exact key order is
// not required, unknown keys are ignored (the SOURCE LOCK validates the
// real constraints; this only locates the checkouts).
namespace
{
struct CheckoutBinding
{
    std::string repository;
    std::string checkout;
    std::string ref;
};

std::vector<CheckoutBinding> parse_bindings(const std::string& text)
{
    std::vector<CheckoutBinding> bindings;
    qiven::usize position = 0;
    while (true)
    {
        const qiven::usize object_begin = text.find('{', position);
        if (object_begin == std::string::npos)
        {
            break;
        }
        const qiven::usize object_end = text.find('}', object_begin);
        if (object_end == std::string::npos)
        {
            break;
        }
        const std::string object = text.substr(object_begin, object_end - object_begin);
        auto field               = [&](const char* key) {
            const std::string needle = std::string("\"") + key + "\":\"";
            const qiven::usize at    = object.find(needle);
            if (at == std::string::npos)
            {
                return std::string();
            }
            const qiven::usize start = at + needle.size();
            const qiven::usize end   = object.find('"', start);
            return end == std::string::npos ? std::string() : object.substr(start, end - start);
        };
        CheckoutBinding binding;
        binding.repository = field("repository");
        binding.checkout   = field("checkout");
        binding.ref        = field("ref");
        if (!binding.repository.empty() && !binding.checkout.empty() && !binding.ref.empty())
        {
            bindings.push_back(std::move(binding));
        }
        position = object_end + 1;
    }
    return bindings;
}

qiven::u64 now_ms_epoch()
{
    return static_cast<qiven::u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::system_clock::now().time_since_epoch())
                                       .count());
}

std::optional<std::string> read_file_text(const std::filesystem::path& file);

// WR-7 cutover shared helper: tier-2 filter sets read at the workspace
// lock's context (carrier) node revision — the corpus table that exists at
// the LOCKED revision, compared against the lock-driving table.
qiven::runtime::cognition::ShadowFilterSets filters_at_carrier_node(
    const qiven::runtime::processx::ProcessRunner& runner,
    const std::vector<CheckoutBinding>& bindings,
    const qiven::runtime::cognition::WorkspaceLockState& workspace)
{
    qiven::runtime::cognition::ShadowFilterSets at_node;
    const std::string* carrier_checkout = nullptr;
    for (const CheckoutBinding& binding : bindings)
    {
        if (binding.repository == "qiven-context")
        {
            carrier_checkout = &binding.checkout;
            break;
        }
    }
    auto carrier_node = workspace.nodes.find("qiven-context");
    if (carrier_checkout != nullptr && carrier_node != workspace.nodes.end() &&
        carrier_node->second.commit.size() == 40)
    {
        qiven::runtime::processx::ProcessSpec show;
        show.executable  = R"(C:\Program Files\Git\cmd\git.exe)";
        show.argv        = { "git", "-C", *carrier_checkout, "show",
                             carrier_node->second.commit + ":runtime/cognition/cognition-core.yaml" };
        show.deadline_ms = 30'000;
        auto shown       = runner.run(show);
        if (shown.is_ok() && shown.value().exit_code == 0)
        {
            auto node_core = qiven::runtime::cognition::parse_cognition_core(shown.value().out);
            if (node_core.is_ok())
            {
                at_node.carrier_table_present = true;
                for (const auto& corpus : node_core.value().corpus)
                {
                    at_node.filters.emplace(corpus.repository, corpus.path_filters);
                }
            }
        }
        // unreadable/absent/malformed at the node revision stays
        // carrier_table_present=false: FilterSetMismatch, fail-closed
    }
    return at_node;
}

int index_shadow_compare(const std::vector<std::string>& args)
{
    // WR-7 engineering leg: shadow-compare the TCA source closure against
    // the WorkspaceGeneration. PURE + NON-MUTATING (no index build, no
    // ACTIVE pointer, no digest into the lock/ActivationGeneration);
    // divergence fails the WR-7 migration gate only.
    std::filesystem::path core_path;
    std::filesystem::path request_path;
    std::filesystem::path workspace_lock_path;
    if (const auto given = flag_value(args, "--core"); !given.empty())
    {
        core_path = given;
    }
    if (const auto given = flag_value(args, "--request"); !given.empty())
    {
        request_path = given;
    }
    if (const auto given = flag_value(args, "--workspace-lock"); !given.empty())
    {
        workspace_lock_path = given;
    }
    if (core_path.empty() || request_path.empty() || workspace_lock_path.empty())
    {
        std::cout << "index shadow-compare: --core, --request and --workspace-lock are required\n";
        return exit_usage;
    }
    const auto core_text      = read_file_text(core_path);
    const auto request_text   = read_file_text(request_path);
    const auto workspace_text = read_file_text(workspace_lock_path);
    if (!core_text || !request_text || !workspace_text)
    {
        std::cout << "index shadow-compare: unreadable core/request/workspace-lock file\n";
        return exit_usage;
    }
    auto core = qiven::runtime::cognition::parse_cognition_core(*core_text);
    if (!core.is_ok())
    {
        std::cout << "index shadow-compare: core rejected at line "
                  << core.reason().line << ": " << core.reason().detail << "\n";
        return exit_fail;
    }
    const std::vector<CheckoutBinding> bindings = parse_bindings(*request_text);

    qiven::runtime::cognition::SourceLockRequest lock_request;
    for (const auto& corpus : core.value().corpus)
    {
        const CheckoutBinding* bound = nullptr;
        for (const CheckoutBinding& binding : bindings)
        {
            if (binding.repository == corpus.repository)
            {
                bound = &binding;
                break;
            }
        }
        if (bound == nullptr)
        {
            std::cout << "index shadow-compare: corpus repository " << corpus.repository
                      << " has no checkout binding in the request\n";
            return exit_fail;
        }
        qiven::runtime::cognition::LockRepository locked;
        locked.repository   = corpus.repository;
        locked.path_filters = corpus.path_filters;
        locked.checkout     = bound->checkout;
        locked.ref          = bound->ref;
        lock_request.repositories.push_back(std::move(locked));
    }
    lock_request.git_executable = R"(C:\Program Files\Git\cmd\git.exe)";
    const qiven::runtime::processx::ProcessRunner runner;
    lock_request.runner = &runner;

    const qiven::runtime::cognition::SourceLockBuilder lock_builder;
    auto lock = lock_builder.build(lock_request);
    if (!lock.is_ok())
    {
        // name the failing repository: the builder fails on the first
        // typed fault without saying which closure member produced it
        std::string failing = "<unknown>";
        for (const auto& corpus : core.value().corpus)
        {
            const CheckoutBinding* bound = nullptr;
            for (const CheckoutBinding& binding : bindings)
            {
                if (binding.repository == corpus.repository)
                {
                    bound = &binding;
                    break;
                }
            }
            if (bound == nullptr)
            {
                continue;
            }
            qiven::runtime::cognition::SourceLockRequest one;
            qiven::runtime::cognition::LockRepository single;
            single.repository   = corpus.repository;
            single.path_filters = corpus.path_filters;
            single.checkout     = bound->checkout;
            single.ref          = bound->ref;
            one.repositories.push_back(std::move(single));
            one.git_executable = R"(C:\Program Files\Git\cmd\git.exe)";
            one.runner         = &runner;
            auto single_lock   = lock_builder.build(one);
            if (!single_lock.is_ok())
            {
                failing = corpus.repository;
                break;
            }
        }
        std::cout << "index shadow-compare: source lock failed at [" << failing << "] ("
                  << qiven::runtime::cognition::lock_error_text(lock.reason())
                  << ") - observed CA-1 builder state, not an equivalence verdict\n";
        return exit_fail;
    }

    qiven::runtime::cognition::WorkspaceLockState workspace =
        qiven::runtime::cognition::parse_workspace_lock(*workspace_text);
    if (workspace.nodes.empty() || workspace.generation.empty())
    {
        std::cout << "index shadow-compare: workspace lock unreadable or has no nodes\n";
        return exit_fail;
    }

    // tier 2 (the spec's complete comparison): the corpus table re-read
    // at the workspace node's CONTEXT (carrier) revision, compared with
    // the lock-driving table. Per-file digests follow from (commit,
    // root tree) + filters by construction; the filter dimension is
    // what the tree identity cannot express.
    qiven::runtime::cognition::ShadowFilterSets filters_used;
    filters_used.carrier_table_present = true;
    for (const auto& corpus : core.value().corpus)
    {
        filters_used.filters.emplace(corpus.repository, corpus.path_filters);
    }
    qiven::runtime::cognition::ShadowFilterSets filters_at_node =
        filters_at_carrier_node(runner, bindings, workspace);
    qiven::runtime::cognition::ShadowCompareResult compared =
        qiven::runtime::cognition::shadow_compare(lock.value(), workspace, filters_used,
                                                  filters_at_node);
    compared.workspace_lock_binding = "fresh-build";
    std::cout << qiven::runtime::cognition::shadow_compare_receipt_json(compared) << "\n";
    return compared.all_equal() ? exit_ok : exit_fail;
}

std::optional<std::string> read_file_text(const std::filesystem::path& file)
{
    std::ifstream in(file, std::ios::binary);
    if (!in)
    {
        return std::nullopt;
    }
    in.seekg(0, std::ios::end);
    const auto size = in.tellg();
    if (size < 0)
    {
        return std::nullopt;
    }
    std::string text;
    text.resize(static_cast<std::size_t>(size));
    in.seekg(0, std::ios::beg);
    if (!text.empty())
    {
        in.read(text.data(), static_cast<std::streamsize>(text.size()));
    }
    if (!in)
    {
        return std::nullopt;
    }
    return text;
}

int index_rebuild(const std::vector<std::string>& args)
{
    std::filesystem::path core_path;
    std::filesystem::path policy_path;
    std::filesystem::path request_path;
    std::filesystem::path workspace_lock_path;
    std::filesystem::path root;
    std::string generation_id;
    if (const auto given = flag_value(args, "--core"); !given.empty())
    {
        core_path = given;
    }
    if (const auto given = flag_value(args, "--policy"); !given.empty())
    {
        policy_path = given;
    }
    if (const auto given = flag_value(args, "--request"); !given.empty())
    {
        request_path = given;
    }
    if (const auto given = flag_value(args, "--workspace-lock"); !given.empty())
    {
        workspace_lock_path = given;
    }
    if (const auto given = flag_value(args, "--root"); !given.empty())
    {
        root = given;
    }
    if (const auto given = flag_value(args, "--generation"); !given.empty())
    {
        generation_id = given.string();
    }
    if (core_path.empty() || policy_path.empty() || request_path.empty() ||
        workspace_lock_path.empty() || root.empty() || generation_id.empty())
    {
        std::cout << "index rebuild: --core, --policy, --request, --workspace-lock, --root and"
                     " --generation are required\n";
        return exit_usage;
    }
    const auto core_text    = read_file_text(core_path);
    const auto policy_text  = read_file_text(policy_path);
    const auto request_text = read_file_text(request_path);
    if (!core_text || !policy_text || !request_text)
    {
        std::cout << "index rebuild: unreadable core/policy/request file\n";
        return exit_fail;
    }
    auto core   = qiven::runtime::cognition::parse_cognition_core(*core_text);
    auto policy = qiven::runtime::cognition::parse_activation_policy(*policy_text);
    if (!core.is_ok() || !policy.is_ok())
    {
        std::cout << "index rebuild: policy instance rejected ("
                  << (!core.is_ok() ? core.reason().detail : policy.reason().detail) << ")\n";
        return exit_fail;
    }

    // The pinned canonical bundle + execution generation come from the
    // ACTIVE verified bundle at the runtime root.
    const qiven::runtime::cognition::BundleStore store(root);
    auto bundle = store.load_active();
    if (!bundle.is_ok())
    {
        std::cout << "index rebuild: no verified ACTIVE bundle (" << bundle.reason().detail
                  << ")\n";
        return exit_fail;
    }

    const auto bindings = parse_bindings(*request_text);
    qiven::runtime::cognition::SourceLockRequest lock_request;
    for (const auto& corpus : core.value().corpus)
    {
        const CheckoutBinding* bound = nullptr;
        for (const CheckoutBinding& binding : bindings)
        {
            if (binding.repository == corpus.repository)
            {
                bound = &binding;
                break;
            }
        }
        if (bound == nullptr)
        {
            std::cout << "index rebuild: corpus repository " << corpus.repository
                      << " has no checkout binding in the request\n";
            return exit_fail;
        }
        qiven::runtime::cognition::LockRepository locked;
        locked.repository   = corpus.repository;
        locked.path_filters = corpus.path_filters;
        locked.checkout     = bound->checkout;
        locked.ref          = bound->ref;
        lock_request.repositories.push_back(std::move(locked));
    }
    lock_request.git_executable = R"(C:\Program Files\Git\cmd\git.exe)";
    const qiven::runtime::processx::ProcessRunner runner;
    lock_request.runner = &runner;

    const qiven::runtime::cognition::SourceLockBuilder lock_builder;
    auto lock = lock_builder.build(lock_request);
    if (!lock.is_ok())
    {
        std::cout << "index rebuild: source lock failed ("
                  << qiven::runtime::cognition::lock_error_text(lock.reason()) << ")\n";
        return exit_fail;
    }

    // ---- WR-7 CUTOVER GATE (ADR-0058; H1#2 accepted 2026-09-28): the
    // repository selection is bound to the WorkspaceGeneration. The
    // freshly built closure must be EQUAL to the workspace lock nodes
    // (tier-1 commit/tree + tier-2 corpus filters) and every compared
    // node must be repository-manifest (non-shadow). Divergence is a
    // typed build failure — the v42 StaleNode class (49-behind) is
    // structurally impossible to activate from, never a silent stale
    // read. E7.1: same-window lock-entry means the checkouts are at the
    // CURRENT admitted lock; a stale lock fails here.
    const auto workspace_text = read_file_text(workspace_lock_path);
    if (!workspace_text)
    {
        std::cout << "index rebuild: workspace lock unreadable\n";
        return exit_fail;
    }
    qiven::runtime::cognition::WorkspaceLockState workspace =
        qiven::runtime::cognition::parse_workspace_lock(*workspace_text);
    if (workspace.nodes.empty() || workspace.generation.empty())
    {
        std::cout << "index rebuild: workspace lock unreadable or has no nodes\n";
        return exit_fail;
    }
    {
        qiven::runtime::cognition::ShadowFilterSets filters_used;
        filters_used.carrier_table_present = true;
        for (const auto& corpus : core.value().corpus)
        {
            filters_used.filters.emplace(corpus.repository, corpus.path_filters);
        }
        const qiven::runtime::cognition::ShadowFilterSets filters_at_node =
            filters_at_carrier_node(runner, bindings, workspace);
        const qiven::runtime::cognition::ShadowCompareResult compared =
            qiven::runtime::cognition::shadow_compare(lock.value(), workspace, filters_used,
                                                      filters_at_node);
        if (!compared.cutover_grade())
        {
            std::cout << "index rebuild: WR-7 cutover gate FAILED - the closure does not"
                         " match the workspace selection (divergence or shadow-only node):\n";
            std::cout << qiven::runtime::cognition::shadow_compare_receipt_json(compared)
                      << "\n";
            return exit_fail;
        }
        std::cout << "index rebuild: WR-7 cutover gate PASS (workspace generation "
                  << workspace.generation << ")\n";
    }

    // canonical bundle digest = sha256 over the verified bundle's
    // manifest.json bytes (the publisher's content-address identity)
    const auto manifest_text = read_file_text(bundle.value().dir / "manifest.json");
    if (!manifest_text)
    {
        std::cout << "index rebuild: ACTIVE bundle manifest unreadable\n";
        return exit_fail;
    }
    qiven::SHA256Hasher manifest_hasher;
    manifest_hasher.update(reinterpret_cast<const std::byte*>(manifest_text->data()),
                           manifest_text->size());
    const qiven::SHA256Digest manifest_digest = manifest_hasher.finish();
    static constexpr char manifest_hex[]      = "0123456789abcdef";
    std::string bundle_digest;
    bundle_digest.resize(manifest_digest.size() * 2);
    for (std::size_t i = 0; i < manifest_digest.size(); ++i)
    {
        const auto b             = static_cast<unsigned char>(manifest_digest[i]);
        bundle_digest[2 * i]     = manifest_hex[b >> 4];
        bundle_digest[2 * i + 1] = manifest_hex[b & 0x0Fu];
    }

    qiven::runtime::cognition::IndexBuildRequest build;
    build.runtime_root            = root / ".qiven" / "runtime";
    build.source_lock             = std::move(lock.value());
    build.policy                  = std::move(policy.value());
    build.canonical_bundle_digest = bundle_digest;
    build.runtime_generation_id   = generation_id; // execution generation, its own identity axis
    build.publisher_build         = "qiven-runtime-ca1";
    build.workspace_generation    = workspace.generation; // provenance only (ADR-0058 d6)

    const qiven::runtime::cognition::ActivationIndexBuilder index_builder;
    auto built = index_builder.build(build);
    if (!built.is_ok())
    {
        std::cout << "index rebuild: build failed ("
                  << qiven::runtime::cognition::index_error_text(built.reason()) << ")\n";
        return exit_fail;
    }
    std::cout << "index rebuild: generation " << built.value().activation_generation << " ("
              << built.value().source_count << " sources, " << built.value().rule_count
              << " rules)\n";
    return exit_ok;
}

int index_status(const std::vector<std::string>& args)
{
    std::filesystem::path root;
    if (const auto given = flag_value(args, "--root"); !given.empty())
    {
        root = given;
    }
    if (root.empty())
    {
        std::cout << "index status: --root is required\n";
        return exit_usage;
    }
    const qiven::runtime::cognition::ActivationIndexBuilder builder;
    const auto generation =
        builder.active_generation(root / ".qiven" / "runtime");
    if (generation.empty())
    {
        std::cout << "index status: NO ACTIVE GENERATION\n";
        return exit_ok; // absence is a state, not a failure
    }
    std::cout << "index status: ACTIVE generation " << generation << "\n";
    return exit_ok;
}

// cognition activate: envelope json → shared-core activation. The envelope
// file carries the neutral task facts (condition-blind normalizer input):
// {"objective": "...", "repository": "...", "revision": "...",
//  "changed_paths": ["a", "b"], "phase": "implementation", "risk": "R2"}
int cognition_activate(const std::vector<std::string>& args)
{
    std::filesystem::path core_path;
    std::filesystem::path policy_path;
    std::filesystem::path envelope_path;
    std::filesystem::path root;
    std::string generation;
    std::string runtime_generation;
    std::string lock_digest;
    std::string profile = "zcode-jason";
    if (const auto given = flag_value(args, "--core"); !given.empty())
    {
        core_path = given;
    }
    if (const auto given = flag_value(args, "--policy"); !given.empty())
    {
        policy_path = given;
    }
    if (const auto given = flag_value(args, "--envelope"); !given.empty())
    {
        envelope_path = given;
    }
    if (const auto given = flag_value(args, "--root"); !given.empty())
    {
        root = given;
    }
    if (const auto given = flag_value(args, "--generation"); !given.empty())
    {
        generation = given.string();
    }
    if (const auto given = flag_value(args, "--runtime-generation"); !given.empty())
    {
        runtime_generation = given.string();
    }
    if (const auto given = flag_value(args, "--source-lock"); !given.empty())
    {
        lock_digest = given.string();
    }
    if (const auto given = flag_value(args, "--profile"); !given.empty())
    {
        profile = given.string();
    }
    if (core_path.empty() || policy_path.empty() || envelope_path.empty() || root.empty() ||
        generation.empty() || runtime_generation.empty() || lock_digest.empty())
    {
        std::cout << "activate: --core, --policy, --envelope, --root, --generation,"
                     " --runtime-generation and --source-lock are required\n";
        return exit_usage;
    }

    // ---- validate the operator-supplied facts against the SIDE CAR ----
    // (TCA section 10 step 1 / section 13.2: the one-shot path must bind
    // the LIVE activated state, never free strings. Every axis is checked
    // against the ACTIVE generation's index manifest; mismatch = typed
    // rejection, no receipt is minted.)
    const auto sidecar_root = root / ".qiven" / "runtime" / "activation-generations";
    const qiven::runtime::cognition::ActivationIndexBuilder index_builder;
    const std::string active = index_builder.active_generation(root / ".qiven" / "runtime");
    if (active.empty())
    {
        std::cout << "activate: NO ACTIVE activation generation - run `cognition index"
                     " rebuild` first\n";
        return exit_fail;
    }
    if (active != generation)
    {
        std::cout << "activate: --generation " << generation
                  << " does not match the ACTIVE generation " << active
                  << " - stale or fabricated axis rejected\n";
        return exit_fail;
    }
    const auto manifest_text = read_file_text(sidecar_root / active / "index-manifest.json");
    if (!manifest_text)
    {
        std::cout << "activate: ACTIVE index manifest unreadable\n";
        return exit_fail;
    }
    const auto manifest_field = [&](const char* key) {
        const std::string needle = std::string("\"") + key + "\":\"";
        const auto at            = manifest_text->find(needle);
        if (at == std::string::npos)
        {
            return std::string();
        }
        const auto begin = at + needle.size();
        const auto end   = manifest_text->find('"', begin);
        return end == std::string::npos ? std::string()
                                        : manifest_text->substr(begin, end - begin);
    };
    const std::string manifest_lock    = manifest_field("external_source_lock_sha256");
    const std::string manifest_runtime = manifest_field("runtime_generation");
    const std::string manifest_policy  = manifest_field("activation_policy_sha256");
    if (manifest_lock != lock_digest)
    {
        std::cout << "activate: --source-lock does not match the ACTIVE index manifest ("
                  << manifest_lock << ") - stale or fabricated axis rejected\n";
        return exit_fail;
    }
    if (manifest_runtime != runtime_generation)
    {
        std::cout << "activate: --runtime-generation does not match the ACTIVE index manifest ("
                  << manifest_runtime << ") - stale or fabricated axis rejected\n";
        return exit_fail;
    }

    const auto core_text     = read_file_text(core_path);
    const auto policy_text   = read_file_text(policy_path);
    const auto envelope_text = read_file_text(envelope_path);
    if (!core_text || !policy_text || !envelope_text)
    {
        std::cout << "activate: unreadable core/policy/envelope file\n";
        return exit_fail;
    }
    auto core   = qiven::runtime::cognition::parse_cognition_core(*core_text);
    auto policy = qiven::runtime::cognition::parse_activation_policy(*policy_text);
    if (!core.is_ok() || !policy.is_ok())
    {
        std::cout << "activate: policy rejected ("
                  << (!core.is_ok() ? core.reason().detail : policy.reason().detail) << ")\n";
        return exit_fail;
    }

    // envelope extraction (exact fields; unknown keys ignored)
    auto field = [&](const char* key) {
        const std::string needle = std::string("\"") + key + "\":\"";
        const qiven::usize at    = envelope_text->find(needle);
        if (at == std::string::npos)
        {
            return std::string();
        }
        const qiven::usize begin = at + needle.size();
        const qiven::usize end   = envelope_text->find('"', begin);
        return end == std::string::npos ? std::string() : envelope_text->substr(begin, end - begin);
    };
    qiven::runtime::cognition::TaskEnvelope envelope;
    envelope.objective  = field("objective");
    envelope.repository = field("repository");
    envelope.revision   = field("revision");
    if (envelope.objective.empty() || envelope.repository.empty())
    {
        std::cout << "activate: envelope must carry objective and repository\n";
        return exit_fail;
    }
    // Fail-visible vocabulary law on operator-supplied axes: an unknown or
    // empty phase/risk is rejected with the admissible set, never silently
    // coerced to a default (a coerced axis silently changes typed selection).
    const std::string phase_text_field = field("phase");
    const auto phase                   = qiven::runtime::cognition::parse_task_phase(phase_text_field);
    if (!phase)
    {
        std::cout << "activate: envelope phase '" << phase_text_field
                  << "' is not vocabulary (specify|design|implementation|review|acceptance)\n";
        return exit_fail;
    }
    envelope.phase                    = *phase;
    const std::string risk_text_field = field("risk");
    const auto risk                   = qiven::runtime::cognition::parse_task_risk(risk_text_field);
    if (!risk)
    {
        std::cout << "activate: envelope risk '" << risk_text_field
                  << "' is not vocabulary (R0|R1|R2|R3)\n";
        return exit_fail;
    }
    envelope.risk = *risk;
    {
        const std::string needle = "\"changed_paths\":[";
        const qiven::usize at    = envelope_text->find(needle);
        if (at != std::string::npos)
        {
            const qiven::usize end = envelope_text->find(']', at);
            if (end != std::string::npos)
            {
                const std::string list = envelope_text->substr(at + needle.size(), end - at - needle.size());
                qiven::usize position  = 0;
                while (position < list.size())
                {
                    const qiven::usize quote_begin = list.find('"', position);
                    if (quote_begin == std::string::npos)
                    {
                        break;
                    }
                    const qiven::usize quote_end = list.find('"', quote_begin + 1);
                    if (quote_end == std::string::npos)
                    {
                        break;
                    }
                    envelope.changed_paths.push_back(
                        list.substr(quote_begin + 1, quote_end - quote_begin - 1));
                    position = quote_end + 1;
                }
            }
        }
    }

    // the policy digest recomputed over the parsed rule table (the same
    // canonical serialization the index binds)
    qiven::runtime::cognition::ActivationRequest request;
    request.envelope                    = envelope;
    request.core                        = std::move(core.value());
    request.policy                      = std::move(policy.value());
    request.activation_generation       = generation;
    request.runtime_generation_id       = runtime_generation;
    request.external_source_lock_sha256 = lock_digest;
    request.activation_policy_sha256 =
        qiven::runtime::cognition::activation_policy_digest(request.policy);
    // the parsed policy must BE the policy the ACTIVE index bound
    if (request.activation_policy_sha256 != manifest_policy)
    {
        std::cout << "activate: the parsed policy's digest does not match the ACTIVE index"
                     " manifest ("
                  << manifest_policy << ") - policy/axis drift rejected\n";
        return exit_fail;
    }
    request.consumer_profile = profile;
    request.runtime_root     = root / ".qiven" / "runtime";
    request.now_ms           = now_ms_epoch();

    // WR-7 cutover (ADR-0058 d6): the parent WorkspaceGeneration is a
    // validated axis, never a free string — checked against the ACTIVE
    // index's provenance sidecar (written by the gated rebuild).
    {
        const auto provenance_text =
            read_file_text(sidecar_root / active / "workspace-provenance.json");
        if (!provenance_text)
        {
            std::cout << "activate: ACTIVE index carries no workspace provenance record"
                         " (pre-cutover index - rerun `cognition index rebuild`)\n";
            return exit_fail;
        }
        const std::string needle = "\"workspace_generation\":\"";
        const auto at            = provenance_text->find(needle);
        if (at == std::string::npos)
        {
            std::cout << "activate: workspace provenance record malformed\n";
            return exit_fail;
        }
        const auto begin = at + needle.size();
        const auto end   = provenance_text->find('"', begin);
        if (end == std::string::npos)
        {
            std::cout << "activate: workspace provenance record malformed\n";
            return exit_fail;
        }
        request.workspace_generation = provenance_text->substr(begin, end - begin);
    }

    const qiven::runtime::cognition::ActivationService service;
    auto outcome = service.activate(request);
    if (!outcome.is_ok())
    {
        std::cout << "activate: " << qiven::runtime::cognition::activation_error_text(outcome.reason())
                  << "\n";
        return exit_fail;
    }
    std::cout << "activate: readiness "
              << qiven::runtime::cognition::readiness_text(outcome.value().selection.readiness)
              << "\n  bundle " << outcome.value().bundle.bundle_id << " ("
              << outcome.value().bundle.protected_count << " protected, "
              << outcome.value().bundle.candidate_count << " candidates, "
              << outcome.value().bundle.payload_bytes << " bytes)\n"
              << "  receipt " << outcome.value().receipt.receipt_id << "\n"
              << "  workspace " << outcome.value().receipt.workspace_generation << "\n";
    return exit_ok;
}

// cognition activation show|explain|verify-receipt: the receipt surfaces
// (roadmap CA-1: issue, persist, EXPLAIN, INVALIDATE, verify — v1 CLI
// carries show/explain/verify-receipt; invalidation is implicit by
// generation movement (receipts fail verify on any bound-axis change)
// and by deleting the sidecar journal entry at re-key time).
int activation_show(const std::vector<std::string>& args)
{
    std::filesystem::path root;
    std::string receipt_id;
    if (const auto given = flag_value(args, "--root"); !given.empty())
    {
        root = given;
    }
    if (const auto given = flag_value(args, "--id"); !given.empty())
    {
        receipt_id = given.string();
    }
    if (root.empty())
    {
        std::cout << "activation show: --root is required ([--id <receipt>])\n";
        return exit_usage;
    }
    const qiven::runtime::cognition::ActivationIndexBuilder index_builder;
    const std::string active =
        index_builder.active_generation(root / ".qiven" / "runtime");
    std::cout << "activation show: ACTIVE generation "
              << (active.empty() ? "<none>" : active) << "\n";
    if (!receipt_id.empty())
    {
        const qiven::runtime::cognition::ActivationReceiptJournal journal(
            root / ".qiven" / "runtime" / "receipts");
        auto loaded = journal.load(receipt_id);
        if (!loaded.is_ok())
        {
            std::cout << "activation show: journal error ("
                      << qiven::runtime::cognition::receipt_error_text(loaded.reason())
                      << ")\n";
            return exit_fail;
        }
        if (!loaded.value().has_value())
        {
            std::cout << "activation show: receipt " << receipt_id << " UNKNOWN\n";
            return exit_fail;
        }
        const auto& receipt = *loaded.value();
        std::cout << "  receipt " << receipt.receipt_id << "\n"
                  << "    issued_at_ms " << receipt.issued_at_ms << "\n"
                  << "    bundle " << receipt.facts.bundle_id << "\n"
                  << "    runtime_generation " << receipt.facts.runtime_generation_id << "\n"
                  << "    activation_generation " << receipt.facts.activation_generation << "\n"
                  << "    budget " << receipt.facts.budget_bytes << "\n";
    }
    return exit_ok;
}

int activation_explain(const std::vector<std::string>& args)
{
    std::filesystem::path root;
    std::string receipt_id;
    if (const auto given = flag_value(args, "--root"); !given.empty())
    {
        root = given;
    }
    if (const auto given = flag_value(args, "--id"); !given.empty())
    {
        receipt_id = given.string();
    }
    if (root.empty() || receipt_id.empty())
    {
        std::cout << "activation explain: --root and --id are required\n";
        return exit_usage;
    }
    const qiven::runtime::cognition::ActivationReceiptJournal journal(
        root / ".qiven" / "runtime" / "receipts");
    auto loaded = journal.load(receipt_id);
    if (!loaded.is_ok() || !loaded.value().has_value())
    {
        std::cout << "activation explain: receipt " << receipt_id << " unavailable\n";
        return exit_fail;
    }
    const auto& receipt = *loaded.value();
    // the canonical facts ARE the explanation (every axis an activation
    // bound; the receipt explains itself deterministically)
    std::cout << "activation explain: " << receipt.receipt_id << "\n"
              << receipt.canonical_json() << "\n";
    return exit_ok;
}

int activation_verify(const std::vector<std::string>& args)
{
    std::filesystem::path root;
    std::string receipt_id;
    if (const auto given = flag_value(args, "--root"); !given.empty())
    {
        root = given;
    }
    if (const auto given = flag_value(args, "--id"); !given.empty())
    {
        receipt_id = given.string();
    }
    if (root.empty() || receipt_id.empty())
    {
        std::cout << "activation verify-receipt: --root and --id are required\n";
        return exit_usage;
    }
    const qiven::runtime::cognition::ActivationReceiptJournal journal(
        root / ".qiven" / "runtime" / "receipts");
    auto loaded = journal.load(receipt_id);
    if (!loaded.is_ok() || !loaded.value().has_value())
    {
        std::cout << "activation verify-receipt: receipt " << receipt_id << " unavailable\n";
        return exit_fail;
    }
    const auto& receipt = *loaded.value();

    // verify against the LIVE activated state: the ACTIVE generation and
    // its manifest are the current facts (task/bundle axes verified at
    // consume time by the service; this is the state-axis check).
    qiven::runtime::cognition::ReceiptFacts current = receipt.facts;
    const qiven::runtime::cognition::ActivationIndexBuilder index_builder;
    const std::string active =
        index_builder.active_generation(root / ".qiven" / "runtime");
    if (active.empty())
    {
        std::cout << "activation verify-receipt: NO ACTIVE generation\n";
        return exit_fail;
    }
    current.activation_generation = active;
    const auto manifest           = read_file_text(root / ".qiven" / "runtime" / "activation-generations" /
                                                   active / "index-manifest.json");
    if (!manifest)
    {
        // Fail-closed (same class as the activate path): an unreadable
        // ACTIVE manifest must never let verification fall back to the
        // receipt's own axes - a broken sidecar cannot verify itself.
        std::cout << "activation verify-receipt: ACTIVE manifest unreadable (generation "
                  << active << ")\n";
        return exit_fail;
    }
    {
        const std::string needle = "\"external_source_lock_sha256\":\"";
        const auto at            = manifest->find(needle);
        if (at == std::string::npos)
        {
            std::cout << "activation verify-receipt: ACTIVE manifest carries no source-lock "
                         "digest\n";
            return exit_fail;
        }
        const auto begin = at + needle.size();
        const auto end   = manifest->find('"', begin);
        if (end == std::string::npos)
        {
            std::cout << "activation verify-receipt: ACTIVE manifest source-lock digest "
                         "malformed\n";
            return exit_fail;
        }
        current.external_source_lock_sha256 = manifest->substr(begin, end - begin);
    }
    const auto verdict = qiven::runtime::cognition::verify_receipt(
        receipt, current, now_ms_epoch());
    std::cout << "activation verify-receipt: "
              << qiven::runtime::cognition::receipt_verify_text(verdict) << "\n";
    return verdict == qiven::runtime::cognition::ReceiptVerify::Valid ? exit_ok : exit_fail;
}
} // namespace

int main(int argc, char** argv)
{
    // Headless CRT failure behavior (the 2026-09-19 modal-abort law): a CRT
    // fault terminates with evidence and exit 3143, never modal UI.
    qiven::install_headless_crt_failure_behavior();

    if (argc < 3)
    {
        return usage();
    }
    const std::string object = argv[1];
    const std::string verb   = argv[2];
    const std::vector<std::string> args(argv + 3, argv + argc);
    if (object == "cognition" && verb == "show")
    {
        return cognition_show(args);
    }
    if (object == "cognition" && verb == "activate")
    {
        return cognition_activate(args);
    }
    if (object == "activation" && verb == "show")
    {
        return activation_show(args);
    }
    if (object == "activation" && verb == "explain")
    {
        return activation_explain(args);
    }
    if (object == "activation" && verb == "verify-receipt")
    {
        return activation_verify(args);
    }
    if (object == "index" && verb == "rebuild")
    {
        return index_rebuild(args);
    }
    if (object == "index" && verb == "shadow-compare")
    {
        return index_shadow_compare(args);
    }
    if (object == "index" && verb == "status")
    {
        return index_status(args);
    }
    if (object == "profile" && verb == "show")
    {
        return profile_show(args);
    }
    if (object == "status" && verb == "show")
    {
        return ipc_show(args, false);
    }
    if (object == "doctor" && verb == "show")
    {
        return ipc_show(args, true);
    }
    if (object == "host" && verb == "shutdown")
    {
        return host_shutdown(args);
    }
    if (object == "host" && verb == "refresh")
    {
        return host_refresh(args);
    }
    return usage();
}
