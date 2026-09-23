// ============================================================================
// apps/runtimectl_main.cpp — qiven-runtimectl, the read-only operational
// client (ARCH section 6.4; MVP-2 local surface + MVP-3 IPC surface)
//
//   qiven-runtimectl cognition show [--root <checkout>]
//   qiven-runtimectl profile show  [--profile <file>]
//   qiven-runtimectl status show    [--root <checkout>]   (authenticated IPC)
//   qiven-runtimectl doctor show    [--root <checkout>]   (authenticated IPC)
//
// An unreachable or uninstalled host is reported as such (exit 1) — never
// as "governed" (the ARCH section 15 MVP-4 row 5 discipline, applied
// early). Exit codes: 0 pass, 1 failure, 2 usage.
// ============================================================================

#include <qiven/runtime/cognition/bundle.hpp>
#include <qiven/runtime/host/deployment_profile.hpp>
#include <qiven/runtime/ipc/framing.hpp>
#include <qiven/runtime/ipc/named_pipe_server.hpp>
#include <qiven/runtime/ipc/protocol.hpp>

#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace
{
constexpr int exit_ok    = 0;
constexpr int exit_fail  = 1;
constexpr int exit_usage = 2;

int usage()
{
    std::cerr << "usage: qiven-runtimectl cognition show [--root <qiven-context checkout>]\n"
              << "       qiven-runtimectl profile show [--profile <profile file>]\n"
              << "       qiven-runtimectl status show [--root <qiven-context checkout>]\n"
              << "       qiven-runtimectl doctor show [--root <qiven-context checkout>]\n"
              << "       qiven-runtimectl host shutdown [--root <qiven-context checkout>] "
                 "[--grace-ms N]\n";
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

// One authenticated round-trip over the installation pipe.
qiven::Result<qiven::runtime::ipc::Reply> transact(
    const std::filesystem::path& runtime_root, const qiven::runtime::ipc::Request& request)
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
            header, qiven::runtime::ipc::encode_request_body(request))))
    {
        return ReplyResult::fail(
            qiven::Error::make(qiven::error_category::unavailable, 65, "request write failed"));
    }
    auto frame = client.value().read_frame();
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
    request.kind        = qiven::runtime::ipc::Request::Kind::Shutdown;
    request.request_id  = 1;
    request.grace_ms    = grace_ms;
    request.deadline_ms = 3000;
    auto reply          = transact(repo_root / ".qiven" / "runtime", request);
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
} // namespace

int main(int argc, char** argv)
{
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
    return usage();
}
