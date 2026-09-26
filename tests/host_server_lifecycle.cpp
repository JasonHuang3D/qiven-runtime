// ============================================================================
// tests/host_server_lifecycle.cpp — REAL-EXE lifecycle laws of the
// host-server redesign (docs/design/mvp4-host-server.md §7)
//
// Spawns the production qiven-runtime-host.exe against a scratch governed
// root and asserts the PROCESS-level properties the handle()-level suite
// cannot see: the long-lived server boots and serves; FIRST-CONTACT
// pre_tool verdicts need no registration ritual (LL-2a); session_start is
// FAST (no refresh in the request path, LL-2b); the host-autonomous
// refresh worker is observable (§6); concurrent connects all receive
// verdicts (listen pool, LL-3); an authenticated shutdown exits the real
// process and a restart resumes with no ritual (LL-4).
//
// The test drives the REAL sibling client executables (qiven-zcode-hook,
// qiven-runtimectl — same installation unit, admitted by boot's sibling
// merge), exactly as the kit does. The 125/stalled-peer/serve-loop rows
// run at LIBRARY level in ipc_multiframe_contract.cpp (ServeLoop is a
// unit there); this file owns the process boundary.
// ============================================================================

#include <qiven/runtime/cognition/bundle.hpp>
#include <qiven/runtime/journal/runtime_journal.hpp>
#include <qiven/types.hpp>

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#define QIVEN_VERIFY(expr)                                                         \
    do                                                                             \
    {                                                                              \
        if (!(expr))                                                               \
        {                                                                          \
            std::fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            std::exit(1);                                                          \
        }                                                                          \
    } while (false)

