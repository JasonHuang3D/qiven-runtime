#pragma once

// ============================================================================
// ipc/named_pipe_server.hpp — the owner-scoped local transport
// (MVP-3 batch design section 3.3; ARCH section 12.1/12.3; cpp-design §10)
//
// One named pipe per installation: \\.\pipe\qiven-runtime\<install-id>\v1
// with an owner-only DACL (SDDL "D:P(A;;GA;;OW)" — nobody else may connect;
// exit gate 1's locally-provable part). Client identity is validated from
// the CONNECTION, never the payload: GetNamedPipeClientProcessId →
// OpenProcess → QueryFullProcessImageNameW against the install record
// (.qiven/runtime/clients.json, written at first boot; every connect is
// checked). The 256-bit installation secret is minted once and persisted
// via DPAPI CurrentUser (.qiven/runtime/client.secret.dpapi).
//
// Synchronous single-connection service model: the host accepts and serves
// one client at a time on its control thread (GR-4; H-3 records the
// concurrency deferral). One connection may carry a BOUNDED SEQUENCE of
// frames (ipc/pipe_service.hpp, 2026-09-24 corrective decision): the wire
// contract has always declared connection_seq "strictly increasing per
// connection". Reads and writes are bounded by the frame caps; frame
// ARRIVAL is bounded by the read deadline (a silent client cannot wedge
// the serve loop).
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

// One served connection: a connected pipe handle with bounded frame IO.
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
    // Deadline form: waits at most timeout_ms for the frame's FIRST bytes
    // (PeekNamedPipe poll); expiry returns an empty optional with
    // last_read_timed_out() true. A stall mid-frame after arrival keeps the
    // blocking semantics (owner-scoped clients; see pipe_service.hpp).
    [[nodiscard]] std::optional<std::string> read_frame(u64 timeout_ms);
    [[nodiscard]] bool write_bytes(std::string_view bytes);

    // True when the most recent read_frame(timeout_ms) returned empty
    // BECAUSE the deadline expired (vs a disconnect/short read).
    [[nodiscard]] bool last_read_timed_out() const noexcept
    {
        return m_last_read_timed_out;
    }

    // The connected client's image path (QueryFullProcessImageNameW).
    [[nodiscard]] qiven::Result<std::string> client_image() const;

    [[nodiscard]] bool valid() const noexcept
    {
        return m_handle != nullptr;
    }

private:
    void* m_handle             = nullptr;
    bool m_last_read_timed_out = false;
};

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

// Client side (same Windows user; runtimectl and tests).
class PipeClient
{
public:
    [[nodiscard]] static qiven::Result<PipeClient> connect(const std::wstring& name);

    ~PipeClient();
    PipeClient(PipeClient&& other) noexcept;
    PipeClient& operator=(PipeClient&& other) noexcept;
    PipeClient(const PipeClient&)            = delete;
    PipeClient& operator=(const PipeClient&) = delete;

    [[nodiscard]] bool write_bytes(std::string_view bytes);
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
