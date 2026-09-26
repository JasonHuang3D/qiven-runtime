#include <qiven/runtime/ipc/named_pipe_server.hpp>
#include <qiven/runtime/ipc/serve_loop.hpp>

#include <qiven/error.hpp>
#include <qiven/runtime/ipc/framing.hpp>
#include <qiven/runtime/jsonx/json_codec.hpp>

#include <aclapi.h>
#include <sddl.h>
#include <wincrypt.h>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iterator>
#include <memory>
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

// --- overlapped primitives (host-server redesign §5 I/O model) ---------------
//
// Instances and client handles are FILE_FLAG_OVERLAPPED. Every operation is
// issued once and its event waited in SLICES: a deadline bounds the whole
// operation; an optional stop flag aborts the wait promptly (the operation
// is cancelled — no cross-thread handle close, no handle-reuse hazard; the
// design's CancelSynchronousIo contract is met with a smaller surface).

struct OverlappedOp
{
    OVERLAPPED ov {};
    HANDLE event = nullptr;

    OverlappedOp()
    {
        event     = CreateEventW(nullptr, TRUE, FALSE, nullptr); // manual reset
        ov.hEvent = event;
    }
    ~OverlappedOp()
    {
        if (event != nullptr)
        {
            CloseHandle(event);
        }
    }
    OverlappedOp(const OverlappedOp&)            = delete;
    OverlappedOp& operator=(const OverlappedOp&) = delete;
};

// Wait one slice; returns 0 = signaled, WAIT_TIMEOUT, or WAIT_FAILED.
DWORD wait_slice(HANDLE event, u64 slice_ms)
{
    return WaitForSingleObject(event, static_cast<DWORD>(
                                          slice_ms > 0xFFFFFFFF ? 0xFFFFFFFF : slice_ms));
}

// Sliced wait with deadline + optional stop. Outcomes: completed / timed_out /
// aborted(stop) / failed.
enum class WaitResult
{
    Completed,
    TimedOut,
    Aborted,
    Failed
};

WaitResult wait_deadline(HANDLE handle, OVERLAPPED& ov, u64 deadline_ms,
                         const std::atomic<bool>* stop, u64 slice_ms)
{
    using clock       = std::chrono::steady_clock;
    const auto begin  = clock::now();
    const auto end_at = begin + std::chrono::milliseconds(deadline_ms);
    while (true)
    {
        if (stop != nullptr && stop->load(std::memory_order_acquire))
        {
            CancelIoEx(handle, &ov);
            // Reap the cancelled operation so the OVERLAPPED is not pending.
            DWORD reaped = 0;
            GetOverlappedResult(handle, &ov, &reaped, TRUE);
            return WaitResult::Aborted;
        }
        const auto now = clock::now();
        if (now >= end_at)
        {
            CancelIoEx(handle, &ov);
            DWORD reaped = 0;
            GetOverlappedResult(handle, &ov, &reaped, TRUE);
            return WaitResult::TimedOut;
        }
        const auto remaining = static_cast<u64>(
            std::chrono::duration_cast<std::chrono::milliseconds>(end_at - now).count());
        const u64 this_slice = (std::min)(remaining, slice_ms);
        const DWORD waited   = wait_slice(ov.hEvent, this_slice);
        if (waited == WAIT_OBJECT_0)
        {
            return WaitResult::Completed;
        }
        if (waited == WAIT_FAILED)
        {
            return WaitResult::Failed;
        }
        // WAIT_TIMEOUT: next loop iteration re-checks stop/deadline.
    }
}

