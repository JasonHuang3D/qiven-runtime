#pragma once

// ============================================================================
// ipc/named_pipe_server.hpp — the owner-scoped local transport
// (MVP-3 batch design section 3.3; ARCH section 12.1/12.3; cpp-design §10;
// host-server redesign LL-1/LL-3 — docs/design/mvp4-host-server.md)
//
// One flat-named pipe per installation with an owner-only DACL (SDDL
// "D:P(A;;GA;;;OW)"). Client identity is validated from the CONNECTION,
// never the payload: GetNamedPipeClientProcessId → OpenProcess →
// QueryFullProcessImageNameW against the install record
// (.qiven/runtime/clients.json, written at first boot; every connect is
// checked). The 256-bit installation secret is minted once and persisted
// via DPAPI CurrentUser (.qiven/runtime/client.secret.dpapi).
//
// Host-server redesign concurrency model (LL-3): instances and client
// handles are FILE_FLAG_OVERLAPPED; every wait is slice-quantized against
// an optional stop flag, so no read, write, or accept can block a thread
// indefinitely and a stop is observed within one slice WITHOUT any
// cross-thread handle close (the handle-reuse hazard) or CancelSynchronousIo
// (the design names it; sliced overlapped waits meet the same contract
// with a strictly smaller surface — recorded in the design's batch-b
// implementation notes). ServeLoop (ipc/serve_loop.hpp) owns the LISTEN
// POOL: several concurrently-armed accept instances, one thread per
// accepted connection, a serve-thread cap with the typed busy frame, a
// never-fatal accept policy, and the phased stop.
// ============================================================================

