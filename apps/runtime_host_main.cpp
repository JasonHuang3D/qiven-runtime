// ============================================================================
// apps/runtime_host_main.cpp — qiven-runtime-host, the production
// composition root entrypoint (MVP-3; ARCH section 6.1)
//
// Serves the owner-scoped named pipe: accept → client identity validation
// → frame verify (HMAC/replay) → protocol handle → framed reply. Status
// and doctor serve from boot; mutation kinds deny typed 61 (HostRecovering
// / not-implemented-until-MVP-5) — fail closed, never a hang inside the
// request deadline. Ctrl+C or console close drains and checkpoints.
//
// Exit codes: 0 clean shutdown, 1 boot failure, 2 usage.
// ============================================================================

#include <qiven/runtime/host/runtime_host.hpp>
#include <qiven/runtime/ipc/framing.hpp>
#include <qiven/runtime/ipc/named_pipe_server.hpp>
#include <qiven/runtime/ipc/protocol.hpp>
#include <qiven/types.hpp>

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace
{
volatile BOOL g_stop = FALSE;

BOOL WINAPI console_handler(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_CLOSE_EVENT || type == CTRL_BREAK_EVENT)
    {
        g_stop = TRUE;
        return TRUE;
    }
    return FALSE;
}

int usage()
{
    std::cerr << "usage: qiven-runtime-host [--root <qiven-context checkout>] "
                 "[--profile <file>] [--git <git.exe>] [--build-id <id>]\n";
    return 2;
}
} // namespace

int main(int argc, char** argv)
{
    std::filesystem::path repo_root = std::filesystem::current_path();
    std::filesystem::path profile   = repo_root / "config" / "profiles" /
                                    "zcode-jason-context-record-mvp.yaml";
    std::filesystem::path git = "C:/Program Files/Git/cmd/git.exe";
    std::string build_id      = "qiven-runtime-host-mvp3";
    for (int i = 1; i + 1 < argc; ++i)
    {
        const std::string flag = argv[i];
        if (flag == "--help" || flag == "-h")
        {
            std::cout << "qiven-runtime-host (MVP-3)\n";
            return 0;
        }
        if (flag == "--root")
        {
            repo_root = argv[++i];
        }
        else if (flag == "--profile")
        {
            profile = argv[++i];
        }
        else if (flag == "--git")
        {
            git = argv[++i];
        }
        else if (flag == "--build-id")
        {
            build_id = argv[++i];
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

    qiven::runtime::host::HostBoot boot;
    boot.repo_root      = repo_root;
    boot.profile_file   = profile;
    boot.git_executable = git;
    boot.build_id       = build_id;
    boot.now_ms         = qiven::runtime::host::wall_now_ms();

    auto host = qiven::runtime::host::RuntimeHost::boot(boot);
    if (!host.is_ok())
    {
        std::cerr << "RuntimeHost boot failed: " << host.reason().message << "\n";
        return 1;
    }
    const auto status = host.value()->status();
    if (status.state != "running")
    {
        std::cerr << "RuntimeHost is not running: " << status.failure_detail << "\n";
        return 1;
    }
    std::cout << "[qiven-runtime-host] running: install " << status.install_id << ", generation "
              << status.generation << ", bundle " << status.bundle_revision.substr(0, 12)
              << ", journal events " << status.journal_events << "\n";

    SetConsoleCtrlHandler(console_handler, TRUE);

    // The installation secret is shared with same-user clients via DPAPI;
    // the client install record governs connect authorization.
    const std::filesystem::path runtime_root = repo_root / ".qiven" / "runtime";
    auto secret                              = qiven::runtime::ipc::InstallationSecret::ensure(runtime_root);
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
    auto allowed_clients = qiven::runtime::ipc::ClientRecord::ensure(runtime_root, { self_narrow });
    if (!allowed_clients.is_ok())
    {
        std::cerr << "client record unavailable: " << allowed_clients.reason().message << "\n";
        return 1;
    }
    const qiven::runtime::ipc::FrameCodec codec(secret.value());

    auto server = qiven::runtime::ipc::NamedPipeServer::create(status.install_id);
    if (!server.is_ok())
    {
        std::cerr << "pipe creation failed: " << server.reason().message << "\n";
        return 1;
    }

    qiven::runtime::ipc::ReplayGuard replay;
    replay.note_wall_clock(boot.now_ms);
    while (!g_stop)
    {
        auto connection = server.value().accept();
        if (!connection.is_ok())
        {
            if (g_stop)
            {
                break;
            }
            std::cerr << "accept failed: " << connection.reason().message << "\n";
            break;
        }

        // Client identity from the connection, never the payload.
        auto image = connection.value().client_image();
        if (!image.is_ok() ||
            !qiven::runtime::ipc::ClientRecord::image_allowed(allowed_clients.value(),
                                                              image.value()))
        {
            continue; // drop the connection (typed 62 class; no reply surface)
        }

        auto frame_bytes = connection.value().read_frame();
        if (!frame_bytes.has_value())
        {
            continue;
        }
        auto verified = codec.decode(frame_bytes.value());
        if (!verified.is_ok())
        {
            // Auth/frame failure: typed error frame, then drop.
            const auto error_reply = qiven::runtime::ipc::make_error(
                0, static_cast<qiven::i32>(verified.reason().code), verified.reason().message);
            const std::string body = qiven::runtime::ipc::encode_reply(error_reply);
            connection.value().write_bytes(
                codec.encode(qiven::runtime::ipc::FrameHeader {}, body));
            continue;
        }

        // Replay window: the body's first 16 bytes are the nonce; the
        // timestamp rides the frame request identity (hello carries it in
        // the body; later kinds reuse the established connection state).
        auto request = qiven::runtime::ipc::decode_request(verified.value().body);
        if (!request.is_ok())
        {
            const auto error_reply = qiven::runtime::ipc::make_error(
                verified.value().header.request_id, request.reason().code, request.reason().detail);
            const std::string body = qiven::runtime::ipc::encode_reply(error_reply);
            connection.value().write_bytes(
                codec.encode(qiven::runtime::ipc::FrameHeader {}, body));
            continue;
        }

        auto reply = host.value()->handle(request.value(),
                                          qiven::runtime::host::wall_now_ms());
        qiven::runtime::ipc::FrameHeader reply_header;
        reply_header.request_id     = verified.value().header.request_id;
        reply_header.connection_seq = verified.value().header.connection_seq;
        const std::string body      = qiven::runtime::ipc::encode_reply(reply);
        connection.value().write_bytes(codec.encode(reply_header, body));
    }

    host.value()->request_shutdown();
    std::cout << "[qiven-runtime-host] stopped cleanly\n";
    return 0;
}
