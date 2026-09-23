#include <qiven/runtime/ipc/named_pipe_server.hpp>

#include <qiven/error.hpp>
#include <qiven/runtime/ipc/framing.hpp>
#include <qiven/runtime/jsonx/json_codec.hpp>

#include <aclapi.h>
#include <sddl.h>
#include <wincrypt.h>
#include <windows.h>

#include <chrono>
#include <fstream>
#include <iterator>
#include <optional>
#include <utility>

namespace qiven::runtime::ipc
{
namespace
{
qiven::Error os_error(i32 code, std::string_view detail)
{
    return qiven::Error::make(qiven::error_category::unavailable, code,
                              "pipe: " + std::string(detail) + " (os error " +
                                  std::to_string(GetLastError()) + ")");
}

bool read_exact(HANDLE handle, char* buffer, usize size)
{
    usize done = 0;
    while (done < size)
    {
        DWORD chunk = 0;
        if (!ReadFile(handle, buffer + done, static_cast<DWORD>(size - done), &chunk, nullptr) ||
            chunk == 0)
        {
            return false;
        }
        done += chunk;
    }
    return true;
}

bool write_all(HANDLE handle, const char* buffer, usize size)
{
    usize done = 0;
    while (done < size)
    {
        DWORD chunk = 0;
        if (!WriteFile(handle, buffer + done, static_cast<DWORD>(size - done), &chunk, nullptr))
        {
            return false;
        }
        done += chunk;
    }
    return true;
}

std::optional<std::string> read_bounded(HANDLE handle)
{
    // Frame layout (framing.cpp): 32-byte header, 32-byte MAC, body.
    // Read the header, validate the body length against the cap, then read
    // MAC + body. Anything oversized drops the connection (the typed
    // denial policy belongs to the host loop).
    std::string header(32, '\0');
    if (!read_exact(handle, header.data(), header.size()))
    {
        return std::nullopt;
    }
    u64 body_len = 0;
    for (int i = 0; i < 8; ++i)
    {
        body_len |= static_cast<u64>(static_cast<unsigned char>(header[8 + i])) << (8 * i);
    }
    if (body_len > max_body_bytes)
    {
        return std::nullopt;
    }
    std::string tail(static_cast<usize>(body_len) + 32, '\0');
    if (!tail.empty() && !read_exact(handle, tail.data(), tail.size()))
    {
        return std::nullopt;
    }
    header.append(tail);
    return header;
}

// Bounded wait for the first incoming bytes (deadline discipline,
// 2026-09-24 corrective lane): polls PeekNamedPipe so a silent client
// cannot wedge a synchronous serve loop, and a slow host is a DIAGNOSABLE
// timeout on the client side rather than an undifferentiated "no reply".
enum class WaitStatus
{
    Readable,
    Broken,
    TimedOut,
};

WaitStatus wait_readable(HANDLE handle, u64 timeout_ms)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (true)
    {
        DWORD available = 0;
        if (!PeekNamedPipe(handle, nullptr, 0, nullptr, &available, nullptr))
        {
            return WaitStatus::Broken; // pipe broken/closing
        }
        if (available > 0)
        {
            return WaitStatus::Readable;
        }
        if (std::chrono::steady_clock::now() >= deadline)
        {
            return WaitStatus::TimedOut;
        }
        Sleep(5);
    }
}
} // namespace

std::wstring owner_only_sddl()
{
    // ACE string: type;flags;rights;object_guid;inherit_guid;sid — the
    // owner-rights trustee (OW) sits in the SIXTH field.
    return L"D:P(A;;GA;;;OW)";
}

std::wstring pipe_name(std::string_view install_id)
{
    // FLAT single-segment name (delta on ARCH section 12.1's
    // "\\.\pipe\qiven-runtime\<install>\v1"): NPFS prunes the intermediate
    // namespace directories of a multi-segment pipe name once no listening
    // instance exists, so next-instance creation at accept() time fails
    // PATH_NOT_FOUND. The flat name preserves every security property —
    // the name is not the boundary; the owner-only DACL is (batch design
    // section 3.3 records this deviation).
    std::wstring wide(install_id.begin(), install_id.end());
    return L"\\\\.\\pipe\\qiven-runtime-" + wide + L"-v1";
}

