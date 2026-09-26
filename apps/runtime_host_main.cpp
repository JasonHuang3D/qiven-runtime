// ============================================================================
// apps/runtime_host_main.cpp -- qiven-runtime-host, the production
// composition root entrypoint (MVP-3; ARCH section 6.1; host-server
// redesign LL-1/LL-3 — docs/design/mvp4-host-server.md)
//
// The LONG-LIVED server: ServeLoop owns the listen pool (concurrently
// armed instances, thread-per-connection, typed 125 beyond the cap,
// never-fatal accept policy, phased stop); the RuntimeHost owns the
// state mutex, first-contact session minting, and the host-autonomous
// refresh worker. This exe wires them together, prints the staged boot,
// and exits only on operator stop (Ctrl+C, console close, logoff/
// shutdown, authenticated `host shutdown`) or boot-class failure.
//
// --log <file> runs the server in BACKGROUND mode: every staged marker
// and heartbeat line APPENDS to the file (the kit's autostart shape);
// without it the console output is interactive as before.
//
// Exit codes: 0 clean shutdown, 1 boot failure, 2 usage.
// ============================================================================

#include <qiven/runtime/host/runtime_host.hpp>
#include <qiven/runtime/ipc/framing.hpp>
#include <qiven/runtime/ipc/named_pipe_server.hpp>
#include <qiven/runtime/ipc/pipe_service.hpp>
#include <qiven/runtime/ipc/protocol.hpp>
#include <qiven/runtime/ipc/serve_loop.hpp>
#include <qiven/types.hpp>

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace
{
volatile BOOL g_stop = FALSE;
// The console events (Ctrl+C / close / logoff / shutdown) must reach the
// serve loop: the handler cannot capture, so the main flow publishes the
// loop pointer here BEFORE the loop starts serving (a handler that only
// sets g_stop while nobody reads it would be a dead stop path).
qiven::runtime::ipc::ServeLoop* g_loop = nullptr;

BOOL WINAPI console_handler(DWORD type)
{
    // The long-lived model's DOMINANT termination source is logoff/reboot
    // (a Startup-launched server) — those events drain here too; the
    // unclean-kill backstop is the boot recovery walk (MVP-1 law).
    if (type == CTRL_C_EVENT || type == CTRL_CLOSE_EVENT || type == CTRL_BREAK_EVENT ||
        type == CTRL_LOGOFF_EVENT || type == CTRL_SHUTDOWN_EVENT)
    {
        g_stop = TRUE;
        if (g_loop != nullptr)
        {
            g_loop->request_stop(); // phased stop: arms + serve threads drain
        }
        return TRUE;
    }
    return FALSE;
}

int usage()
{
    std::cerr << "usage: qiven-runtime-host [--root <qiven-context checkout>] "
                 "[--profile <file>] [--git <git.exe>] [--build-id <id>] [--log <file>]\n";
    return 2;
}
} // namespace