// Deadline-bounded exact READ of `size` bytes (the deadline bounds the
// WHOLE chunk, M1 semantics preserved). Byte-mode pipes return on >=1 byte,
// so the loop accumulates partial completions. timed_out/aborted close the
// connection at the caller level (a frame that cannot complete in bounds).
bool read_exact_ov(HANDLE handle, char* buffer, usize size, u64 deadline_ms,
                   const std::atomic<bool>* stop, u64 slice_ms, bool& timed_out, bool& aborted)
{
    timed_out  = false;
    aborted    = false;
    usize done = 0;
    while (done < size)
    {
        OverlappedOp op;
        op.ov.Offset     = 0;
        op.ov.OffsetHigh = 0;
        DWORD chunk      = 0;
        if (!ReadFile(handle, buffer + done, static_cast<DWORD>(size - done), &chunk, &op.ov))
        {
            if (GetLastError() != ERROR_IO_PENDING)
            {
                return false;
            }
        }
        const WaitResult waited =
            wait_deadline(handle, op.ov, deadline_ms, stop, slice_ms);
        if (waited == WaitResult::Aborted)
        {
            aborted = true;
            return false;
        }
        if (waited == WaitResult::TimedOut)
        {
            timed_out = true;
            return false;
        }
        if (waited == WaitResult::Failed)
        {
            return false;
        }
        if (!GetOverlappedResult(handle, &op.ov, &chunk, FALSE))
        {
            return false;
        }
        if (chunk == 0)
        {
            return false; // graceful disconnect
        }
        done += chunk;
    }
    return true;
}

// Deadline-bounded write of ALL bytes. An optional stop flag lets the
// SERVE side abort a write-blocked connection at stop time (§5 phase 3:
// the abort covers writes, not only reads — a non-reading peer must not
// hold a serve thread past the grace).
bool write_all_ov(HANDLE handle, const char* buffer, usize size, u64 deadline_ms,
                  const std::atomic<bool>* stop = nullptr)
{
    constexpr u64 slice_ms = 100;
    usize done             = 0;
    while (done < size)
    {
        OverlappedOp op;
        op.ov.Offset     = 0;
        op.ov.OffsetHigh = 0;
        DWORD chunk      = 0;
        if (!WriteFile(handle, buffer + done, static_cast<DWORD>(size - done), &chunk, &op.ov))
        {
            if (GetLastError() != ERROR_IO_PENDING)
            {
                return false;
            }
        }
        // The write's own deadline stays the bound (hygiene); the stop flag
        // is the ABORT bound for a stopping server (it fires far earlier).
        const WaitResult waited = wait_deadline(handle, op.ov, deadline_ms, stop, slice_ms);
        if (waited != WaitResult::Completed)
        {
            return false;
        }
        if (!GetOverlappedResult(handle, &op.ov, &chunk, FALSE))
        {
            return false;
        }
        if (chunk == 0)
        {
            return false;
        }
        done += chunk;
    }
    return true;
}

// Frame layout reader over the overlapped exact read: ONE deadline bounds
// the COMPLETE frame (header + MAC + body) — the whole-frame M1 semantics
// preserved (a dribbling peer cannot extend the bound by splitting the
// frame across the header/tail reads).
std::optional<std::string> read_bounded_ov(HANDLE handle, u64 timeout_ms,
                                           const std::atomic<bool>* stop, u64 slice_ms,
                                           bool& timed_out, bool& aborted)
{
    timed_out           = false;
    aborted             = false;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    const auto remaining = [&]() {
        return static_cast<u64>(std::max<std::int64_t>(
            0, std::chrono::duration_cast<std::chrono::milliseconds>(
                   deadline - std::chrono::steady_clock::now())
                   .count()));
    };
    std::string header(32, '\0');
    if (!read_exact_ov(handle, header.data(), header.size(), remaining(), stop, slice_ms,
                       timed_out, aborted))
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
    if (!tail.empty() &&
        !read_exact_ov(handle, tail.data(), tail.size(), remaining(), stop, slice_ms, timed_out,
                       aborted))
    {
        return std::nullopt;
    }
    header.append(tail);
    return header;
}

