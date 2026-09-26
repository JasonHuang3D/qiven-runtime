// ============================================================================
// hook_conformance -- the MVP-4 exit-gate rows that are locally provable
// (ARCH section 15 MVP-4 rows 2-5; row 1 -- the real-ZCode deny-no-
// fallthrough -- is the owner-H1 kit, docs/engineering/mvp4-h1-kit.md).
//
// Drives the host's handle() in-process against a REAL workroot (the
// host_lifecycle fixture pattern) plus the pure event extractor and the
// client-side fail-closed mapping.
// ============================================================================

#include <qiven/contracts.hpp>
#include <qiven/runtime/adapter/zcode_event.hpp>
#include <qiven/runtime/adapter/zcode_hook.hpp>
#include <qiven/runtime/cognition/policy.hpp>
#include <qiven/runtime/host/runtime_host.hpp>
#include <qiven/runtime/ipc/protocol.hpp>
#include <qiven/types.hpp>

#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
using qiven::runtime::adapter::extract_zcode_event;
using qiven::runtime::adapter::run_zcode_hook;
using qiven::runtime::host::HostBoot;
using qiven::runtime::host::RuntimeHost;
using qiven::runtime::ipc::Reply;
using qiven::runtime::ipc::Request;

void write_file(const std::filesystem::path& file, const std::string& bytes)
{
    std::filesystem::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << bytes;
}

const char* fixture_policy =
    "schema: qiven-invocation-policy-v1\n"
    "version: 1\n"
    "policy:\n"
    "  present: true\n"
    "  rules:\n"
    "    - action: Commit\n"
    "      requirement: RunMechanicalCheck\n"
    "      boundary: BeforeExecution\n"
    "      subject: attribution lint\n"
    "      blocking: true\n"
    "resolvers:\n"
    "  - requirement: RunMechanicalCheck\n"
    "    type: mechanical_check_runner\n"
    "    min_version: 1\n"
    "freshness:\n"
    "  evidence_ttl_ms: 900000\n"
    "  bundle_freshness_ms: 604800000\n"
    "enforcement:\n"
    "  unsatisfied_before_judgment: redeliberate\n"
    "  unsatisfied_before_execution: deny\n";

bool git(const std::filesystem::path& repo, std::initializer_list<const char*> args)
{
    qiven::runtime::processx::ProcessSpec spec;
    spec.executable = R"(C:\Program Files\Git\cmd\git.exe)";
    spec.argv       = { "git", "-C", repo.string() };
    for (const char* arg : args)
    {
        spec.argv.push_back(arg);
    }
    spec.working_dir = repo;
    qiven::runtime::processx::ProcessRunner runner;
    auto run = runner.run(spec);
    return run.is_ok() && run.value().exit_code == 0;
}

std::string to_bytes_of(const std::string& text)
{
    return text;
}

Request hook_request(const char* event, const char* tool, const char* command,
                     const char* file_path, qiven::u64 deadline_ms = 4500)
{
    // deadline_ms is retained in the signature for call-site stability; the
    // wire carries no deadline (host-server redesign LL-3), so it is inert.
    (void)deadline_ms;
    Request request;
    request.kind           = Request::Kind::HookEvent;
    request.event          = event;
    request.session_handle = "sess-fixture-1";
    request.tool_name      = tool;
    request.payload_sha256 = std::string(64, 'a');
    request.payload_bytes  = 256;
    request.command        = command != nullptr ? command : "";
    request.file_path      = file_path != nullptr ? file_path : "";
    return request;
}
} // namespace