qiven::Result<auth::SecretKey> InstallationSecret::ensure(const std::filesystem::path& runtime_root)
{
    using SecretResult = qiven::Result<auth::SecretKey>;
    std::filesystem::create_directories(runtime_root);
    const std::filesystem::path blob = runtime_root / "client.secret.dpapi";

    if (std::filesystem::exists(blob))
    {
        std::ifstream in(blob, std::ios::binary);
        std::string protected_bytes((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char> {});
        DATA_BLOB input {};
        input.pbData = reinterpret_cast<BYTE*>(protected_bytes.data());
        input.cbData = static_cast<DWORD>(protected_bytes.size());
        DATA_BLOB output {};
        if (CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, 0, &output))
        {
            auth::SecretKey key {};
            if (output.cbData == key.size())
            {
                memcpy(key.data(), output.pbData, key.size());
            }
            LocalFree(output.pbData);
            if (output.cbData == key.size())
            {
                return SecretResult(key);
            }
        }
        return SecretResult::fail(
            os_error(err_auth, "the DPAPI secret blob is unreadable or truncated"));
    }

    const auth::SecretKey minted = auth::csrandom_secret();
    DATA_BLOB input {};
    input.pbData = const_cast<BYTE*>(reinterpret_cast<const BYTE*>(minted.data()));
    input.cbData = static_cast<DWORD>(minted.size());
    DATA_BLOB output {};
    if (!CryptProtectData(&input, L"qiven-runtime-client-secret", nullptr, nullptr, nullptr, 0,
                          &output))
    {
        return SecretResult::fail(os_error(err_auth, "CryptProtectData failed"));
    }
    std::ofstream out(blob, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(output.pbData),
              static_cast<std::streamsize>(output.cbData));
    LocalFree(output.pbData);
    if (!out)
    {
        return SecretResult::fail(os_error(err_auth, "secret blob write failed"));
    }
    return SecretResult(minted);
}

qiven::Result<std::vector<std::string>> ClientRecord::ensure(
    const std::filesystem::path& runtime_root, const std::vector<std::string>& first_boot)
{
    using RecordResult = qiven::Result<std::vector<std::string>>;
    namespace jsonx    = qiven::runtime::jsonx;
    std::filesystem::create_directories(runtime_root);
    const std::filesystem::path file = runtime_root / "clients.json";

    std::vector<std::string> images;
    if (std::filesystem::exists(file))
    {
        std::ifstream in(file, std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(in)),
                          std::istreambuf_iterator<char> {});
        auto document = jsonx::parse(bytes);
        if (document.is_ok() && document.value().kind == jsonx::JsonValue::Kind::Object)
        {
            const auto* entries = document.value().find("clients");
            if (entries != nullptr && entries->kind == jsonx::JsonValue::Kind::Array)
            {
                for (const auto& entry : entries->array)
                {
                    if (entry.kind == jsonx::JsonValue::Kind::String && !entry.string_value.empty())
                    {
                        images.push_back(entry.string_value);
                    }
                }
            }
        }
        if (images.empty())
        {
            return RecordResult::fail(
                os_error(err_auth, "the client install record is malformed"));
        }
        return RecordResult(std::move(images));
    }

    jsonx::JsonObject object;
    std::vector<jsonx::JsonValue> entries;
    for (const auto& image : first_boot)
    {
        entries.push_back(jsonx::JsonValue::make_string(image));
    }
    object.emplace_back("clients", jsonx::JsonValue::make_array(std::move(entries)));
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << jsonx::write(jsonx::JsonValue::make_object(std::move(object)));
    if (!out)
    {
        return RecordResult::fail(os_error(err_auth, "client record write failed"));
    }
    return RecordResult(first_boot);
}

bool ClientRecord::image_allowed(const std::vector<std::string>& allowed,
                                 const std::string& image_path)
{
    for (const auto& entry : allowed)
    {
        // Case-insensitive path compare (Windows filesystem semantics).
        if (entry.size() == image_path.size())
        {
            bool same = true;
            for (usize i = 0; i < entry.size(); ++i)
            {
                if (std::tolower(static_cast<unsigned char>(entry[i])) !=
                    std::tolower(static_cast<unsigned char>(image_path[i])))
                {
                    same = false;
                    break;
                }
            }
            if (same)
            {
                return true;
            }
        }
    }
    return false;
}

// --- PipeConnection ---------------------------------------------------------

PipeConnection::PipeConnection(void* handle) noexcept :
m_handle(handle)
{
}

PipeConnection::~PipeConnection()
{
    if (m_handle != nullptr)
    {
        DisconnectNamedPipe(static_cast<HANDLE>(m_handle));
        CloseHandle(static_cast<HANDLE>(m_handle));
    }
}

PipeConnection::PipeConnection(PipeConnection&& other) noexcept :
m_handle(std::exchange(other.m_handle, nullptr))
{
}

PipeConnection& PipeConnection::operator=(PipeConnection&& other) noexcept
{
    if (this != &other)
    {
        if (m_handle != nullptr)
        {
            CloseHandle(static_cast<HANDLE>(m_handle));
        }
        m_handle = std::exchange(other.m_handle, nullptr);
    }
    return *this;
}

std::optional<std::string> PipeConnection::read_frame()
{
    if (m_handle == nullptr)
    {
        return std::nullopt;
    }
    m_last_read_timed_out = false;
    return read_bounded(static_cast<HANDLE>(m_handle));
}