int main(int argc, char** argv)
{
    std::filesystem::path repo_root = std::filesystem::current_path();
    std::filesystem::path profile; // set by --profile, else derived from the
                                   // RESOLVED root below (never from CWD)
    std::filesystem::path git = "C:/Program Files/Git/cmd/git.exe";
    std::string build_id      = "qiven-runtime-host-mvp3";
    std::filesystem::path log_file;
    bool profile_explicit = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::string flag = argv[i];
        if (flag == "--help" || flag == "-h")
        {
            std::cout << "qiven-runtime-host (host-server redesign)\n";
            return 0;
        }
        if (i + 1 >= argc)
        {
            std::cerr << "flag '" << flag << "' requires a value\n";
            return usage(); // a trailing lone flag is a usage error, never a silent boot
        }
        if (flag == "--root")
        {
            repo_root = argv[++i];
        }
        else if (flag == "--profile")
        {
            profile          = argv[++i];
            profile_explicit = true;
        }
        else if (flag == "--git")
        {
            git = argv[++i];
        }
        else if (flag == "--build-id")
        {
            build_id = argv[++i];
        }
        else if (flag == "--log")
        {
            log_file = argv[++i];
        }
        else
        {
            return usage();
        }
    }
    if (!std::filesystem::exists(repo_root))
    {
        std::cerr << "root checkout not found: " << repo_root.string() << "\n";
        return usage();
    }
    // The default profile follows the RESOLVED root, never the caller's
    // working directory (2024-09-24 MVP-4 preflight incident: --root
    // re-pointed the governed root while the profile stayed CWD-derived,
    // so an owner double-click in the kit folder resolved a profile that
    // did not exist). An explicit --profile overrides everything.
    if (!profile_explicit)
    {
        profile = repo_root / "config" / "profiles" /
                  "zcode-jason-context-record-mvp.yaml";
        if (!std::filesystem::exists(profile))
        {
            std::cerr << "default profile not found under the resolved root: "
                      << profile.string() << "\n";
            return usage();
        }
    }

    // Background mode: append every output line to the log file (the
    // autostart shape); the console path stays unchanged.
    if (!log_file.empty())
    {
        std::error_code ec;
        std::filesystem::create_directories(log_file.parent_path(), ec);
        FILE* redirected = nullptr;
        freopen_s(&redirected, log_file.string().c_str(), "a", stdout);
        freopen_s(&redirected, log_file.string().c_str(), "a", stderr);
    }

    qiven::runtime::host::HostBoot boot;
    boot.repo_root      = repo_root;
    boot.profile_file   = profile;
    boot.git_executable = git;
    boot.build_id       = build_id;
    boot.now_ms         = qiven::runtime::host::wall_now_ms();

    // Human-facing output law (human-facing-executable contract + operator
    // human output law): staged markers, every durable path the boot
    // writes, observable state only, and a serving heartbeat — a console
    // (or log tail) must never look dead while healthy.
    const std::filesystem::path runtime_root = repo_root / ".qiven" / "runtime";
    std::printf("[ RUN] qiven-runtime-host boot\n");
    std::printf("       root     : %s\n", repo_root.string().c_str());
    std::printf("       profile  : %s\n", profile.string().c_str());
    std::printf("       state at : %s\n",
                (runtime_root).string().c_str()); // journal.sqlite3, bundles/, install.id
    if (!log_file.empty())
    {
        std::printf("       log      : %s\n", log_file.string().c_str());
    }
    std::fflush(stdout);

    auto host = qiven::runtime::host::RuntimeHost::boot(boot);
    if (!host.is_ok())
    {
        std::printf("[FAIL] boot: %s\n", host.reason().message.c_str());
        return 1;
    }
    const auto status = host.value()->status();
    if (status.state != "running")
    {
        std::printf("[FAIL] boot: host is not running: %s\n", status.failure_detail.c_str());
        return 1;
    }
    std::printf("[ OK ] boot: install %s, boot epoch %llu, generation %llu\n",
                status.install_id.c_str(), static_cast<unsigned long long>(status.boot_epoch),
                static_cast<unsigned long long>(status.generation));
    std::printf("       cognition bundle: %s (journal events: %llu)\n",
                status.bundle_revision.substr(0, 12).c_str(),
                static_cast<unsigned long long>(status.journal_events));
    std::printf("       refresh  : %s (next due %llu ms)\n",
                status.refresh_state.c_str(),
                static_cast<unsigned long long>(status.next_refresh_due_ms));
    std::printf("       journal   : %s\n",
                (runtime_root / "journal.sqlite3").string().c_str());
    std::printf("       bundles   : %s\n",
                (runtime_root / "bundles").string().c_str());
    std::fflush(stdout);

    SetConsoleCtrlHandler(console_handler, TRUE);

    // The installation secret is shared with same-user clients via DPAPI;
    // the client install record governs connect authorization.
    std::printf("[ RUN] ipc surface\n");
    std::fflush(stdout);
    auto secret = qiven::runtime::ipc::InstallationSecret::ensure(runtime_root);
    if (!secret.is_ok())
    {
        std::cerr << "installation secret unavailable: " << secret.reason().message << "\n";
        return 1;
    }
    wchar_t self_image[MAX_PATH] {};
    GetModuleFileNameW(nullptr, self_image, MAX_PATH);
    std::string self_narrow;
    {
        const int size = WideCharToMultiByte(CP_UTF8, 0, self_image, -1, nullptr, 0, nullptr,
                                             nullptr);
        self_narrow.resize(static_cast<std::size_t>(size > 0 ? size - 1 : 0));
        if (size > 0)
        {
            WideCharToMultiByte(CP_UTF8, 0, self_image, -1, self_narrow.data(), size, nullptr,
                                nullptr);
        }
    }
    // The installation's own-tool allowlist: the host itself plus the
    // sibling clients deployed in the same directory (same installation
    // unit; the owner-only DACL stays the trust boundary). Boot MERGES
    // missing images — the 2024-09-24 preflight found deployments where
    // only the host was recorded and every hook client denied admission.
    std::vector<std::string> install_images { self_narrow };
    {
        const std::filesystem::path host_dir =
            std::filesystem::path(self_narrow).parent_path();
        for (const char* sibling :
             { "qiven-zcode-hook.exe", "qiven-runtimectl.exe", "qiven-adapter-bridge.exe" })
        {
            const std::filesystem::path image = host_dir / sibling;
            if (std::filesystem::exists(image))
            {
                install_images.push_back(image.string());
            }
        }
    }
    auto allowed_clients =
        qiven::runtime::ipc::ClientRecord::ensure(runtime_root, install_images);
    if (!allowed_clients.is_ok())
    {
        std::cerr << "client record unavailable: " << allowed_clients.reason().message << "\n";
        return 1;
    }
    const qiven::runtime::ipc::FrameCodec codec(secret.value());

    // The HOST-AUTONOMOUS refresh worker (LL-2b): boot + cadence +
    // coalesced operator triggers — never on a request path.
    host.value()->start_refresh_worker();
    std::printf("[ OK ] refresh worker started (host-autonomous cadence)\n");
    std::fflush(stdout);

    // The serve loop (LL-1/LL-3): listen pool, thread-per-connection,
    // typed 125 beyond the cap, never-fatal accept, phased stop.
    qiven::runtime::ipc::ServeLoop::Hooks hooks;
    hooks.admit = [&allowed_clients](qiven::runtime::ipc::PipeConnection& connection)
        -> std::string {
        // Client identity from the connection, never the payload.
        auto image = connection.client_image();
        if (!image.is_ok())
        {
            return "client identity unavailable";
        }
        if (!qiven::runtime::ipc::ClientRecord::image_allowed(allowed_clients.value(),
                                                              image.value()))
        {
            return "client image is not in the install record: " + image.value();
        }
        return "";
    };
    hooks.handle = [&](const qiven::runtime::ipc::Request& request)
        -> qiven::runtime::ipc::Reply {
        auto reply = host.value()->handle(request, qiven::runtime::host::wall_now_ms());
        // One observable line per request (kind + outcome) — the
        // human-facing law: healthy silence never looks dead (log or tail).
        std::string request_label = "request";
        if (request.kind == qiven::runtime::ipc::Request::Kind::HookEvent && !request.event.empty())
        {
            request_label = request.event;
        }
        else if (request.kind == qiven::runtime::ipc::Request::Kind::Shutdown)
        {
            request_label = "shutdown";
        }
        else if (request.kind == qiven::runtime::ipc::Request::Kind::Refresh)
        {
            request_label = "refresh";
        }
        std::string outcome_label = "ok";
        if (reply.kind == qiven::runtime::ipc::Reply::Kind::ErrorView)
        {
            outcome_label = "error " + std::to_string(reply.error_code);
        }
        else if (reply.kind == qiven::runtime::ipc::Reply::Kind::HookAck)
        {
            outcome_label = reply.verdict;
        }
        std::printf("[conn] %s -> %s\n", request_label.c_str(), outcome_label.c_str());
        std::fflush(stdout);
        return reply;
    };
    hooks.log = [](std::string_view line) {
        std::printf("%s\n", line.data());
        std::fflush(stdout);
    };

    auto loop = qiven::runtime::ipc::ServeLoop::create(status.install_id, codec,
                                                       qiven::runtime::ipc::ServeLoop::Options {},
                                                       std::move(hooks));
    if (!loop.is_ok())
    {
        std::cerr << "pipe creation failed: " << loop.reason().message << "\n";
        return 1;
    }

    g_loop = loop.value().get();
    // g_stop (a console event that fired before the loop existed) also stops it.
    if (g_stop)
    {
        loop.value()->request_stop();
    }
    std::printf("[ OK ] ipc: pipe %ls\n",
                qiven::runtime::ipc::pipe_name(status.install_id).c_str());
    std::printf("[ OK ] serving -- Ctrl+C stops the host (drain + journal checkpoint)\n");
    std::fflush(stdout);

    // Serving heartbeat (human-facing law): observable state only. The
    // loop's run() returns when a stop was requested (console event or an
    // authenticated Shutdown observed on a serve thread).
    std::thread heartbeat([&loop, &host, started = std::chrono::steady_clock::now()] {
        qiven::u64 last_beat_s = 0;
        qiven::u64 busy_rejected_at_last_beat =
            loop.value()->stats().busy_rejected.load();
        bool listener_degraded_at_last_beat =
            loop.value()->stats().degraded_listener.load();
        while (!loop.value()->stop_requested() && !g_stop)
        {
            // Sliced sleep: a stop must not wait out a 10 s tick.
            for (int slice = 0; slice < 40 && !loop.value()->stop_requested() && !g_stop;
                 ++slice)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
            if (loop.value()->stop_requested())
            {
                break;
            }
            const auto uptime_s = static_cast<qiven::u64>(
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - started)
                    .count());
            if (uptime_s / 30 > last_beat_s / 30)
            {
                last_beat_s         = uptime_s;
                const auto snapshot = host.value()->status();
                const qiven::u64 busy_total =
                    loop.value()->stats().busy_rejected.load();
                const qiven::u64 busy_occupancy =
                    loop.value()->stats().busy_occupancy_last.load();
                std::string audit_gaps;
                if (snapshot.journal_append_failures > 0)
                {
                    // LL-4: a recording gap is LOUD (status carries the
                    // count; the heartbeat is the human surface).
                    audit_gaps = ", AUDIT GAPS " +
                                 std::to_string(snapshot.journal_append_failures);
                }
                std::printf("[beat] serving %llus, connections %llu, refresh %s, "
                            "listener %s, journal events %llu%s, busy %llu (last occupancy "
                            "%llu)\n",
                            static_cast<unsigned long long>(uptime_s),
                            static_cast<unsigned long long>(
                                loop.value()->stats().connections_served.load()),
                            snapshot.refresh_state.c_str(),
                            loop.value()->stats().degraded_listener.load() ? "DEGRADED"
                                                                           : "ok",
                            static_cast<unsigned long long>(snapshot.journal_events),
                            audit_gaps.c_str(),
                            static_cast<unsigned long long>(busy_total),
                            static_cast<unsigned long long>(busy_occupancy));
                std::fflush(stdout);
                // The 125 audit row (LL-3 S-5 discriminator): the library
                // layer cannot journal, so the embedder makes each busy
                // episode durable here — one row per beat window in which
                // the busy counter moved, carrying the occupancy breakdown.
                if (busy_total > busy_rejected_at_last_beat)
                {
                    busy_rejected_at_last_beat = busy_total;
                    host.value()->record_audit(
                        "connection_cap_busy",
                        "busy_total=" + std::to_string(busy_total) + ";occupancy_last=" +
                            std::to_string(busy_occupancy));
                }
                // Listener degradation is equally durable (§5): the rising
                // edge journals listener_degraded with the recreate count
                // (the same embedder seam; ≤30 s observation latency — a
                // discriminator, not a realtime feed).
                const bool degraded_now = loop.value()->stats().degraded_listener.load();
                if (degraded_now && !listener_degraded_at_last_beat)
                {
                    host.value()->record_audit(
                        "listener_degraded",
                        "accept_recreates=" +
                            std::to_string(
                                loop.value()->stats().accept_recreates.load()));
                }
                listener_degraded_at_last_beat = degraded_now;
            }
        }
    });

    loop.value()->run();
    g_stop = TRUE;
    heartbeat.join();

    host.value()->drain(qiven::runtime::host::wall_now_ms());
    std::printf("[ OK ] stopped cleanly (outstanding observations marked; journal "
                "checkpointed)\n");
    std::fflush(stdout);
    // Exit WITHOUT running local destructors: a straggler serve thread past
    // the grace would race the destruction of the loop/host objects it
    // still touches (use-after-free window); the journal already
    // checkpointed inside drain(). Static/global teardown only.
    std::exit(0);
}