// Create one listen instance of the named pipe (overlapped, owner-only
// DACL). first_instance adds FILE_FLAG_FIRST_PIPE_INSTANCE (the OS-level
// singleton ownership assertion): ACCESS_DENIED / PIPE_BUSY there is the
// typed second-host error.
qiven::Result<HANDLE> create_listen_instance(const std::wstring& name, bool first_instance)
{
    using HandleResult              = qiven::Result<HANDLE>;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            owner_only_sddl().c_str(), SDDL_REVISION_1, &descriptor, nullptr))
    {
        return HandleResult::fail(os_error(err_auth, "owner-only DACL construction failed"));
    }
    SECURITY_ATTRIBUTES attributes { sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE };
    const DWORD first = first_instance ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0;
    HANDLE handle     = CreateNamedPipeW(
        name.c_str(), PIPE_ACCESS_DUPLEX | first | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        PIPE_UNLIMITED_INSTANCES, 64 * 1024, 64 * 1024, 0, &attributes);
    if (handle == INVALID_HANDLE_VALUE)
    {
        const DWORD error = GetLastError();
        LocalFree(descriptor);
        if (first_instance && (error == ERROR_ACCESS_DENIED || error == ERROR_PIPE_BUSY))
        {
            return HandleResult::fail(os_error(err_host_singleton,
                                               "the pipe already exists (another host owns it)"));
        }
        return HandleResult::fail(os_error(err_auth, "pipe instance creation failed"));
    }
    LocalFree(descriptor);
    return HandleResult(handle);
}

// Wait for a client on an armed instance, sliced against the stop flag
// (indefinite patience — LL-3: the host never times out a connect).
// Returns: connected / stopped / vanished / failed.
enum class ConnectOutcome
{
    Connected,
    Stopped,
    Vanished,
    Failed
};

ConnectOutcome wait_connect(HANDLE instance, const std::atomic<bool>* stop, u64 slice_ms)
{
    OverlappedOp op;
    if (!ConnectNamedPipe(instance, &op.ov) && GetLastError() != ERROR_IO_PENDING)
    {
        const DWORD error = GetLastError();
        return error == ERROR_PIPE_CONNECTED ? ConnectOutcome::Connected
                                             : ConnectOutcome::Failed;
    }
    while (true)
    {
        if (stop != nullptr && stop->load(std::memory_order_acquire))
        {
            CancelIoEx(instance, &op.ov);
            DWORD reaped = 0;
            GetOverlappedResult(instance, &op.ov, &reaped, TRUE);
            return ConnectOutcome::Stopped;
        }
        const DWORD waited = wait_slice(op.ov.hEvent, slice_ms);
        if (waited == WAIT_OBJECT_0)
        {
            DWORD transferred = 0;
            if (GetOverlappedResult(instance, &op.ov, &transferred, FALSE))
            {
                return ConnectOutcome::Connected;
            }
            const DWORD error = GetLastError();
            if (error == ERROR_NO_DATA || error == ERROR_BROKEN_PIPE ||
                error == ERROR_PIPE_NOT_CONNECTED)
            {
                return ConnectOutcome::Vanished;
            }
            return ConnectOutcome::Failed;
        }
        if (waited == WAIT_FAILED)
        {
            return ConnectOutcome::Failed;
        }
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

    // The record is the installation's own-tool allowlist: exes deployed in
    // the SAME directory as the host are the same installation unit (the
    // owner-only DACL + same-user pipe remain the trust boundary). Boot
    // MERGES missing images (idempotent): the 2024-09-24 preflight found
    // real deployments where only the host was recorded and every hook
    // client denied admission — the second root cause hiding behind the
    // trial-3 connection-model defect (silent drop made it look like 116).
    auto write_record = [&](const std::vector<std::string>& entries) -> RecordResult {
        jsonx::JsonObject object;
        std::vector<jsonx::JsonValue> values;
        for (const auto& image : entries)
        {
            values.push_back(jsonx::JsonValue::make_string(image));
        }
        object.emplace_back("clients", jsonx::JsonValue::make_array(std::move(values)));
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << jsonx::write(jsonx::JsonValue::make_object(std::move(object)));
        if (!out)
        {
            return RecordResult::fail(os_error(err_auth, "client record write failed"));
        }
        return RecordResult(entries);
    };

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
    }

    bool changed = false;
    for (const auto& image : first_boot)
    {
        bool known = false;
        for (const auto& existing : images)
        {
            if (existing == image)
            {
                known = true;
                break;
            }
        }
        if (!known)
        {
            images.push_back(image);
            changed = true;
        }
    }
    if (!std::filesystem::exists(file) || changed)
    {
        return write_record(images);
    }
    return RecordResult(std::move(images));
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
m_handle(std::exchange(other.m_handle, nullptr)),
m_last_read_timed_out(other.m_last_read_timed_out),
m_last_read_aborted(other.m_last_read_aborted)
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
        m_handle              = std::exchange(other.m_handle, nullptr);
        m_last_read_timed_out = other.m_last_read_timed_out;
        m_last_read_aborted   = other.m_last_read_aborted;
    }
    return *this;
}