namespace
{
int run_command(const std::string& file, const std::vector<std::string>& args,
                const std::string& stdin_text, int timeout_s, std::string* out = nullptr)
{
    std::string command = "\"" + file + "\"";
    for (const auto& arg : args)
    {
        command += " " + arg;
    }
    SECURITY_ATTRIBUTES inheritable { sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
    HANDLE in_read  = nullptr;
    HANDLE in_write = nullptr;
    CreatePipe(&in_read, &in_write, &inheritable, 0);
    HANDLE out_read  = nullptr;
    HANDLE out_write = nullptr;
    CreatePipe(&out_read, &out_write, &inheritable, 0);
    // ONLY the std ends may be inherited: the child holding its own
    // stdin's WRITE end never sees EOF (the hook would block forever in
    // read_stdin_verbatim — found live in the first run of this test).
    SetHandleInformation(in_write, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si {};
    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdInput  = in_read;
    si.hStdOutput = out_write;
    si.hStdError  = out_write;
    PROCESS_INFORMATION pi {};
    std::vector<char> mutable_command(command.begin(), command.end());
    mutable_command.push_back('\0');
    const BOOL started = CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(in_read);
    CloseHandle(out_write);
    if (!started)
    {
        CloseHandle(in_write);
        CloseHandle(out_read);
        return -1;
    }
    if (!stdin_text.empty())
    {
        DWORD written = 0;
        WriteFile(in_write, stdin_text.data(), static_cast<DWORD>(stdin_text.size()), &written,
                  nullptr);
    }
    CloseHandle(in_write);
    std::string captured;
    char buffer[4096];
    DWORD got           = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s);
    bool pipe_open      = true;
    while (std::chrono::steady_clock::now() < deadline)
    {
        while (pipe_open && PeekNamedPipe(out_read, nullptr, 0, nullptr, &got, nullptr) && got > 0)
        {
            if (!ReadFile(out_read, buffer, sizeof(buffer), &got, nullptr) || got == 0)
            {
                pipe_open = false;
                break;
            }
            captured.append(buffer, buffer + got);
        }
        if (WaitForSingleObject(pi.hProcess, 200) == WAIT_OBJECT_0)
        {
            break;
        }
    }
    while (pipe_open && PeekNamedPipe(out_read, nullptr, 0, nullptr, &got, nullptr) && got > 0)
    {
        if (!ReadFile(out_read, buffer, sizeof(buffer), &got, nullptr) || got == 0)
        {
            break;
        }
        captured.append(buffer, buffer + got);
    }
    DWORD exit_code = STILL_ACTIVE;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(out_read);
    if (out != nullptr)
    {
        *out = captured;
    }
    return exit_code == STILL_ACTIVE ? -2 : static_cast<int>(exit_code);
}

void write_file(const std::filesystem::path& path, const std::string& text)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

int git(const std::filesystem::path& repo, const std::vector<std::string>& args)
{
    std::vector<std::string> full = { "-C", repo.string() };
    full.insert(full.end(), args.begin(), args.end());
    return run_command("git", full, "", 60);
}

struct Procs
{
    std::filesystem::path host;
    std::filesystem::path hook;
    std::filesystem::path ctl;
};

// One hook invocation against the scratch root: exit code (+ optional
// stderr capture). The payload is the pinned-harness camelCase shape.
int run_hook(const Procs& procs, const std::filesystem::path& root, const char* event,
             const char* tool, const char* session, std::string* err = nullptr)
{
    std::vector<std::string> args = { "--event", event, "--root", root.string(), "--session-handle",
                                      session };
    if (tool != nullptr && tool[0] != '\0')
    {
        args.push_back("--tool");
        args.push_back(tool);
    }
    std::string payload = "{\"session_id\":\"" + std::string(session) +
                          "\",\"cwd\":\"D:/nowhere\",\"timestamp\":\"2026-09-27T00:00:00Z\"}";
    if (tool != nullptr && tool[0] != '\0')
    {
        payload = "{\"session_id\":\"" + std::string(session) +
                  "\",\"tool_name\":\"" + tool + "\",\"tool_input\":{\"command\":\"echo ok\"},"
                                                 "\"cwd\":\"D:/nowhere\",\"timestamp\":\"2026-09-27T00:00:00Z\"}";
    }
    return run_command(procs.hook.string(), args, payload, 30, err);
}

// Wall-clock ms for journal read-opens: the journal rejects a now_ms before
// its last written event (D-8 monotonicity), so reads carry wall time.
qiven::u64 journal_wall_now_ms() noexcept
{
    return static_cast<qiven::u64>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

// Read-only scalar over the scratch journal (WAL mode: safe concurrent with
// a live host process; the host connection never blocks a reader).
qiven::i64 journal_scalar(const std::filesystem::path& journal_file, const char* sql)
{
    auto db = qiven::runtime::journal::JournalDb::open(
        journal_file, qiven::runtime::journal::JournalOpenIntent::OpenExisting,
        journal_wall_now_ms());
    QIVEN_VERIFY(db.is_ok());
    auto stmt = db.value().prepare(sql);
    QIVEN_VERIFY(stmt.is_ok());
    auto row = stmt.value().step();
    QIVEN_VERIFY(row.is_ok() && row.value());
    return stmt.value().col_i64(0);
}
} // namespace

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    const std::filesystem::path build_bin =
        std::filesystem::path(QIVEN_RUNTIME_BIN_DIR) / "qiven-runtime-host.exe";
    Procs procs;
    procs.host = build_bin;
    procs.hook = build_bin.parent_path() / "qiven-zcode-hook.exe";
    procs.ctl  = build_bin.parent_path() / "qiven-runtimectl.exe";
    QIVEN_VERIFY(std::filesystem::exists(procs.host));
    QIVEN_VERIFY(std::filesystem::exists(procs.hook));
    QIVEN_VERIFY(std::filesystem::exists(procs.ctl));

    // Scratch governed root: minimal git fixture + profile revision 3
    // (offline: no origin remote, so the refresh worker's attempts fall
    // back to the verified local bundle inside the window — deterministic).
    // UNIQUE scratch root per run: a leftover host from a killed test run
    // holds its root's singleton + pipe forever (the long-lived model), and
    // a reused path would race it. Uniqueness makes every run independent.
    const auto run_stamp                 = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path case_dir = std::filesystem::path(QIVEN_RUNTIME_TEST_WORKROOT) /
                                           ("host-server-lifecycle-" +
                                            std::to_string(GetCurrentProcessId()) + "-" +
                                            std::to_string(run_stamp));
    std::error_code ec;
    std::filesystem::remove_all(case_dir, ec);
    const std::filesystem::path repo = case_dir / "repo";
    std::filesystem::create_directories(repo);
    write_file(repo / "runtime" / "invocation-policy.yaml",
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
               "  unsatisfied_before_execution: deny\n");
    write_file(repo / "memory" / "index.yaml", "schema_version: 1\nrecords: []\n");
    QIVEN_VERIFY(git(repo, { "init", "-q", "--initial-branch=main" }) == 0);
    QIVEN_VERIFY(git(repo, { "config", "user.email", "test@qiven.invalid" }) == 0);
    QIVEN_VERIFY(git(repo, { "config", "user.name", "Qiven Test" }) == 0);
    QIVEN_VERIFY(git(repo, { "add", "-A" }) == 0);
    QIVEN_VERIFY(git(repo, { "commit", "-q", "-m", "fixture" }) == 0);

    // The host's default profile lives under <root>/config/profiles; the
    // scratch root carries its own revision-3 instance. The policy pin is
    // the REAL digest of the fixture policy (boot verifies it).
    std::string policy_hex;
    {
        const std::string policy_text = std::string("schema: qiven-invocation-policy-v1\n") +
                                        "version: 1\n" +
                                        "policy:\n" +
                                        "  present: true\n" +
                                        "  rules:\n" +
                                        "    - action: Commit\n" +
                                        "      requirement: RunMechanicalCheck\n" +
                                        "      boundary: BeforeExecution\n" +
                                        "      subject: attribution lint\n" +
                                        "      blocking: true\n" +
                                        "resolvers:\n" +
                                        "  - requirement: RunMechanicalCheck\n" +
                                        "    type: mechanical_check_runner\n" +
                                        "    min_version: 1\n" +
                                        "freshness:\n" +
                                        "  evidence_ttl_ms: 900000\n" +
                                        "  bundle_freshness_ms: 604800000\n" +
                                        "enforcement:\n" +
                                        "  unsatisfied_before_judgment: redeliberate\n" +
                                        "  unsatisfied_before_execution: deny\n";
        const auto digest = qiven::runtime::cognition::digest_of(policy_text);
        policy_hex        = qiven::runtime::cognition::hex_lower(
            std::span<const std::byte>(digest.sha256.data(), digest.sha256.size()));
    }
    write_file(repo / "config" / "profiles" / "zcode-jason-context-record-mvp.yaml",
               "schema: qiven-deployment-profile-v1\n"
               "profile_id: host-server-lifecycle-test\n"
               "revision: 3\n"
               "control_version: 1\n"
               "resolver_registry_revision: 1\n"
               "classifier_contract_revision: 1\n"
               "conformance_evidence: tests/host_server_lifecycle.cpp\n"
               "freshness_window_ms: 604800000\n"
               "refresh_interval_ms: 3600000\n"
               "governed_paths:\n"
               "  - state\n"
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
                   "    - memory/index.yaml\n");

    const std::filesystem::path host_log = case_dir / "host.log";

    // 1. Boot the long-lived server (background, log to file) and wait for
    // the pipe to answer (bounded wait, no polling past the budget).
    auto server = [&] {
        std::string command = "\"" + procs.host.string() + "\" --root " + repo.string() +
                              " --log " + host_log.string();
        SECURITY_ATTRIBUTES inheritable { sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
        STARTUPINFOA si {};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi {};
        std::vector<char> mutable_command(command.begin(), command.end());
        mutable_command.push_back('\0');
        const BOOL started = CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr,
                                            FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        QIVEN_VERIFY(started);
        CloseHandle(pi.hThread);
        return pi.hProcess;
    }();

    const auto boot_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    bool booted              = false;
    std::string status_text;
    while (std::chrono::steady_clock::now() < boot_deadline)
    {
        const int code = run_command(procs.ctl.string(),
                                     { "status", "show", "--root", repo.string() }, "", 20,
                                     &status_text);
        if (code == 0)
        {
            booted = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    QIVEN_VERIFY(booted);
    std::printf("[ OK ] boot: long-lived server answering status\n");

    // 2. FIRST-CONTACT pre_tool (no session_start ever): a real verdict.
    {
        std::string err;
        const int code = run_hook(procs, repo, "pre_tool", "Bash", "hsl-first-contact", &err);
        QIVEN_VERIFY(code == 0); // not_governed allow is silent, exit 0
        std::printf("[ OK ] first contact: pre_tool verdict with no registration\n");
    }

    // 3. session_start is FAST: no refresh in the request path (LL-2b).
    {
        const auto begin = std::chrono::steady_clock::now();
        const int code   = run_hook(procs, repo, "session_start", "", "hsl-reg");
        const auto ms    = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - begin)
                            .count();
        QIVEN_VERIFY(code == 0);
        QIVEN_VERIFY(ms < 2000); // expected single-digit; bound far under the 9750 class
        std::printf("[ OK ] session_start round trip in %lld ms (< 2000)\n",
                    static_cast<long long>(ms));
    }

    // 4. The host-autonomous refresh worker is observable: on the offline
    // fixture the first attempt (2 s after boot) fails its fetch and falls
    // back to the verified local bundle — status reports the state.
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
        bool observed       = false;
        std::string text;
        while (std::chrono::steady_clock::now() < deadline)
        {
            const int code = run_command(procs.ctl.string(),
                                         { "status", "show", "--root", repo.string() }, "", 20,
                                         &text);
            if (code == 0 && text.find("refresh_state   : local_fallback") != std::string::npos)
            {
                observed = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        QIVEN_VERIFY(observed);
        std::printf("[ OK ] refresh worker: local_fallback observed in status\n");
    }

    // 5. Concurrent connects all receive verdicts (listen pool, LL-3):
    // six simultaneous hook clients, no ERROR_PIPE_BUSY -> 120 denials.
    {
        std::vector<int> codes(6, -3);
        std::vector<std::thread> spawners;
        for (int i = 0; i < 6; ++i)
        {
            spawners.emplace_back([&, i] {
                codes[static_cast<std::size_t>(i)] =
                    run_hook(procs, repo, "pre_tool", "Bash",
                             ("hsl-concurrent-" + std::to_string(i)).c_str());
            });
        }
        for (auto& spawner : spawners)
        {
            spawner.join();
        }
        for (const int code : codes)
        {
            QIVEN_VERIFY(code == 0);
        }
        std::printf("[ OK ] concurrent connects: 6/6 simultaneous clients served\n");
    }

    // 5b. Verdict DURING a triggered refresh (lock discipline, §5 two-level):
    // trigger the worker, then hook verdicts while the attempt runs.
    {
        (void)run_command(procs.ctl.string(),
                          { "host", "refresh", "--root", repo.string() }, "", 20);
        const auto begin = std::chrono::steady_clock::now();
        std::string err;
        const int code = run_hook(procs, repo, "pre_tool", "Bash", "hsl-during-refresh", &err);
        const auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - begin)
                            .count();
        QIVEN_VERIFY(code == 0);
        QIVEN_VERIFY(ms < 2000); // verdict never waits behind refresh work
        std::printf("[ OK ] verdict during refresh trigger in %lld ms (< 2000)\n",
                    static_cast<long long>(ms));
    }

    // 5c. Outstanding pre observation + the restart row's session baseline:
    // a not_governed allow records the outstanding correlation (no post_tool
    // is ever sent for it), and the session count is the minting baseline
    // the restart leg below asserts its +1 against.
    qiven::i64 sessions_before_restart = 0;
    {
        std::string err;
        const int code = run_hook(procs, repo, "pre_tool", "Bash", "hsl-outstanding", &err);
        QIVEN_VERIFY(code == 0);
        const std::filesystem::path journal_file =
            repo / ".qiven" / "runtime" / "journal.sqlite3";
        QIVEN_VERIFY(std::filesystem::exists(journal_file));
        sessions_before_restart = journal_scalar(journal_file, "SELECT COUNT(*) FROM sessions");
        QIVEN_VERIFY(sessions_before_restart >= 9); // one minted session per handle used above
        QIVEN_VERIFY(journal_scalar(journal_file,
                                    "SELECT COUNT(*) FROM audit_events WHERE kind = "
                                    "'hook_outcome_indeterminate'") == 0);
        std::printf("[ OK ] outstanding pre recorded (sessions so far: %lld)\n",
                    static_cast<long long>(sessions_before_restart));
    }

    // 6. Authenticated shutdown EXITS the real process (bounded grace).
    {
        std::string shutdown_out;
        const int code = run_command(procs.ctl.string(),
                                     { "host", "shutdown", "--root", repo.string() }, "", 20,
                                     &shutdown_out);
        std::printf("[diag] shutdown ctl exit %d: %s\n", code, shutdown_out.c_str());
        QIVEN_VERIFY(code == 0);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        BOOL exited         = FALSE;
        DWORD exit_code     = STILL_ACTIVE;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (WaitForSingleObject(server, 300) == WAIT_OBJECT_0)
            {
                GetExitCodeProcess(server, &exit_code);
                exited = TRUE;
                break;
            }
        }
        QIVEN_VERIFY(exited);
        QIVEN_VERIFY(exit_code == 0);
        CloseHandle(server);
        std::printf("[ OK ] shutdown: process exited cleanly (code 0)\n");
    }

    // 6b. Drain marked the outstanding pre Indeterminate (§5 stop
    // semantics): the audit row is durable in the journal the drain
    // checkpointed before exit.
    {
        const std::filesystem::path journal_file =
            repo / ".qiven" / "runtime" / "journal.sqlite3";
        const qiven::i64 indeterminate_rows =
            journal_scalar(journal_file, "SELECT COUNT(*) FROM audit_events WHERE kind = "
                                         "'hook_outcome_indeterminate'");
        QIVEN_VERIFY(indeterminate_rows >= 1);
        std::printf("[ OK ] drain: %lld outstanding observation(s) marked Indeterminate\n",
                    static_cast<long long>(indeterminate_rows));
    }

    // 7. Restart resumes with no ritual (LL-4): the next verdict works
    // immediately on a fresh process.
    {
        server = [&] {
            std::string command = "\"" + procs.host.string() + "\" --root " + repo.string() +
                                  " --log " + host_log.string();
            STARTUPINFOA si {};
            si.cb = sizeof(si);
            PROCESS_INFORMATION pi {};
            std::vector<char> mutable_command(command.begin(), command.end());
            mutable_command.push_back('\0');
            const BOOL started =
                CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr, FALSE,
                               CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
            QIVEN_VERIFY(started);
            CloseHandle(pi.hThread);
            return pi.hProcess;
        }();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        bool answered       = false;
        while (std::chrono::steady_clock::now() < deadline)
        {
            std::string err;
            if (run_hook(procs, repo, "pre_tool", "Bash", "hsl-after-restart", &err) == 0)
            {
                answered = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        QIVEN_VERIFY(answered);
        std::printf("[ OK ] restart: verdicts resume with no ritual\n");

        // 7a. A handle from BEFORE the restart re-contacts: a FRESH runtime
        // session is minted (the second open_session row; cross-restart
        // correlation is never guessed — §5 restart semantics, the journal
        // row count is the oracle).
        {
            std::string err;
            const int code = run_hook(procs, repo, "pre_tool", "Bash", "hsl-outstanding", &err);
            QIVEN_VERIFY(code == 0);
            const qiven::i64 sessions_after =
                journal_scalar(repo / ".qiven" / "runtime" / "journal.sqlite3",
                               "SELECT COUNT(*) FROM sessions");
            QIVEN_VERIFY(sessions_after == sessions_before_restart + 2);
            std::printf("[ OK ] restart: re-contact minted a fresh session (%lld -> %lld)\n",
                        static_cast<long long>(sessions_before_restart),
                        static_cast<long long>(sessions_after));
        }

        // Leave the scratch server in a clean state.
        (void)run_command(procs.ctl.string(), { "host", "shutdown", "--root", repo.string() }, "",
                          20);
        const auto stop_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (std::chrono::steady_clock::now() < stop_deadline)
        {
            if (WaitForSingleObject(server, 300) == WAIT_OBJECT_0)
            {
                break;
            }
        }
        CloseHandle(server);
    }

    std::printf("[ OK ] host_server_lifecycle: all real-exe laws pass\n");
    return 0;
}