#include <qiven/result.hpp>
#include <qiven/runtime/auth.hpp>
#include <qiven/types.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace qiven::runtime::ipc
{
inline constexpr i32 err_host_singleton = 101;
// The connection-cap busy code (host-server redesign LL-3; the hook client
// is taught this code in classify_transport_failure).
inline constexpr i32 err_server_busy = 125;

// The DACL shape is part of the contract (pinned by the security test):
// owner-only generic-all, no other trustees.
[[nodiscard]] std::wstring owner_only_sddl();
[[nodiscard]] std::wstring pipe_name(std::string_view install_id);

// DPAPI-protected installation secret: mint once, load thereafter.
class InstallationSecret
{
public:
    // Loads <runtime_root>/client.secret.dpapi, or mints + protects a new
    // 256-bit secret when absent (first boot).
    [[nodiscard]] static qiven::Result<auth::SecretKey> ensure(
        const std::filesystem::path& runtime_root);
};

// The allowed client image set (install record). First boot at a runtime
// root records the given images; later loads fail closed on a malformed
// or unreadable record rather than admitting nothing silently.
class ClientRecord
{
public:
    [[nodiscard]] static qiven::Result<std::vector<std::string>> ensure(
        const std::filesystem::path& runtime_root, const std::vector<std::string>& first_boot);

    // Validate one connected pipe client by its OS identity.
    [[nodiscard]] static bool image_allowed(const std::vector<std::string>& allowed,
                                            const std::string& image_path);
};

// One served connection: a connected OVERLAPPED pipe handle with bounded
// frame IO. Reads and writes are overlapped operations waited in slices:
// a read deadline bounds the COMPLETE frame; a write deadline bounds the
// complete write; an optional stop flag aborts the wait promptly (the
// pending operation is cancelled — no partial-data ambiguity: a frame
// that cannot complete inside its bounds closes the connection).
class PipeConnection
{
public:
    explicit PipeConnection(void* handle) noexcept; // HANDLE, owning
    ~PipeConnection();
    PipeConnection(PipeConnection&& other) noexcept;
    PipeConnection& operator=(PipeConnection&& other) noexcept;
    PipeConnection(const PipeConnection&)            = delete;
    PipeConnection& operator=(const PipeConnection&) = delete;

    // Reads ONE frame's bytes (bounded by max_frame_bytes; short read or
    // disconnect returns an empty optional).
    [[nodiscard]] std::optional<std::string> read_frame();
    // Deadline form: the timeout bounds the COMPLETE frame (header + MAC +
    // body) under one deadline. The wait is sliced against `stop`: when the
    // flag is observed the pending read is cancelled and an empty optional
    // returns with last_read_aborted() true (distinct from a deadline
    // expiry so the caller can classify a stop vs an idle close).
    [[nodiscard]] std::optional<std::string> read_frame(u64 timeout_ms,
                                                        const std::atomic<bool>* stop = nullptr);
    // Deadline-bounded write (hygiene bound, LL-3: a non-reading peer
    // cannot hold the serve thread forever).
    [[nodiscard]] bool write_bytes(std::string_view bytes, u64 deadline_ms);

    // True when the most recent read_frame(timeout_ms) returned empty
    // BECAUSE the deadline expired (vs a disconnect/short read).
    [[nodiscard]] bool last_read_timed_out() const noexcept
    {
        return m_last_read_timed_out;
    }
    // True when the most recent read was aborted by the stop flag.
    [[nodiscard]] bool last_read_aborted() const noexcept
    {
        return m_last_read_aborted;
    }

    // The connected client's image path (QueryFullProcessImageNameW).
    [[nodiscard]] qiven::Result<std::string> client_image() const;

    // Connectivity probe that CONSUMES NOTHING (PeekNamedPipe): true while
    // the peer is still connected, false once it closes/disconnects or the
    // handle is gone. Used by the error-frame linger (pipe_service).
    [[nodiscard]] bool peer_connected() const noexcept;

    [[nodiscard]] bool valid() const noexcept
    {
        return m_handle != nullptr;
    }

private:
    void* m_handle             = nullptr;
    bool m_last_read_timed_out = false;
    bool m_last_read_aborted   = false;
};

// TEST-SURFACE adapter (the production serve path is ServeLoop,
// ipc/serve_loop.hpp): a raw single-instance server for the DACL shape
// check, the FIRST_PIPE_INSTANCE singleton assertion, and single-
// connection library drivers. Its accept() blocks on ONE armed instance
// at a time — the retired serial shape, retained as an explicitly-labeled
// test surface.
class NamedPipeServer
{
public:
    // Creates the FIRST instance with the owner-only DACL. A denied
    // FIRST_PIPE_INSTANCE means another host owns the pipe (typed 101).
    [[nodiscard]] static qiven::Result<NamedPipeServer> create(std::string_view install_id);

    // Blocks until one client connects (synchronous accept).
    [[nodiscard]] qiven::Result<PipeConnection> accept();

    ~NamedPipeServer();
    NamedPipeServer(NamedPipeServer&& other) noexcept;
    NamedPipeServer& operator=(NamedPipeServer&& other) noexcept;
    NamedPipeServer(const NamedPipeServer&)            = delete;
    NamedPipeServer& operator=(const NamedPipeServer&) = delete;

    // The DACL actually applied, as SDDL text (for the security test and
    // doctor); empty on failure.
    [[nodiscard]] std::wstring applied_sddl() const;

private:
    NamedPipeServer(void* handle, std::wstring name) noexcept;
    void* m_handle = nullptr;
    std::wstring m_name;
};

// Client side (same Windows user; runtimectl, the hook, and tests).
class PipeClient
{
public:
    // Overlapped connect with WaitNamedPipe etiquette (LL-3): on
    // ERROR_PIPE_BUSY (all instances momentarily mid-handshake) the client
    // waits bounded by `busy_wait_ms` for an instance to re-arm and
    // retries — transport-level etiquette, never a verdict retry. 120 is
    // classified only when no instance arms within the budget.
    [[nodiscard]] static qiven::Result<PipeClient> connect(const std::wstring& name,
                                                           u64 busy_wait_ms = 1000);

    ~PipeClient();
    PipeClient(PipeClient&& other) noexcept;
    PipeClient& operator=(PipeClient&& other) noexcept;
    PipeClient(const PipeClient&)            = delete;
    PipeClient& operator=(const PipeClient&) = delete;

    [[nodiscard]] bool write_bytes(std::string_view bytes, u64 deadline_ms);
    [[nodiscard]] std::optional<std::string> read_frame();
    // Deadline form (client side): the caller's reply budget. Expiry returns
    // an empty optional with last_read_timed_out() true — the timeout class
    // is diagnosable, not folded into "no reply" (2026-09-24 taxonomy split).
    [[nodiscard]] std::optional<std::string> read_frame(u64 timeout_ms);
    [[nodiscard]] bool last_read_timed_out() const noexcept
    {
        return m_last_read_timed_out;
    }

private:
    explicit PipeClient(void* handle) noexcept;
    void* m_handle             = nullptr;
    bool m_last_read_timed_out = false;
};
} // namespace qiven::runtime::ipc