std::optional<std::string> PipeConnection::read_frame()
{
    // The undecorated form has no deadline: one frame, however long the
    // peer takes (single-frame test/admin callers).
    if (m_handle == nullptr)
    {
        return std::nullopt;
    }
    m_last_read_timed_out      = false;
    m_last_read_aborted        = false;
    constexpr u64 far_deadline = 3'600'000;
    bool timed_out             = false;
    bool aborted               = false;
    auto frame                 = read_bounded_ov(static_cast<HANDLE>(m_handle), far_deadline, nullptr, 200,
                                                 timed_out, aborted);
    m_last_read_timed_out      = timed_out;
    m_last_read_aborted        = aborted;
    return frame;
}

std::optional<std::string> PipeConnection::read_frame(u64 timeout_ms,
                                                      const std::atomic<bool>* stop)
{
    if (m_handle == nullptr)
    {
        return std::nullopt;
    }
    m_last_read_timed_out = false;
    m_last_read_aborted   = false;
    bool timed_out        = false;
    bool aborted          = false;
    auto frame            = read_bounded_ov(static_cast<HANDLE>(m_handle), timeout_ms, stop, 200,
                                            timed_out, aborted);
    m_last_read_timed_out = timed_out;
    m_last_read_aborted   = aborted;
    return frame;
}