int main()
{
    using qiven::runtime::adapter::hook_reason_bash_reference;
    using qiven::runtime::adapter::hook_reason_correlation;
    using qiven::runtime::adapter::hook_reason_governed_write;
    using qiven::runtime::adapter::hook_reason_host_unavailable;
    using qiven::runtime::adapter::hook_reason_scope_mismatch;
    using qiven::runtime::adapter::hook_reason_unknown_session;
    using qiven::runtime::adapter::hook_reason_unknown_tool;

    // --- extraction table (delta 1.1) ---------------------------------------
    {
        const std::string payload =
            "{\"session_id\":\"s-1\",\"hook_event_name\":\"PreToolUse\",\"tool_name\":"
            "\"Write\",\"cwd\":\"D:/x\",\"tool_input\":{\"file_path\":\"D:/JasonWork/"
            "qiven-context/state/current.md\",\"content\":\"hi\\n\"},\"num\":-3,\"f\":1.5}";
        const auto bytes = std::vector<std::byte>(
            reinterpret_cast<const std::byte*>(payload.data()),
            reinterpret_cast<const std::byte*>(payload.data()) + payload.size());
        auto fields = extract_zcode_event(bytes);
        QIVEN_VERIFY(fields.is_ok());
        QIVEN_VERIFY(fields.value().session_handle == "s-1");
        QIVEN_VERIFY(fields.value().event_name == "PreToolUse");
        QIVEN_VERIFY(fields.value().tool_name == "Write");
        QIVEN_VERIFY(fields.value().file_path.find("state/current.md") != std::string::npos);
        QIVEN_VERIFY(fields.value().payload_bytes == payload.size());
        // signed/float values in unneeded fields did not break extraction.
    }
    {
        // Deny-118 incident regression (2026-09-23): the REAL ZCode payload
        // carries NO session identity field -- extraction must SUCCEED with
        // an empty handle (identity comes from the registration template),
        // and the digest still binds the verbatim bytes.
        const std::string payload =
            "{\"tool_input\":{\"command\":\"echo hi\"},\"unrelated\":-1.5}";
        const auto bytes = std::vector<std::byte>(
            reinterpret_cast<const std::byte*>(payload.data()),
            reinterpret_cast<const std::byte*>(payload.data()) + payload.size());
        auto fields = extract_zcode_event(bytes);
        QIVEN_VERIFY(fields.is_ok());
        QIVEN_VERIFY(fields.value().session_handle.empty());
        QIVEN_VERIFY(fields.value().command == "echo hi");
        QIVEN_VERIFY(fields.value().payload_bytes == payload.size());
    }

    // --- client-side fail-closed mapping (exit gate rows 3/5) --------------
    {
        qiven::runtime::adapter::HookRun run;
        run.runtime_root = std::filesystem::temp_directory_path() / "qiven-hook-nohost";
        run.event        = "pre_tool";
        const std::string payload =
            "{\"session_id\":\"s-1\",\"hook_event_name\":\"PreToolUse\",\"tool_name\":\"Bash\","
            "\"tool_input\":{\"command\":\"echo hi\"}}";
        // OWNED payload (trial-2 regression shape: the run holds the bytes;
        // a temporary-owner span once dangled here and fed freed heap to
        // the extractor -- the deny-118 recurrence).
        run.payload.assign(reinterpret_cast<const std::byte*>(payload.data()),
                           reinterpret_cast<const std::byte*>(payload.data()) +
                               payload.size());
        const auto outcome = run_zcode_hook(run);
        QIVEN_VERIFY(outcome.exit_code == 2);
        // 2026-09-24 taxonomy split: an absent host is the DISJOINT
        // no-listener class (120), never the undifferentiated 116 the
        // deny-116 incident made every transport failure wear.
        QIVEN_VERIFY(outcome.stderr_text.find("deny 120") != std::string::npos);
        QIVEN_VERIFY(outcome.stderr_text.find("no host verdict") != std::string::npos);
        // Gate 5 honesty: the text claims NO governed status.
        QIVEN_VERIFY(outcome.stderr_text.find("fail-closed deny") != std::string::npos);
    }

    // --- host fixture: real workroot, real journal, profile revision 2 -----
    const std::filesystem::path workroot = QIVEN_RUNTIME_TEST_WORKROOT;
    const std::filesystem::path case_dir = workroot / "hook-conformance";
    std::error_code ec;
    std::filesystem::remove_all(case_dir, ec);
    const std::filesystem::path repo = case_dir / "repo";
    std::filesystem::create_directories(repo);
    write_file(repo / "runtime" / "invocation-policy.yaml", fixture_policy);
    write_file(repo / "memory" / "index.yaml", "schema_version: 1\nrecords: []\n");
    QIVEN_VERIFY(git(repo, { "init", "-q", "--initial-branch=main" }));
    QIVEN_VERIFY(git(repo, { "config", "user.email", "test@qiven.invalid" }));
    QIVEN_VERIFY(git(repo, { "config", "user.name", "Qiven Test" }));
    QIVEN_VERIFY(git(repo, { "add", "-A" }));
    QIVEN_VERIFY(git(repo, { "commit", "-q", "-m", "fixture" }));

    std::string policy_hex;
    {
        const auto digest = qiven::runtime::cognition::digest_of(fixture_policy);
        policy_hex        = qiven::runtime::cognition::hex_lower(digest.sha256);
    }
    const std::filesystem::path profile_file = case_dir / "profile.yaml";
    write_file(
        profile_file,
        "schema: qiven-deployment-profile-v1\n"
        "profile_id: hook-conformance-test\n"
        "revision: 3\n"
        "control_version: 1\n"
        "resolver_registry_revision: 1\n"
        "classifier_contract_revision: 1\n"
        "conformance_evidence: evidence/audits/hook-conformance.md\n"
        "freshness_window_ms: 604800000\n"
        "refresh_interval_ms: 3600000\n"
        "governed_paths:\n"
        "  - state\n"
        "  - memory/records\n"
        "actors:\n"
        "  - adapter: 1\n"
        "    session_token: 1\n"
        "    credential_token: 1\n"
        "capabilities:\n"
        "  - id: 1\n"
        "    adapter: 1\n"
        "    operation_class: FileSystemWrite\n"
        "    can_mutate_world: true\n"
        "    can_publish_claims: false\n"
        "    requires_execution_authority: true\n"
        "mediation:\n"
        "  - capability: 1\n"
        "    kind: ActionInterception\n"
        "claims:\n"
        "  tool_mediated: true\n"
        "  free_text: false\n"
        "tool_inventory:\n"
        "  - tool: Bash\n"
        "    capability: 1\n"
        "    extraction: command\n"
        "    detector: conservative_text_reference\n"
        "  - tool: Write\n"
        "    capability: 1\n"
        "    extraction: file_path\n"
        "    detector: exact_path\n"
        "  - tool: Edit\n"
        "    capability: 1\n"
        "    extraction: file_path\n"
        "    detector: exact_path\n"
        "record_launcher: qiven-record --request-file <path> | --stdin\n"
        "cognition:\n"
        "  repository: https://example.invalid/qiven-context.git\n"
        "  authorized_ref: refs/heads/main\n"
        "  policy_path: runtime/invocation-policy.yaml\n"
        "  policy_sha256: " +
            policy_hex +
            "\n"
            "  source_paths:\n"
            "    - runtime/invocation-policy.yaml\n"
            "    - memory/index.yaml\n"
            "git_executable: C:/Program Files/Git/cmd/git.exe\n"
            "git_min_version: 2.40\n");

    HostBoot boot;
    boot.repo_root      = repo;
    boot.profile_file   = profile_file;
    boot.git_executable = R"(C:\Program Files\Git\cmd\git.exe)";
    boot.build_id       = "hook-conformance-test-build";
    boot.now_ms         = 7'000'000;
    auto host           = RuntimeHost::boot(boot);
    QIVEN_VERIFY(host.is_ok());
    QIVEN_VERIFY(host.value()->status().state == "running");
    const qiven::u64 now = 7'000'100;

    // --- session registration + idempotent identity (gate 4) ---------------
    std::string session_id;
    {
        Request start        = hook_request("session_start", "", nullptr, nullptr, 9000);
        start.mediated_tools = "Bash,Write,Edit";
        auto ack             = host.value()->handle(start, now);
        if (ack.kind == Reply::Kind::ErrorView)
        {
            std::fprintf(stderr, "session_start error: %d: %s\n", ack.error_code,
                         ack.error_detail.c_str());
        }
        QIVEN_VERIFY(ack.kind == Reply::Kind::HookAck);
        QIVEN_VERIFY(ack.verdict == "allow" || ack.verdict == "degraded");
        QIVEN_VERIFY(!ack.session_id.empty());
        session_id = ack.session_id;
        // Re-start acks the SAME identity (hooks are one-shot; ZCode may
        // re-fire SessionStart).
        auto again = host.value()->handle(start, now + 1);
        QIVEN_VERIFY(again.session_id == session_id);
    }

    // --- mediation matrix (gate 2) ------------------------------------------
    {
        // Write into a governed prefix: deny 110.
        auto ack = host.value()->handle(
            hook_request("pre_tool", "Write", nullptr, "D:/JasonWork/ctx/state/current.md"),
            now + 10);
        if (ack.kind == Reply::Kind::ErrorView)
        {
            std::fprintf(stderr, "pre_tool error: %d: %s\n", ack.error_code,
                         ack.error_detail.c_str());
        }
        QIVEN_VERIFY(ack.kind == Reply::Kind::HookAck);
        QIVEN_VERIFY(ack.verdict == "deny");
        QIVEN_VERIFY(ack.reason_code == hook_reason_governed_write);
        QIVEN_VERIFY(!ack.action_id.empty());
    }
    {
        // Edit with traversal form: deny (no bypass through traversal).
        auto ack = host.value()->handle(
            hook_request("pre_tool", "Edit", nullptr, "D:/x/../state/current.md"), now + 20);
        QIVEN_VERIFY(ack.verdict == "deny");
    }
    {
        // Bash referencing the governed scope: deny 111 (conservative).
        auto ack = host.value()->handle(
            hook_request("pre_tool", "Bash", "grep foo STATE/whatever", nullptr), now + 30);
        QIVEN_VERIFY(ack.verdict == "deny");
        QIVEN_VERIFY(ack.reason_code == hook_reason_bash_reference);
    }
    {
        // Clean command outside the scope: not_governed.
        auto ack = host.value()->handle(
            hook_request("pre_tool", "Bash", "echo hello world", nullptr), now + 40);
        QIVEN_VERIFY(ack.verdict == "not_governed");
        const std::string first_action = ack.action_id;
        QIVEN_VERIFY(!first_action.empty());

        // Repeated identical actions receive UNIQUE ids (gate 4).
        auto repeat = host.value()->handle(
            hook_request("post_tool", "Bash", "echo hello world", nullptr), now + 41);
        QIVEN_VERIFY(repeat.verdict == "allow"); // outcome correlated + closed
        auto second = host.value()->handle(
            hook_request("pre_tool", "Bash", "echo hello world", nullptr), now + 42);
        QIVEN_VERIFY(second.verdict == "not_governed");
        QIVEN_VERIFY(!second.action_id.empty() && second.action_id != first_action);
    }
    {
        // Ungoverned Write target: not_governed; governed memory path: deny.
        auto ok_write = host.value()->handle(
            hook_request("pre_tool", "Write", nullptr, "D:/JasonWork/scratch/notes.txt"),
            now + 50);
        QIVEN_VERIFY(ok_write.verdict == "not_governed");
        (void)host.value()->handle(hook_request("post_tool", "Write", nullptr,
                                                "D:/JasonWork/scratch/notes.txt"),
                                   now + 51);
        auto denied = host.value()->handle(
            hook_request("pre_tool", "Write", nullptr, "D:/ctx/memory/records/MEM-1.md"),
            now + 52);
        QIVEN_VERIFY(denied.verdict == "deny");
        QIVEN_VERIFY(denied.reason_code == hook_reason_governed_write);
    }

    // --- unknown tools fail closed (gate 3) ---------------------------------
    {
        auto ack = host.value()->handle(hook_request("pre_tool", "WebFetch", nullptr, nullptr),
                                        now + 60);
        QIVEN_VERIFY(ack.verdict == "deny");
        QIVEN_VERIFY(ack.reason_code == hook_reason_unknown_tool);
    }

    // --- first contact mints (LL-2a): the deny-114 class is RETIRED -------
    {
        // An unregistered handle's pre_tool mints the session and is judged
        // on its merits (the governed target below still denies 110 — the
        // verdict is REAL, never a registration denial).
        Request request        = hook_request("pre_tool", "Write", nullptr, "D:/y/state/x.md");
        request.session_handle = "sess-never-registered";
        auto ack               = host.value()->handle(request, now + 70);
        QIVEN_VERIFY(ack.verdict == "deny");
        QIVEN_VERIFY(ack.reason_code == hook_reason_governed_write);
        QIVEN_VERIFY(!ack.session_id.empty()); // minted on first contact
    }
    {
        // Duplicate post (no outstanding pre): Indeterminate, typed 115.
        auto ack = host.value()->handle(hook_request("post_tool", "Edit", nullptr,
                                                     "D:/z/README.md"),
                                        now + 80);
        QIVEN_VERIFY(ack.verdict == "degraded");
        QIVEN_VERIFY(ack.reason_code == hook_reason_correlation);
    }

    // --- first contact is sufficient (LL-2a): deny-114 is retired ----------
    {
        // A pre_tool on a NEVER-registered handle mints the session and
        // returns a real verdict — never a 114-class deny.
        Request first        = hook_request("pre_tool", "Bash", "echo clean", nullptr);
        first.session_handle = "sess-first-contact";
        auto ack             = host.value()->handle(first, now + 20);
        QIVEN_VERIFY(ack.kind == Reply::Kind::HookAck);
        QIVEN_VERIFY(ack.verdict == "not_governed");
        QIVEN_VERIFY(ack.reason_code != 114);
        QIVEN_VERIFY(!ack.session_id.empty());
        // Full pre/post flow with NO session_start at all: identical shape.
        Request pre        = hook_request("pre_tool", "Bash", "echo independent", nullptr);
        pre.session_handle = "sess-no-registration";
        auto pre_ack       = host.value()->handle(pre, now + 21);
        QIVEN_VERIFY(pre_ack.verdict == "not_governed");
        Request post        = hook_request("post_tool", "Bash", nullptr, nullptr);
        post.session_handle = "sess-no-registration";
        auto post_ack       = host.value()->handle(post, now + 22);
        QIVEN_VERIFY(post_ack.verdict == "allow"); // outcome correlated
        std::printf("[ OK ] first contact: pre_tool mints; no registration ritual\n");
    }

    // --- manifest advisory is PER-TOOL (LL-2a): 113 scoped ------------------
    {
        Request start        = hook_request("session_start", "", nullptr, nullptr);
        start.session_handle = "sess-partial-manifest";
        start.mediated_tools = "Bash"; // lacks Write/Edit
        auto ack             = host.value()->handle(start, now + 23);
        QIVEN_VERIFY(ack.kind == Reply::Kind::HookAck);
        Request bash        = hook_request("pre_tool", "Bash", "echo fine", nullptr);
        bash.session_handle = "sess-partial-manifest";
        auto bash_ack       = host.value()->handle(bash, now + 24);
        QIVEN_VERIFY(bash_ack.verdict == "not_governed"); // Bash unaffected
        Request edit        = hook_request("pre_tool", "Edit", nullptr, "D:/z/a.md");
        edit.session_handle = "sess-partial-manifest";
        auto edit_ack       = host.value()->handle(edit, now + 25);
        QIVEN_VERIFY(edit_ack.verdict == "deny");
        QIVEN_VERIFY(edit_ack.reason_code == hook_reason_scope_mismatch); // 113 scoped
        std::printf("[ OK ] manifest advisory: 113 only for mismatched tools\n");
    }

    // --- the wire carries no deadline (LL-3) ---------------------------------
    {
        const Request hook     = hook_request("pre_tool", "Bash", "echo wire", nullptr);
        const std::string body = qiven::runtime::ipc::encode_request_body(hook);
        QIVEN_VERIFY(body.find("deadline_ms") == std::string::npos);
        QIVEN_VERIFY(!qiven::runtime::ipc::decode_request(
                          R"({"kind":"status","request_id":1,"deadline_ms":3000})")
                          .is_ok()); // old client shape fails closed typed
        std::printf("[ OK ] wire: no deadline_ms; retired field rejected\n");
    }

    // --- request-path cost bound (LL-2b, timed leg) --------------------------
    {
        const auto begin     = std::chrono::steady_clock::now();
        Request probe        = hook_request("pre_tool", "Bash", "echo fast", nullptr);
        probe.session_handle = "sess-cost-bound";
        auto ack             = host.value()->handle(probe, now + 26);
        const auto elapsed   = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - begin)
                                 .count();
        QIVEN_VERIFY(ack.kind == Reply::Kind::HookAck);
        QIVEN_VERIFY(elapsed < 250); // expected single-digit ms; bound pinned
        std::printf("[ OK ] request-path cost: verdict in %lld ms (< 250)\n",
                    static_cast<long long>(elapsed));
    }

    // --- registry eviction (§5): idle + no outstanding -> fresh mint ---------
    {
        std::string first_id;
        {
            Request fresh        = hook_request("pre_tool", "Bash", "echo one", nullptr);
            fresh.session_handle = "sess-eviction";
            auto ack             = host.value()->handle(fresh, now + 26);
            QIVEN_VERIFY(ack.kind == Reply::Kind::HookAck);
            first_id = ack.session_id;
            QIVEN_VERIFY(!first_id.empty());
            // Clear the outstanding pre (eviction skips sessions with one).
            Request post        = hook_request("post_tool", "Bash", nullptr, nullptr);
            post.session_handle = "sess-eviction";
            (void)host.value()->handle(post, now + 26);
        }
        host.value()->set_session_eviction_bound_for_test(0); // every idle session
        host.value()->evict_idle_sessions_for_test(now + 27); // the sweep itself
        Request fresh        = hook_request("pre_tool", "Bash", "echo evicted", nullptr);
        fresh.session_handle = "sess-eviction"; // already-minted handle
        auto ack             = host.value()->handle(fresh, now + 28);
        QIVEN_VERIFY(ack.kind == Reply::Kind::HookAck);
        QIVEN_VERIFY(ack.session_id != first_id); // FRESH mint after eviction
        std::printf("[ OK ] eviction: re-contact after eviction mints fresh\n");
        host.value()->set_session_eviction_bound_for_test(86400000);
    }

    // --- hook client 125 mapping (§7): the classifier is taught the code ----
    {
        using qiven::runtime::adapter::classify_transport_failure;
        using qiven::runtime::adapter::hook_reason_server_busy;
        const auto error = qiven::Error::make(qiven::error_category::unavailable,
                                              hook_reason_server_busy, "server busy");
        QIVEN_VERIFY(classify_transport_failure(error) == hook_reason_server_busy);
        QIVEN_VERIFY(hook_reason_server_busy == 125);
        std::printf("[ OK ] hook client: 125 maps to the fail-closed busy class\n");
    }

    // --- freshness expiry gates GOVERNED mutations (117, scoped) -----------
    {
        const qiven::u64 far_future =
            host.value()->status().last_refresh_ok_ms + 604800000 + 1000;
        Request governed        = hook_request("pre_tool", "Write", nullptr,
                                               "D:/z/state/current.md");
        governed.session_handle = "sess-expiry";
        auto ack                = host.value()->handle(governed, far_future);
        QIVEN_VERIFY(ack.verdict == "deny");
        QIVEN_VERIFY(ack.reason_code == hook_reason_cognition_expired); // 117
        Request clean        = hook_request("pre_tool", "Bash", "echo fine", nullptr);
        clean.session_handle = "sess-expiry";
        auto clean_ack       = host.value()->handle(clean, far_future);
        QIVEN_VERIFY(clean_ack.verdict == "not_governed"); // telemetry stays up
        std::printf("[ OK ] freshness expiry: governed 117; not_governed unaffected\n");
    }

    // --- H-4 shutdown: ack precedes drain; state drains ---------------------
    {
        Request request;
        request.kind       = Request::Kind::Shutdown;
        request.request_id = 9;
        request.grace_ms   = 1500;
        auto ack           = host.value()->handle(request, now + 90);
        QIVEN_VERIFY(ack.kind == Reply::Kind::ShutdownAck);
        QIVEN_VERIFY(ack.draining);
        QIVEN_VERIFY(host.value()->status().state == "draining");
        // Post-shutdown hook events deny typed (draining).
        auto denied = host.value()->handle(
            hook_request("pre_tool", "Bash", "echo late", nullptr), now + 91);
        QIVEN_VERIFY(denied.kind == Reply::Kind::ErrorView);
    }

    // --- protocol decode: unknown hook fields fail closed (gate 3) ---------
    {
        const std::string body =
            "{\"kind\":\"hook_event\",\"event\":\"pre_tool\",\"session_handle\":\"s\","
            "\"payload_sha256\":\"abc\",\"request_id\":1,\"deadline_ms\":3000,"
            "\"surprise\":true}";
        auto decoded = qiven::runtime::ipc::decode_request(body);
        QIVEN_VERIFY(!decoded.is_ok());
    }

    std::printf("[ OK ] hook_conformance\n");
    return 0;
}