std::optional<std::string> PipeConnection::read_frame(u64 timeout_ms)
{
    if (m_handle == nullptr)
    {
        return std::nullopt;
    }
    m_last_read_timed_out = false;
    switch (wait_readable(static_cast<HANDLE>(m_handle), timeout_ms))
    {
    case WaitStatus::Readable:
        break;
    case WaitStatus::TimedOut:
        m_last_read_timed_out = true;
        return std::nullopt;
    case WaitStatus::Broken:
    default:
        return std::nullopt;
    }
    return read_bounded(static_cast<HANDLE>(m_handle));
}

bool PipeConnection::write_bytes(std::string_view bytes)
{
    if (m_handle == nullptr)
    {
        return false;
    }
    return write_all(static_cast<HANDLE>(m_handle), bytes.data(), bytes.size());
}

bool PipeConnection::peer_connected() const noexcept
{
    if (m_handle == nullptr)
    {
        return false;
    }
    DWORD available = 0;
    return PeekNamedPipe(static_cast<HANDLE>(m_handle), nullptr, 0, nullptr, &available,
                         nullptr) !=
           FALSE;
}

qiven::Result<std::string> PipeConnection::client_image() const
{
    using ImageResult = qiven::Result<std::string>;
    ULONG pid         = 0;
    if (m_handle == nullptr ||
        !GetNamedPipeClientProcessId(static_cast<HANDLE>(m_handle), &pid))
    {
        return ImageResult::fail(os_error(err_auth, "client pid unavailable"));
    }
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr)
    {
        return ImageResult::fail(os_error(err_auth, "client process cannot be opened"));
    }
    wchar_t path[MAX_PATH] {};
    DWORD size = MAX_PATH;
    std::string image;
    if (QueryFullProcessImageNameW(process, 0, path, &size))
    {
        const int narrow = WideCharToMultiByte(CP_UTF8, 0, path, size, nullptr, 0, nullptr, nullptr);
        if (narrow > 0)
        {
            image.resize(static_cast<usize>(narrow));
            WideCharToMultiByte(CP_UTF8, 0, path, size, image.data(), narrow, nullptr, nullptr);
        }
    }
    CloseHandle(process);
    if (image.empty())
    {
        return ImageResult::fail(os_error(err_auth, "client image query failed"));
    }
    return ImageResult(std::move(image));
}

// --- NamedPipeServer --------------------------------------------------------

NamedPipeServer::NamedPipeServer(void* handle, std::wstring name) noexcept :
m_handle(handle),
m_name(std::move(name))
{
}

NamedPipeServer::~NamedPipeServer()
{
    if (m_handle != nullptr)
    {
        CloseHandle(static_cast<HANDLE>(m_handle));
    }
}

NamedPipeServer::NamedPipeServer(NamedPipeServer&& other) noexcept :
m_handle(std::exchange(other.m_handle, nullptr)),
m_name(std::move(other.m_name))
{
}

NamedPipeServer& NamedPipeServer::operator=(NamedPipeServer&& other) noexcept
{
    if (this != &other)
    {
        if (m_handle != nullptr)
        {
            CloseHandle(static_cast<HANDLE>(m_handle));
        }
        m_handle = std::exchange(other.m_handle, nullptr);
        m_name   = std::move(other.m_name);
    }
    return *this;
}

qiven::Result<NamedPipeServer> NamedPipeServer::create(std::string_view install_id)
{
    using ServerResult              = qiven::Result<NamedPipeServer>;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            owner_only_sddl().c_str(), SDDL_REVISION_1, &descriptor, nullptr))
    {
        return ServerResult::fail(os_error(err_auth, "owner-only DACL construction failed"));
    }
    SECURITY_ATTRIBUTES attributes { sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE };

    // FILE_FLAG_FIRST_PIPE_INSTANCE asserts ownership: a second host's
    // create() is denied here (the OS-level singleton half; the named
    // mutex fires first in practice). Later accept() instances omit the
    // flag but keep the DACL.
    const std::wstring name = pipe_name(install_id);
    HANDLE handle           = CreateNamedPipeW(
        name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        PIPE_UNLIMITED_INSTANCES, 64 * 1024, 64 * 1024, 0, &attributes);
    LocalFree(descriptor);
    if (handle == INVALID_HANDLE_VALUE)
    {
        const DWORD error = GetLastError();
        if (error == ERROR_ACCESS_DENIED || error == ERROR_PIPE_BUSY)
        {
            return ServerResult::fail(os_error(
                err_host_singleton, "the pipe already exists (another host owns it)"));
        }
        return ServerResult::fail(os_error(err_auth, "pipe creation failed"));
    }
    return ServerResult(NamedPipeServer(handle, name));
}