bool PipeConnection::write_bytes(std::string_view bytes, u64 deadline_ms,
                                 const std::atomic<bool>* stop)
{
    if (m_handle == nullptr)
    {
        return false;
    }
    return write_all_ov(static_cast<HANDLE>(m_handle), bytes.data(), bytes.size(), deadline_ms,
                        stop);
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

// --- ServeLoop (the production listen pool) ----------------------------------

ServeLoop::ServeLoop(std::wstring pipe, void* first_instance, FrameCodec codec,
                     Options options, Hooks hooks) :
m_pipe(std::move(pipe)),
m_codec(std::move(codec)),
m_options(options),
m_hooks(std::move(hooks)),
m_first_instance(first_instance)
{
}

ServeLoop::~ServeLoop()
{
    request_stop();
    for (auto& arm : m_arms)
    {
        if (arm.joinable())
        {
            arm.join();
        }
    }
    // Serve threads are detached; the stop flag aborts their reads AND
    // writes at slice granularity, so the countdown reaches zero promptly.
    // Wait bounded for it before freeing the members a straggler could
    // still touch (the registry notify, the stats): a straggler past this
    // bound can only be a handle-hook callback stuck in user code — the
    // documented teardown worst case (the production exe exits without
    // destructors; §5 stop semantics).
    {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(m_options.stop_grace_ms);
        std::unique_lock<std::mutex> lock(m_serve_mutex);
        (void)m_serve_cv.wait_until(lock, deadline, [this] { return m_live_serve_threads == 0; });
    }
    if (m_first_instance != nullptr)
    {
        CloseHandle(static_cast<HANDLE>(m_first_instance));
    }
}

qiven::Result<std::unique_ptr<ServeLoop>> ServeLoop::create(std::string_view install_id,
                                                            const FrameCodec& codec, Options options,
                                                            Hooks hooks)
{
    using LoopResult = qiven::Result<std::unique_ptr<ServeLoop>>;
    if (options.listen_arms == 0 || options.max_connections == 0)
    {
        return LoopResult::fail(os_error(err_frame, "listen_arms/max_connections must be > 0"));
    }
    if (!hooks.handle)
    {
        return LoopResult::fail(os_error(err_frame, "ServeLoop requires a handle hook"));
    }
    const std::wstring name = pipe_name(install_id);
    auto first              = create_listen_instance(name, true);
    if (!first.is_ok())
    {
        return LoopResult::fail(first.reason());
    }
    return LoopResult(std::unique_ptr<ServeLoop>(
        new ServeLoop(name, first.value(), codec, options, std::move(hooks))));
}

void ServeLoop::request_stop() noexcept
{
    m_stop.store(true, std::memory_order_release);
    m_serve_cv.notify_all();
}

void ServeLoop::run()
{
    // Arms: the first arm inherits the FIRST-instance handle from create()
    // (the singleton assertion), the rest create their own.
    const u64 arms = m_options.listen_arms;
    for (u64 i = 0; i < arms; ++i)
    {
        m_arms.emplace_back([this, i] { arm_thread_body(static_cast<usize>(i)); });
    }
    for (auto& arm : m_arms)
    {
        if (arm.joinable())
        {
            arm.join();
        }
    }

    // Arms have stopped accepting. Grace: wait for the serve countdown (all
    // in-flight requests completed or their threads observed the stop inside
    // one read slice). A straggler past the grace is terminated by process
    // teardown — documented; the journal checkpoints before that in the
    // caller's drain path.
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(m_options.stop_grace_ms);
    std::unique_lock<std::mutex> lock(m_serve_mutex);
    (void)m_serve_cv.wait_until(lock, deadline, [this] { return m_live_serve_threads == 0; });
}

void ServeLoop::arm_thread_body(usize arm_index)
{
    // Whole-body containment WITHOUT respawn: each loop ITERATION is
    // wrapped, so an escaping exception backs off and retries IN THIS
    // THREAD. The arm must remain the joinable thread run()/the destructor
    // know — a detached respawn thread would outlive a stopped loop and
    // touch freed memory (unjoinable fault containment, found by review).
    HANDLE instance = nullptr;
    if (arm_index == 0)
    {
        instance = static_cast<HANDLE>(std::exchange(m_first_instance, nullptr));
    }
    u64 consecutive_create_failures = 0;
    while (!stop_requested())
    {
        try
        {
            if (instance == nullptr)
            {
                auto created = create_listen_instance(m_pipe, false);
                if (!created.is_ok())
                {
                    // NEVER fatal (LL-1): degraded-loud, backoff, keep retrying.
                    ++consecutive_create_failures;
                    m_stats.accept_recreates.fetch_add(1);
                    if (consecutive_create_failures >= 4)
                    {
                        bool was = m_stats.degraded_listener.exchange(true);
                        if (!was && m_hooks.log)
                        {
                            m_hooks.log("[degraded] listener instance creation keeps failing; "
                                        "retrying (host stays up)");
                        }
                    }
                    const auto retry_at = std::chrono::steady_clock::now() +
                                          std::chrono::milliseconds(
                                              std::min<u64>(500 * consecutive_create_failures, 5000));
                    while (!stop_requested() && std::chrono::steady_clock::now() < retry_at)
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    }
                    continue;
                }
                instance = created.value();
            }
            consecutive_create_failures = 0;
            m_stats.degraded_listener.store(false);

            const ConnectOutcome outcome = wait_connect(instance, &m_stop, m_options.connect_slice_ms);
            switch (outcome)
            {
            case ConnectOutcome::Connected:
            {
                PipeConnection connection(instance);
                instance = nullptr;
                ++m_stats.connections_served;
                dispatch_connection(std::move(connection));
                break; // loop re-arms
            }
            case ConnectOutcome::Stopped:
                if (instance != nullptr)
                {
                    DisconnectNamedPipe(instance);
                    CloseHandle(instance);
                    instance = nullptr;
                }
                return;
            case ConnectOutcome::Vanished:
                // A client connected and died inside the accept window:
                // replace the dead instance and keep accepting, WITH a
                // small backoff so an attacker's connect-die churn cannot
                // spin the arm (§5 "recreate and retry with internal
                // backoff" — the unbacked loop was a review finding).
                if (instance != nullptr)
                {
                    CloseHandle(instance);
                    instance = nullptr;
                }
                m_stats.accept_recreates.fetch_add(1);
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            case ConnectOutcome::Failed:
                if (instance != nullptr)
                {
                    CloseHandle(instance);
                    instance = nullptr;
                }
                m_stats.accept_recreates.fetch_add(1);
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue; // never-fatal: recreate and retry
            }
        }
        catch (...)
        {
            // Contained: degrade loud, drop any held instance, back off,
            // and KEEP ARMING in this thread (never exit, never respawn).
            ++m_stats.serve_thread_faults;
            if (m_hooks.log)
            {
                m_hooks.log("[fault] arm thread contained an exception; re-arming");
            }
            if (instance != nullptr)
            {
                DisconnectNamedPipe(instance);
                CloseHandle(instance);
                instance = nullptr;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    if (instance != nullptr)
    {
        DisconnectNamedPipe(instance);
        CloseHandle(instance);
        instance = nullptr;
    }
}

void ServeLoop::dispatch_connection(PipeConnection connection)
{
    if (stop_requested())
    {
        return; // the destructor closes it
    }
    bool over_cap = false;
    u64 occupancy = 0;
    {
        std::lock_guard<std::mutex> guard(m_serve_mutex);
        occupancy = m_live_serve_threads + 1;
        if (m_live_serve_threads >= m_options.max_connections)
        {
            over_cap = true;
            ++m_stats.busy_rejected;
            m_stats.busy_occupancy_last.store(occupancy);
        }
        else
        {
            m_live_serve_threads += 1;
        }
    }
    // One observable line per ACCEPTED connection (the one-connection law's
    // observable: a client that splits hello/event across two connections
    // shows two [open] lines for one registration). The occupancy rides the
    // line so the S-5 discriminator has a per-connection trace.
    if (m_hooks.log)
    {
        m_hooks.log("[open] connection (occupancy " + std::to_string(occupancy) +
                    ")");
    }
    // NOTE: everything below this point must not throw onto the ARM
    // thread (fault containment §5) — the busy/log paths are guarded.
    if (over_cap)
    {
        // Typed busy frame + the SHARED bounded linger every typed error
        // close uses (pipe_service), so the frame is never discarded by the
        // close that follows it. The log line is the observable record for
        // the S-5 discriminator (occupancy vs arms).
        if (m_hooks.log)
        {
            m_hooks.log("[busy] connection cap " + std::to_string(m_options.max_connections) +
                        " reached; occupancy " + std::to_string(occupancy) + "; arms " +
                        std::to_string(m_options.listen_arms));
        }
        const Reply busy = make_error(0, m_options.busy_code,
                                      "server busy: connection cap (" +
                                          std::to_string(m_options.max_connections) +
                                          ") reached; occupancy " + std::to_string(occupancy));
        FrameHeader header;
        (void)connection.write_bytes(m_codec.encode(header, encode_reply(busy)),
                                     m_options.write_deadline_ms, &m_stop);
        linger_for_client_read(connection);
        return;
    }
    try
    {
        std::thread server([this, connection = std::move(connection)]() mutable {
            serve_thread_body(std::move(connection));
        });
        server.detach();
    }
    catch (...)
    {
        // Thread creation failed (resource exhaustion under a client
        // storm): the arm NEVER dies (§5 no-accept-path-exit). Release the
        // registry slot and answer with the typed busy frame instead.
        {
            // Notify INSIDE the lock: the countdown reaching zero releases
            // run()'s drain wait, and an embedder may destroy the loop the
            // moment run() returns — a notify after the unlock can touch
            // the destroyed condition variable (found by review).
            std::lock_guard<std::mutex> guard(m_serve_mutex);
            if (m_live_serve_threads > 0)
            {
                m_live_serve_threads -= 1;
            }
            m_serve_cv.notify_all();
        }
        const Reply busy = make_error(0, m_options.busy_code,
                                      "server busy: serve thread unavailable");
        FrameHeader header;
        (void)connection.write_bytes(m_codec.encode(header, encode_reply(busy)),
                                     m_options.write_deadline_ms, &m_stop);
        linger_for_client_read(connection);
    }
}

void ServeLoop::serve_thread_body(PipeConnection connection)
{
    bool saw_shutdown = false;
    try
    {
        AdmitFn admit   = m_hooks.admit;
        HandleFn handle = [this, &saw_shutdown](const Request& request) {
            Reply reply = m_hooks.handle(request);
            // An authenticated shutdown stops the loop AFTER the ack rides
            // this connection (serve_connection writes the reply first).
            if (request.kind == Request::Kind::Shutdown)
            {
                saw_shutdown = true;
                request_stop();
            }
            return reply;
        };
        ServeOptions options;
        options.idle_timeout_ms   = m_options.idle_timeout_ms;
        options.max_frames        = m_options.max_frames;
        options.write_deadline_ms = m_options.write_deadline_ms;
        options.stop              = &m_stop;
        (void)serve_connection(connection, m_codec, admit ? admit : [](PipeConnection&) { return std::string(); }, handle, options);
    }
    catch (...)
    {
        // Per-connection fault containment (LL-3): the connection object
        // closes at scope exit; the host and every other connection
        // continue.
        ++m_stats.serve_thread_faults;
        if (m_hooks.log)
        {
            m_hooks.log("[fault] serve thread contained an exception; connection closed");
        }
    }
    if (saw_shutdown)
    {
        // The shutdown ack must reach the client before teardown closes the
        // pipe: DisconnectNamedPipe discards unread bytes, and the loop
        // tears down FAST once stopped (arms join, grace, exit). The same
        // bounded linger every terminal error frame uses covers the ack's
        // read window (found live by host_server_lifecycle: the ctl
        // delivered shutdown, the host logged the ack, and the client
        // still observed "no reply").
        linger_for_client_read(connection);
    }
    {
        // Notify INSIDE the lock (same lifetime law as the creation-failure
        // path): once the countdown hits zero, run() may return and the
        // embedder may destroy the loop — the notify must not race that
        // destruction from outside the lock.
        std::lock_guard<std::mutex> guard(m_serve_mutex);
        if (m_live_serve_threads > 0)
        {
            m_live_serve_threads -= 1;
        }
        m_serve_cv.notify_all();
    }
}

// --- NamedPipeServer (test-surface adapter) ----------------------------------
//
// The production serve path is ServeLoop. This adapter remains for tests
// that need a raw single-instance server (the DACL shape check and the
// FIRST_PIPE_INSTANCE singleton assertion) and for single-connection
// library drivers; its accept() blocks on ONE armed instance at a time —
// the retired serial shape, retained as an explicitly-labeled test surface.

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
    using ServerResult      = qiven::Result<NamedPipeServer>;
    const std::wstring name = pipe_name(install_id);
    auto first              = create_listen_instance(name, true);
    if (!first.is_ok())
    {
        const std::string message = first.reason().message;
        if (message.find("os error 5") != std::string::npos ||
            message.find("os error 231") != std::string::npos)
        {
            return ServerResult::fail(
                os_error(err_host_singleton, "the pipe already exists (another host owns it)"));
        }
        return ServerResult::fail(first.reason());
    }
    return ServerResult(NamedPipeServer(first.value(), name));
}

qiven::Result<PipeConnection> NamedPipeServer::accept()
{
    using AcceptResult = qiven::Result<PipeConnection>;
    if (m_handle == nullptr)
    {
        return AcceptResult::fail(os_error(err_frame, "server is closed"));
    }
    while (true)
    {
        const ConnectOutcome outcome = wait_connect(static_cast<HANDLE>(m_handle), nullptr, 100);
        if (outcome == ConnectOutcome::Connected)
        {
            void* connected = m_handle;
            auto next       = create_listen_instance(m_name, false);
            if (!next.is_ok())
            {
                CloseHandle(static_cast<HANDLE>(connected));
                m_handle = nullptr;
                return AcceptResult::fail(os_error(err_frame,
                                                   "next pipe instance creation failed"));
            }
            m_handle = next.value();
            return AcceptResult(PipeConnection(connected));
        }
        if (outcome == ConnectOutcome::Vanished)
        {
            // A client connected and died inside the accept window: replace
            // the dead instance and keep accepting (INTERNAL retry, M2).
            CloseHandle(static_cast<HANDLE>(m_handle));
            auto next = create_listen_instance(m_name, false);
            if (!next.is_ok())
            {
                m_handle = nullptr;
                return AcceptResult::fail(os_error(err_frame,
                                                   "next pipe instance creation failed"));
            }
            m_handle = next.value();
            continue;
        }
        if (outcome == ConnectOutcome::Stopped)
        {
            return AcceptResult::fail(os_error(err_frame, "accept stopped"));
        }
        return AcceptResult::fail(os_error(err_frame, "accept failed"));
    }
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
m_handle(std::exchange(other.m_handle, nullptr)),
m_last_read_timed_out(other.m_last_read_timed_out)
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
        m_handle              = std::exchange(other.m_handle, nullptr);
        m_last_read_timed_out = other.m_last_read_timed_out;
    }
    return *this;
}

qiven::Result<PipeClient> PipeClient::connect(const std::wstring& name, u64 busy_wait_ms)
{
    using ClientResult = qiven::Result<PipeClient>;
    const auto begin   = std::chrono::steady_clock::now();
    while (true)
    {
        HANDLE handle = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                    OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (handle != INVALID_HANDLE_VALUE)
        {
            return ClientResult(PipeClient(handle));
        }
        const DWORD error = GetLastError();
        if (error != ERROR_PIPE_BUSY)
        {
            return ClientResult::fail(os_error(err_frame, "pipe connect failed"));
        }
        // WaitNamedPipe etiquette (LL-3): wait bounded for an instance to
        // re-arm, then retry the connect. 120-class "no listener" is
        // classified by the CALLER only when the budget expires.
        const auto now = std::chrono::steady_clock::now();
        if (now >= begin + std::chrono::milliseconds(busy_wait_ms))
        {
            return ClientResult::fail(os_error(err_frame, "pipe connect failed"));
        }
        const u64 remaining = static_cast<u64>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                begin + std::chrono::milliseconds(busy_wait_ms) - now)
                .count());
        WaitNamedPipeW(name.c_str(),
                       static_cast<DWORD>(std::min<u64>(remaining, 1000)));
    }
}