qiven::Result<PipeConnection> NamedPipeServer::accept()
{
    using AcceptResult = qiven::Result<PipeConnection>;
    if (m_handle == nullptr)
    {
        return AcceptResult::fail(os_error(err_frame, "server is closed"));
    }

    // Stand up the NEXT listen instance first (same owner-only DACL, no
    // FIRST flag), so the connected instance can be handed to the caller
    // wholesale — its handle, its connection, no disconnect mid-service.
    HANDLE next = nullptr;
    {
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                owner_only_sddl().c_str(), SDDL_REVISION_1, &descriptor, nullptr))
        {
            return AcceptResult::fail(os_error(err_auth, "owner-only DACL construction failed"));
        }
        SECURITY_ATTRIBUTES attributes { sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE };
        next = CreateNamedPipeW(
            m_name.c_str(), PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            PIPE_UNLIMITED_INSTANCES, 64 * 1024, 64 * 1024, 0, &attributes);
        LocalFree(descriptor);
        if (next == INVALID_HANDLE_VALUE)
        {
            return AcceptResult::fail(os_error(err_frame, "next pipe instance creation failed"));
        }
    }

    if (!ConnectNamedPipe(static_cast<HANDLE>(m_handle), nullptr) &&
        GetLastError() != ERROR_PIPE_CONNECTED)
    {
        CloseHandle(next);
        return AcceptResult::fail(os_error(err_frame, "accept failed"));
    }

    // The connected instance becomes the connection; the server keeps
    // listening on the fresh instance.
    void* connected = m_handle;
    m_handle        = next;
    return AcceptResult(PipeConnection(connected));
}

std::wstring NamedPipeServer::applied_sddl() const
{
    if (m_handle == nullptr)
    {
        return {};
    }
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    BOOL owner_defaulted            = FALSE;
    PACL dacl                       = nullptr;
    BOOL dacl_defaulted             = FALSE;
    if (GetSecurityInfo(static_cast<HANDLE>(m_handle), SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION,
                        nullptr, nullptr, &dacl, nullptr, &descriptor) != ERROR_SUCCESS)
    {
        return {};
    }
    (void)owner_defaulted;
    (void)dacl_defaulted;
    LPWSTR sddl = nullptr;
    std::wstring out;
    if (ConvertSecurityDescriptorToStringSecurityDescriptorW(descriptor, SDDL_REVISION_1,
                                                             DACL_SECURITY_INFORMATION, &sddl,
                                                             nullptr))
    {
        out.assign(sddl);
        LocalFree(sddl);
    }
    LocalFree(descriptor);
    return out;
}

// --- PipeClient -------------------------------------------------------------

PipeClient::PipeClient(void* handle) noexcept :
m_handle(handle)
{
}

PipeClient::~PipeClient()
{
    if (m_handle != nullptr)
    {
        CloseHandle(static_cast<HANDLE>(m_handle));
    }
}

PipeClient::PipeClient(PipeClient&& other) noexcept :
m_handle(std::exchange(other.m_handle, nullptr))
{
}

PipeClient& PipeClient::operator=(PipeClient&& other) noexcept
{
    if (this != &other)
    {
        if (m_handle != nullptr)
        {
            CloseHandle(static_cast<HANDLE>(m_handle));
        }
        m_handle = std::exchange(other.m_handle, nullptr);
    }
    return *this;
}

qiven::Result<PipeClient> PipeClient::connect(const std::wstring& name)
{
    using ClientResult = qiven::Result<PipeClient>;
    HANDLE handle      = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        return ClientResult::fail(os_error(err_frame, "pipe connect failed"));
    }
    DWORD mode = PIPE_READMODE_BYTE;
    SetNamedPipeHandleState(handle, &mode, nullptr, nullptr);
    return ClientResult(PipeClient(handle));
}

bool PipeClient::write_bytes(std::string_view bytes)
{
    if (m_handle == nullptr)
    {
        return false;
    }
    return write_all(static_cast<HANDLE>(m_handle), bytes.data(), bytes.size());
}

std::optional<std::string> PipeClient::read_frame()
{
    if (m_handle == nullptr)
    {
        return std::nullopt;
    }
    m_last_read_timed_out = false;
    return read_bounded(static_cast<HANDLE>(m_handle));
}

std::optional<std::string> PipeClient::read_frame(u64 timeout_ms)
{
    if (m_handle == nullptr)
    {
        return std::nullopt;
    }
    m_last_read_timed_out = false;
    switch (wait_readable(static_cast<HANDLE>(m_handle), timeout_ms))
    {
    case WaitStatus::Readable:
        break;
    case WaitStatus::TimedOut:
        m_last_read_timed_out = true;
        return std::nullopt;
    case WaitStatus::Broken:
    default:
        return std::nullopt;
    }
    return read_bounded(static_cast<HANDLE>(m_handle));
}
} // namespace qiven::runtime::ipc