bool PipeClient::write_bytes(std::string_view bytes, u64 deadline_ms)
{
    if (m_handle == nullptr)
    {
        return false;
    }
    return write_all_ov(static_cast<HANDLE>(m_handle), bytes.data(), bytes.size(), deadline_ms);
}

std::optional<std::string> PipeClient::read_frame()
{
    if (m_handle == nullptr)
    {
        return std::nullopt;
    }
    m_last_read_timed_out      = false;
    constexpr u64 far_deadline = 3'600'000;
    bool timed_out             = false;
    bool aborted               = false;
    auto frame                 = read_bounded_ov(static_cast<HANDLE>(m_handle), far_deadline, nullptr, 200,
                                                 timed_out, aborted);
    m_last_read_timed_out      = timed_out;
    return frame;
}

std::optional<std::string> PipeClient::read_frame(u64 timeout_ms)
{
    if (m_handle == nullptr)
    {
        return std::nullopt;
    }
    m_last_read_timed_out = false;
    bool timed_out        = false;
    bool aborted          = false;
    auto frame            = read_bounded_ov(static_cast<HANDLE>(m_handle), timeout_ms, nullptr, 200,
                                            timed_out, aborted);
    m_last_read_timed_out = timed_out;
    return frame;
}
} // namespace qiven::runtime::ipc
