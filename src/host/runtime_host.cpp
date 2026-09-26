#include <qiven/runtime/host/runtime_host.hpp>

#include <qiven/runtime/adapter/zcode_hook.hpp>
#include <qiven/runtime/host/generation_activation.hpp>
#include <qiven/runtime/ipc/named_pipe_server.hpp>
#include <qiven/runtime/processx/process_runner.hpp>
#include <qiven/runtime/scope.hpp>

#include <windows.h>

#include <cctype>
#include <chrono>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <locale>
#include <optional>
#include <set>
#include <sstream>
#include <utility>
#include <vector>

namespace qiven::runtime::host
{
namespace
{
constexpr u64 hook_default_decision_ttl_ms = 600'000; // 10 min single-use window

class MutexHandle
{
public:
    explicit MutexHandle(void* handle) noexcept :
    m_handle(handle)
    {
    }
    ~MutexHandle()
    {
        if (m_handle != nullptr)
        {
            CloseHandle(static_cast<HANDLE>(m_handle));
        }
    }
    MutexHandle(MutexHandle&& other) noexcept :
    m_handle(std::exchange(other.m_handle, nullptr))
    {
    }
    MutexHandle(const MutexHandle&)            = delete;
    MutexHandle& operator=(const MutexHandle&) = delete;
    [[nodiscard]] void* release() noexcept
    {
        return std::exchange(m_handle, nullptr);
    }

private:
    void* m_handle = nullptr;
};

qiven::Result<MutexHandle> acquire_singleton(const std::filesystem::path& repo_root)
{
    // ROOT-IDENTITY keyed singleton (host-server redesign §5 boot order):
    // acquired BEFORE any journal open or recovery, so an idempotent start
    // racing a healthy server fails fast WITHOUT touching the live
    // journal. The key is the root's CANONICAL identity (GetFinalPathFromHandle
    // — the OS-truth spelling resolving case variance, 8.3 short names,
    // subst drives, and symlinks), case-folded and hashed: alias spellings
    // of one checkout share the mutex. (The previous install-id key was
    // minted by the journal itself, forcing a journal-first order —
    // mvp3-host-ipc.md §3.4's mutex-first spec was unimplementable with it.)
    using MutexResult = qiven::Result<MutexHandle>;
    HANDLE dir        = CreateFileW(repo_root.c_str(), GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (dir == INVALID_HANDLE_VALUE)
    {
        return MutexResult::fail(qiven::Error::make(qiven::error_category::unavailable,
                                                    err_singleton,
                                                    "governed root cannot be opened"));
    }
    wchar_t final_path[MAX_PATH * 2] {};
    std::string canonical;
    const DWORD final_len =
        GetFinalPathNameByHandleW(dir, final_path, MAX_PATH * 2,
                                  FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (final_len > 0 && final_len < MAX_PATH * 2) // == size means truncated
    {
        std::wstring folded(final_path);
        for (auto& c : folded)
        {
            c = static_cast<wchar_t>(towlower(c));
        }
        const ContentDigest digest =
            cognition::digest_of(std::string(folded.begin(), folded.end()));
        canonical = cognition::hex_lower(
            std::span<const std::byte>(digest.sha256.data(), digest.sha256.size()));
    }
    CloseHandle(dir);
    if (canonical.empty())
    {
        return MutexResult::fail(qiven::Error::make(qiven::error_category::unavailable,
                                                    err_singleton,
                                                    "governed root identity unavailable"));
    }
    const std::wstring name = L"Local\\qiven-runtime-root-" +
                              std::wstring(canonical.begin(), canonical.begin() + 16);
    HANDLE handle = CreateMutexW(nullptr, TRUE, name.c_str());
    if (handle == nullptr)
    {
        return MutexResult::fail(qiven::Error::make(qiven::error_category::unavailable,
                                                    err_singleton, "singleton mutex creation failed"));
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        CloseHandle(handle);
        return MutexResult::fail(qiven::Error::make(qiven::error_category::unavailable,
                                                    err_singleton,
                                                    "another RuntimeHost owns this governed root"));
    }
    return MutexResult(MutexHandle(handle));
}

std::string narrow_ws(const std::wstring& wide)
{
    if (wide.empty())
    {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<usize>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), out.data(), size,
                        nullptr, nullptr);
    return out;
}

std::vector<std::byte> audit_bytes(std::string_view text)
{
    return { reinterpret_cast<const std::byte*>(text.data()),
             reinterpret_cast<const std::byte*>(text.data()) + text.size() };
}

// The journal's audit surface is the port append (chain-protected); the
// hook events ride it as AuditEvent records with a minted id and a
// "kind|payload" preimage (the private append_audit is command-internal).
void append_event(journal::RuntimeJournal& journal, std::string_view kind,
                  std::string_view payload, u64 now_ms)
{
    // The caller's clock is the time authority (injected, DESIGN section
    // 4) -- never the ambient stamp the port append would apply.
    (void)journal.append_audit_at(kind, audit_bytes(std::string(kind) + "|" + std::string(payload)),
                                  now_ms);
}

// Case-insensitive ASCII contains (governed-prefix scan for the Bash
// conservative detector; Unicode case folding is out of scope for the
// governed-path vocabulary, which is ASCII).
[[nodiscard]] char lower_ascii(char c)
{
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

[[nodiscard]] bool contains_ci(std::string_view haystack, std::string_view needle)
{
    if (needle.empty() || haystack.size() < needle.size())
    {
        return false;
    }
    const auto lower = [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    };
    for (usize at = 0; at + needle.size() <= haystack.size(); at += 1)
    {
        bool same = true;
        for (usize i = 0; i < needle.size(); i += 1)
        {
            if (lower(haystack[at + i]) != lower(needle[i]))
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
    return false;
}

// Case-insensitive "haystack starts with needle" (the governed-root prefix
// bound for the exact-file form).
[[nodiscard]] bool starts_with_ci(const std::string& haystack, const std::string& needle)
{
    if (needle.size() > haystack.size())
    {
        return false;
    }
    for (usize i = 0; i < needle.size(); i += 1)
    {
        if (lower_ascii(haystack[i]) != lower_ascii(needle[i]))
        {
            return false;
        }
    }
    return true;
}

// Case-insensitive "haystack ends with needle" (the governed-path
// exact-file form: an absolute target naming the governed file itself; the
// needle's leading separator is the boundary -- "current.md.bak" and
// "notstate/current.md" cannot match "/state/current.md").
[[nodiscard]] bool ends_with_ci(const std::string& haystack, const std::string& needle)
{
    if (needle.size() > haystack.size())
    {
        return false;
    }
    const usize at = haystack.size() - needle.size();
    for (usize i = 0; i < needle.size(); i += 1)
    {
        if (lower_ascii(haystack[at + i]) != lower_ascii(needle[i]))
        {
            return false;
        }
    }
    return true;
}

// Lexical path normalization for the governed-scope test: forward slashes,
// collapsed separators, trailing-slash trim. Traversal/reparse rejection is
// the caller's deny (ARCH section 12.3 -- no bypass through traversal).
[[nodiscard]] std::string normalize_hook_path(const std::string& raw)
{
    std::string out;
    out.reserve(raw.size());
    for (const char c : raw)
    {
        out.push_back(c == '\\' ? '/' : c);
    }
    while (out.size() > 1 && out.back() == '/')
    {
        out.pop_back();
    }
    return out;
}
} // namespace

RuntimeHost::~RuntimeHost()
{
    {
        std::lock_guard<std::mutex> guard(m_refresh_mutex);
        m_worker_running = false;
    }
    m_refresh_cv.notify_all();
    if (m_refresh_worker.joinable())
    {
        m_refresh_worker.join();
    }
    if (m_journal != nullptr)
    {
        // Graceful-shutdown checkpoint (WAL truncate); recovery at next
        // boot is still authoritative (MVP-1 law). A destructor cannot
        // propagate the failure surface; the discard is deliberate.
        (void)m_journal->checkpoint();
    }
    m_state = "stopped";
}

qiven::Result<std::unique_ptr<RuntimeHost>> RuntimeHost::boot(const HostBoot& boot)
{
    using HostResult = qiven::Result<std::unique_ptr<RuntimeHost>>;

    // 0. ROOT-keyed singleton FIRST (host-server redesign §5): before any
    // durable touch, so a second start against a live server exits here
    // without opening or recovering the live journal.
    auto mutex = acquire_singleton(boot.repo_root);
    if (!mutex.is_ok())
    {
        return HostResult::fail(mutex.reason());
    }

    // 1. Journal: the install identity names the pipe. First boot creates;
    // later boots open (a corruption never yields a fresh mutating
    // database -- the MVP-1 fresh-database guard).
    const std::filesystem::path runtime_root = boot.repo_root / ".qiven" / "runtime";
    std::filesystem::create_directories(runtime_root);
    const std::filesystem::path journal_file = runtime_root / "journal.sqlite3";
    const journal::JournalOpenIntent intent =
        std::filesystem::exists(journal_file) ? journal::JournalOpenIntent::OpenExisting
                                              : journal::JournalOpenIntent::CreateNew;
    auto journal = journal::RuntimeJournal::open(journal_file, intent, boot.now_ms);
    if (!journal.is_ok())
    {
        return HostResult::fail(journal.reason());
    }
    auto recovery = journal.value()->recover_at(boot.now_ms);
    if (!recovery.is_ok())
    {
        return HostResult::fail(recovery.reason());
    }
    auto install_id = journal.value()->install_id();
    if (!install_id.is_ok())
    {
        return HostResult::fail(install_id.reason());
    }

    auto host          = std::unique_ptr<RuntimeHost>(new RuntimeHost {});
    host->m_journal    = std::move(journal.value());
    host->m_install_id = install_id.value();
    if (auto epoch = host->m_journal->boot_epoch(); epoch.is_ok())
    {
        host->m_boot_epoch = epoch.value();
    }
    host->m_build_id = boot.build_id;
    if (auto quarantined = host->m_journal->quarantined(); quarantined.is_ok())
    {
        host->m_quarantined = quarantined.value();
    }
    host->m_singleton_mutex = mutex.value().release();

    // 3. Installation secret + client record (DPAPI; jsonx).
    auto secret = ipc::InstallationSecret::ensure(runtime_root);
    if (!secret.is_ok())
    {
        host->m_failure_detail = secret.reason().message;
        return HostResult(std::move(host));
    }
    wchar_t self_image[MAX_PATH] {};
    GetModuleFileNameW(nullptr, self_image, MAX_PATH);
    auto clients = ipc::ClientRecord::ensure(runtime_root, { narrow_ws(self_image) });
    if (!clients.is_ok())
    {
        host->m_failure_detail = clients.reason().message;
        return HostResult(std::move(host));
    }

    // 4. Accepted profile (+ its file digest as the generation's profile
    //    identity -- the ACCEPTED instance, content-identified). The loaded
    //    instance is CACHED: it is static accepted configuration, and the
    //    request path must not touch the disk under the state mutex.
    std::ifstream profile_bytes_in(boot.profile_file, std::ios::binary);
    std::string profile_bytes((std::istreambuf_iterator<char>(profile_bytes_in)),
                              std::istreambuf_iterator<char> {});
    const ContentDigest profile_digest =
        cognition::digest_of(profile_bytes);
    auto profile = load_profile_file(boot.profile_file);
    if (!profile.is_ok())
    {
        host->m_failure_detail = "profile: " + profile.reason().detail;
        return HostResult(std::move(host));
    }

    host->m_profile_cache = profile.value(); // cached; request paths read this copy

    // 5. Publish + pin the ACTIVE bundle from the authorized LOCAL ref
    //    (remote fetch + freshness window land with MVP-4 SessionStart).
    processx::ProcessRunner runner;
    cognition::PublishRequest publish_request;
    publish_request.repo_root       = boot.repo_root;
    publish_request.ref             = profile.value().cognition.authorized_ref;
    publish_request.runtime_root    = runtime_root;
    publish_request.repository_url  = profile.value().cognition.repository;
    publish_request.policy_path     = profile.value().cognition.policy_path;
    publish_request.source_paths    = profile.value().cognition.source_paths;
    publish_request.publisher_build = boot.build_id;
    publish_request.now_ms          = boot.now_ms;
    publish_request.git_executable  = boot.git_executable;
    publish_request.runner          = &runner;
    cognition::CanonicalCognitionPublisher publisher;
    auto published = publisher.publish(publish_request);
    if (!published.is_ok())
    {
        host->m_failure_detail = "publish: " + published.reason().detail;
        return HostResult(std::move(host));
    }
    // The profile's hard-gate policy digest must match the bundle's policy
    // (ARCH section 7.3: the profile pins the policy it was accepted with).
    if (to_hex(published.value().manifest.policy_digest) !=
        profile.value().cognition.policy_sha256)
    {
        host->m_failure_detail = "policy digest disagrees with the accepted profile pin";
        return HostResult(std::move(host));
    }

    cognition::BundleStore store(runtime_root);
    auto bundle = store.load_active();
    if (!bundle.is_ok())
    {
        host->m_failure_detail = "bundle: " + bundle.reason().detail;
        return HostResult(std::move(host));
    }
    auto pin = store.pin_from_bundle(published.value().manifest);
    if (!pin.is_ok())
    {
        host->m_failure_detail = "pin: " + pin.reason().detail;
        return HostResult(std::move(host));
    }

    // 6. Generation activation (invalidates old unconsumed tokens).
    auto generation = activate_generation(*host->m_journal, published.value().bundle_digest,
                                          profile_digest, boot.build_id, boot.now_ms);
    if (!generation.is_ok())
    {
        host->m_failure_detail = "generation: " + generation.reason().message;
        return HostResult(std::move(host));
    }
    host->m_generation      = generation.value().value;
    host->m_bundle_revision = published.value().manifest.source_revision;
    host->m_state           = "running";

    // MVP-4 members: the boot facts, the active bundle identity, and the
    // durable freshness clock (last successful publish -- boot publishes
    // from the authorized LOCAL ref; the HOST-AUTONOMOUS worker confirms
    // currency on its own cadence + the operator trigger — the SessionStart
    // trigger is superseded by the host-server redesign §6, the freshness
    // LAW of ARCH §7.4 stands).
    host->m_repo_root            = boot.repo_root;
    host->m_profile_file         = boot.profile_file;
    host->m_git_executable       = boot.git_executable;
    host->m_active_bundle_digest = published.value().bundle_digest;
    host->m_freshness_window_ms  = profile.value().freshness_window_ms;
    host->m_refresh_interval_ms  = profile.value().refresh_interval_ms;
    if (auto saved = host->m_journal->get_meta("last_refresh_ok_ms"); saved.is_ok() &&
                                                                      saved.value().has_value() && !saved.value()->empty())
    {
        host->m_last_refresh_ok_ms =
            static_cast<u64>(std::strtoull(saved.value()->c_str(), nullptr, 10));
    }
    else
    {
        host->m_last_refresh_ok_ms = boot.now_ms;
        (void)host->m_journal->set_meta("last_refresh_ok_ms", std::to_string(boot.now_ms));
    }
    host->m_refresh_state = host->refresh_state_now(boot.now_ms);

    // The install identity file names the pipe for same-user clients
    // (runtimectl); the journal remains the authority for the identity.
    {
        std::ofstream install_file(runtime_root / "install.id", std::ios::trunc);
        install_file << host->m_install_id << "\n";
    }
    return HostResult(std::move(host));
}

StatusSnapshot RuntimeHost::status() const
{
    std::lock_guard<std::mutex> guard(m_state_mutex);
    return status_locked(wall_now_ms());
}

StatusSnapshot RuntimeHost::status_locked(u64 now_ms) const
{
    // Caller holds m_state (public status() locks; handle() calls this).
    StatusSnapshot snapshot;
    snapshot.state              = m_state;
    snapshot.install_id         = m_install_id;
    snapshot.boot_epoch         = m_boot_epoch;
    snapshot.generation         = m_generation;
    snapshot.bundle_revision    = m_bundle_revision;
    snapshot.quarantined        = m_quarantined;
    snapshot.failure_detail     = m_failure_detail;
    snapshot.refresh_state      = refresh_state_now(now_ms);
    snapshot.last_refresh_ok_ms = m_last_refresh_ok_ms;
    snapshot.next_refresh_due_ms =
        m_last_attempt_ms + m_refresh_interval_ms > m_last_attempt_ms
            ? m_last_attempt_ms + m_refresh_interval_ms
            : 0;
    if (m_journal != nullptr)
    {
        if (auto events = m_journal->audit_event_count(); events.is_ok())
        {
            snapshot.journal_events = events.value();
        }
    }
    return snapshot;
}

ipc::Reply RuntimeHost::doctor() const
{
    std::lock_guard<std::mutex> guard(m_state_mutex);
    return doctor_locked();
}

ipc::Reply RuntimeHost::doctor_locked() const
{
    // Caller holds m_state.
    ipc::Reply reply;
    reply.kind         = ipc::Reply::Kind::DoctorView;
    reply.integrity_ok = true;
    if (m_journal != nullptr)
    {
        reply.audit_chain_ok = m_journal->verify_audit_chain().is_ok();
        if (!reply.audit_chain_ok)
        {
            reply.findings.push_back("audit hash chain verification failed");
        }
    }
    else
    {
        reply.integrity_ok = false;
        reply.findings.push_back("journal is not open");
    }
    if (!m_bundle_revision.empty())
    {
        reply.bundle_active_ok = true;
    }
    else
    {
        reply.findings.push_back("no ACTIVE cognition bundle: " + m_failure_detail);
    }
    if (m_quarantined)
    {
        reply.findings.push_back("journal is quarantined; governed mutation must stop");
    }
    return reply;
}

ipc::Reply RuntimeHost::handle(const ipc::Request& request, u64 now_ms)
{
    // The ONE state mutex (§5): every request handler serializes here; hold
    // times are millisecond-scale by construction (no network, no git, no
    // bundle I/O is reachable from a request path — LL-2b).
    std::lock_guard<std::mutex> guard(m_state_mutex);
    switch (request.kind)
    {
    case ipc::Request::Kind::Hello:
    {
        ipc::Reply reply;
        reply.kind       = ipc::Reply::Kind::HelloAck;
        reply.request_id = request.request_id;
        reply.host_build = m_build_id;
        reply.install_id = m_install_id;
        reply.boot_epoch = m_boot_epoch;
        return reply;
    }
    case ipc::Request::Kind::Status:
    {
        StatusSnapshot snapshot = status_locked(now_ms);
        ipc::Reply reply;
        reply.kind                = ipc::Reply::Kind::StatusView;
        reply.request_id          = request.request_id;
        reply.state               = snapshot.state;
        reply.install_id          = snapshot.install_id;
        reply.boot_epoch          = snapshot.boot_epoch;
        reply.generation          = snapshot.generation;
        reply.bundle_revision     = snapshot.bundle_revision;
        reply.journal_events      = snapshot.journal_events;
        reply.quarantined         = snapshot.quarantined;
        reply.refresh_state       = snapshot.refresh_state;
        reply.last_refresh_ok_ms  = snapshot.last_refresh_ok_ms;
        reply.next_refresh_due_ms = snapshot.next_refresh_due_ms;
        if (!snapshot.failure_detail.empty())
        {
            reply.findings.push_back(snapshot.failure_detail);
        }
        return reply;
    }
    case ipc::Request::Kind::Doctor:
    {
        ipc::Reply reply = doctor_locked();
        reply.request_id = request.request_id;
        return reply;
    }
    case ipc::Request::Kind::Mutation:
        // MVP-3: the governed mutation pipeline does not exist yet, and a
        // Starting host denies everything mutating (fail-closed, ARCH §13.2).
        return ipc::make_error(request.request_id, ipc::err_host_recovering,
                               m_state == "running"
                                   ? "governed mutation is not implemented until MVP-5"
                                   : "host is recovering: " + m_failure_detail);
    case ipc::Request::Kind::HookEvent:
        if (m_state != "running" || m_shutting_down)
        {
            return ipc::make_error(request.request_id,
                                   m_shutting_down ? adapter::hook_reason_shutting_down
                                                   : ipc::err_host_recovering,
                                   m_shutting_down ? "host is draining for shutdown"
                                                   : "host is recovering: " + m_failure_detail);
        }
        return handle_hook_event(request, now_ms);
    case ipc::Request::Kind::Shutdown:
    {
        // H-4 (MVP-4): the ack PRECEDES the drain so the client observes
        // it even if drain outlives the connection (batch design 3.4).
        append_event(*m_journal, "shutdown_requested",
                     request.grace_ms ? "grace" : "default", now_ms);
        ipc::Reply reply;
        reply.kind       = ipc::Reply::Kind::ShutdownAck;
        reply.request_id = request.request_id;
        reply.draining   = true;
        m_shutting_down  = true;
        if (m_state == "running")
        {
            m_state = "draining";
        }
        return reply;
    }
    case ipc::Request::Kind::Refresh:
    {
        // Operator convenience TRIGGER (§6): coalesced, never blocking, and
        // the host is complete without it (LL-2b). The reply is immediate.
        std::string result = "no_worker"; // honest: nothing will consume it
        {
            std::lock_guard<std::mutex> refresh_guard(m_refresh_mutex);
            if (m_worker_running)
            {
                m_refresh_pending = true;
                result            = now_ms >= m_last_attempt_ms + m_refresh_coalesce_ms
                                        ? "triggered"
                                        : "coalesced_pending"; // runs at cooldown end
            }
        }
        m_refresh_cv.notify_all();
        append_event(*m_journal, "refresh_triggered", result, now_ms);
        ipc::Reply reply;
        reply.kind                = ipc::Reply::Kind::RefreshAck;
        reply.request_id          = request.request_id;
        reply.refresh_state       = refresh_state_now(now_ms);
        reply.last_refresh_ok_ms  = m_last_refresh_ok_ms;
        reply.next_refresh_due_ms = m_last_attempt_ms + m_refresh_interval_ms;
        reply.refresh_result      = result;
        return reply;
    }
    }
    return ipc::make_error(request.request_id, ipc::err_frame, "unhandled request kind");
}

void RuntimeHost::request_shutdown() noexcept
{
    std::lock_guard<std::mutex> guard(m_state_mutex);
    if (m_state == "running")
    {
        m_state = "draining";
    }
    m_shutting_down = true;
}

void RuntimeHost::drain(u64 now_ms)
{
    std::lock_guard<std::mutex> guard(m_state_mutex);
    if (m_state == "running")
    {
        m_state = "draining";
    }
    m_shutting_down = true;
    // Outstanding pre observations are audit-marked Indeterminate at drain.
    // The journal's terminal-transition COMMAND for transactions is a
    // pre-MVP-5 hardening item; the open rows reconcile at the next boot's
    // recovery walk (MVP-1 law) — the audit mark is the durable boundary
    // record (host-server §5 restart semantics, batch-b implementation
    // note).
    for (auto& [handle, session] : m_hook_sessions)
    {
        for (const auto& [tool, outstanding] : session.outstanding)
        {
            append_event(*m_journal, "hook_outcome_indeterminate",
                         outstanding.action_hex + "|" + tool + "|drain", now_ms);
        }
        session.outstanding.clear();
    }
    {
        std::lock_guard<std::mutex> refresh_guard(m_refresh_mutex);
        m_worker_running = false;
    }
    m_refresh_cv.notify_all();
    // The WAL checkpoint lives HERE (not only in the destructor): the exe
    // exits without running local destructors after the drain (straggler
    // serve threads past the grace would otherwise race object teardown).
    if (m_journal != nullptr)
    {
        (void)m_journal->checkpoint();
    }
}

void RuntimeHost::start_refresh_worker()
{
    std::lock_guard<std::mutex> guard(m_refresh_mutex);
    if (m_worker_running)
    {
        return;
    }
    if (m_refresh_worker.joinable()) // a stopped worker from a prior drain
    {
        m_refresh_worker.join(); // assigning to a joinable thread terminates
    }
    m_worker_running = true;
    m_refresh_worker = std::thread([this] { refresh_worker_body(); });
}

bool RuntimeHost::refresh_expired(u64 now_ms) const
{
    return m_freshness_window_ms != 0 && m_last_refresh_ok_ms + m_freshness_window_ms <= now_ms;
}

std::string RuntimeHost::refresh_state_now(u64 now_ms) const
{
    if (m_refresh_state == "degraded")
    {
        return "degraded"; // last attempt faulted; next cadence tick retries
    }
    if (m_last_refresh_ok_ms != 0 && m_freshness_window_ms != 0 &&
        now_ms < m_last_refresh_ok_ms + m_freshness_window_ms && m_last_refresh_ok_ms <= now_ms)
    {
        return m_refresh_state.empty() ? std::string("current") : m_refresh_state;
    }
    return "expired";
}

void RuntimeHost::evict_idle_sessions(u64 now_ms)
{
    // §5 registry eviction: no outstanding pre + idle past the bound → the
    // in-memory entry goes (journal rows are permanent); a re-contact
    // mints fresh — the same honest semantics as a restart.
    for (auto it = m_hook_sessions.begin(); it != m_hook_sessions.end();)
    {
        const RuntimeHost::HookSession& session = it->second;
        const bool idle                         = now_ms > session.last_seen_ms &&
                          now_ms - session.last_seen_ms > m_session_idle_evict_ms;
        if (session.outstanding.empty() && idle)
        {
            it = m_hook_sessions.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void RuntimeHost::refresh_worker_body()
{
    // HOST-AUTONOMOUS refresh (§6): one attempt shortly after boot, then on
    // the profile cadence, plus coalesced operator triggers (minimum
    // interval between attempts). NO hook event can reach this path and no
    // request ever waits on it (LL-2b).
    constexpr u64 first_attempt_delay_ms = 2'000;
    auto next_due                        = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(first_attempt_delay_ms);
    while (true)
    {
        {
            std::unique_lock<std::mutex> lock(m_refresh_mutex);
            m_refresh_cv.wait_until(
                lock, next_due, [this] { return !m_worker_running || m_refresh_pending; });
            if (!m_worker_running)
            {
                return;
            }
            const auto now_steady = std::chrono::steady_clock::now();
            if (!m_refresh_pending && now_steady < next_due)
            {
                continue; // spurious wake: keep waiting for the cadence point
            }
            m_refresh_pending = false;
            // Coalescing (§5): a trigger inside the cooldown waits for the
            // cooldown's end. The wait predicate EXCLUDES m_refresh_pending —
            // with pending in the predicate this loop busy-spins the whole
            // cooldown (found by review).
            const u64 now_wall = wall_now_ms();
            if (now_wall < m_last_attempt_ms + m_refresh_coalesce_ms)
            {
                const auto cooldown_end =
                    now_steady + std::chrono::milliseconds(
                                     m_last_attempt_ms + m_refresh_coalesce_ms - now_wall);
                m_refresh_cv.wait_until(lock, cooldown_end,
                                        [this] { return !m_worker_running; });
                if (!m_worker_running)
                {
                    return;
                }
                continue; // pending stays set; the loop re-evaluates
            }
        }
        // Attempt body with fault containment (§5): an escaping fault
        // degrades refresh state, journals an audit row, and the worker
        // CONTINUES at its next cadence tick — never process death.
        try
        {
            std::string state;
            std::string detail;
            const u64 now = wall_now_ms();
            const bool ok = refresh_cognition(now, state, detail);
            (void)ok;
            {
                std::lock_guard<std::mutex> guard(m_state_mutex);
                m_refresh_state   = state;
                m_last_attempt_ms = now;
                evict_idle_sessions(now);
            }
        }
        catch (...)
        {
            const u64 now = wall_now_ms();
            std::lock_guard<std::mutex> guard(m_state_mutex);
            m_refresh_state   = "degraded";
            m_last_attempt_ms = now;
            append_event(*m_journal, "refresh_fault",
                         "worker attempt faulted; retrying at next cadence", now);
        }
        {
            std::lock_guard<std::mutex> guard(m_refresh_mutex);
            next_due = std::chrono::steady_clock::now() +
                       std::chrono::milliseconds(m_refresh_interval_ms);
        }
    }
}

// --- MVP-4 hook surface (batch design section 3.4; host-server LL-2a) ------

RuntimeHost::HookSession* RuntimeHost::ensure_session(const ipc::Request& request, u64 now_ms)
{
    // FIRST-CONTACT MINTING (LL-2a): any event naming an unseen harness
    // handle mints the runtime session — idempotently within one host
    // lifetime. A lost session_start has NO governance consequence.
    auto found = m_hook_sessions.find(request.session_handle);
    if (found != m_hook_sessions.end())
    {
        found->second.last_seen_ms = now_ms;
        return &found->second;
    }
    HookSession session;
    const SortableId128 id = m_minter.next();
    session.id_hex         = cognition::hex_lower(std::span<const std::byte>(id.bytes.data(),
                                                                             id.bytes.size()));
    journal::SessionOpen open;
    open.generation = RuntimeGenerationId { m_generation };
    open.harness    = "zcode";
    auto row        = m_journal->open_session(open, now_ms);
    if (!row.is_ok())
    {
        // Minting failure must NOT be cached (a cached unjournaled session
        // would deny that harness handle for the whole host lifetime — the
        // LL-4 state-poisoning class). Nothing is inserted; the NEXT call
        // retries the mint.
        return nullptr;
    }
    session.journal_row          = row.value();
    session.last_seen_ms         = now_ms;
    auto [inserted, inserted_ok] = m_hook_sessions.emplace(request.session_handle,
                                                           std::move(session));
    (void)inserted_ok;
    append_event(*m_journal, "session_registered",
                 inserted->second.id_hex + "|" + request.session_handle, now_ms);
    return &inserted->second;
}

ipc::Reply RuntimeHost::handle_hook_event(const ipc::Request& request, u64 now_ms)
{
    if (request.event == "session_start")
    {
        return handle_session_start(request, now_ms);
    }
    if (request.event == "pre_tool")
    {
        return handle_pre_tool(request, now_ms);
    }
    return handle_post_tool(request, now_ms);
}

ipc::Reply RuntimeHost::handle_session_start(const ipc::Request& request, u64 now_ms)
{
    using adapter::hook_reason_scope_mismatch;

    ipc::Reply ack;
    ack.kind       = ipc::Reply::Kind::HookAck;
    ack.request_id = request.request_id;
    ack.generation = m_generation;

    HookSession* session = ensure_session(request, now_ms);
    if (session == nullptr)
    {
        return ipc::make_error(request.request_id, ipc::err_host_recovering,
                               "session identity could not be journaled (transient); "
                               "the next event retries registration");
    }
    ack.session_id = session->id_hex;

    // Advisory manifest handshake (LL-2a): a DECLARED mediated-tools
    // surface is checked against the profile inventory and ONLY the
    // mismatched tools degrade (per-tool 113). No manifest ever arriving
    // degrades nothing — governance runs at the profile-declared scope.
    // An EMPTY manifest is absence, not an empty declaration.
    if (!request.mediated_tools.empty())
    {
        auto profile = cached_profile();
        if (!profile.is_ok())
        {
            session->degraded_tools.clear();
            session->degraded_detail = "profile reload failed: " + profile.reason().detail;
            for (const char* tool : { "Bash", "Write", "Edit" })
            {
                session->degraded_tools.insert(tool);
            }
        }
        else
        {
            // Exact TOKEN match on the comma-separated manifest (a
            // substring match would let "EditSuite" satisfy "Edit").
            std::set<std::string> declared_tokens;
            {
                std::string token;
                std::istringstream stream(request.mediated_tools);
                while (std::getline(stream, token, ','))
                {
                    if (!token.empty())
                    {
                        declared_tokens.insert(token);
                    }
                }
            }
            for (const auto& entry : profile.value().tool_inventory)
            {
                if (declared_tokens.count(entry.tool) == 0)
                {
                    session->degraded_tools.insert(entry.tool);
                    session->degraded_detail +=
                        (session->degraded_detail.empty() ? std::string()
                                                          : std::string("; ")) +
                        "hook manifest lacks declared tool '" + entry.tool + "'";
                }
            }
        }
    }
    // NO refresh runs here (LL-2b): the reply reports the host's current
    // autonomous refresh state and returns immediately.
    ack.verdict       = session->degraded_tools.empty() ? "allow" : "degraded";
    ack.reason_code   = static_cast<i64>(session->degraded_tools.empty()
                                             ? 0
                                             : hook_reason_scope_mismatch);
    ack.reason_detail = session->degraded_detail;
    ack.refresh       = refresh_state_now(now_ms);
    return ack;
}

ipc::Reply RuntimeHost::handle_pre_tool(const ipc::Request& request, u64 now_ms)
{
    using adapter::hook_reason_bash_reference;
    using adapter::hook_reason_cognition_expired;
    using adapter::hook_reason_correlation;
    using adapter::hook_reason_governed_write;
    using adapter::hook_reason_scope_mismatch;
    using adapter::hook_reason_unknown_tool;

    ipc::Reply ack;
    ack.kind       = ipc::Reply::Kind::HookAck;
    ack.request_id = request.request_id;
    ack.generation = m_generation;

    // FIRST CONTACT IS SUFFICIENT (LL-2a): the session mints here if this
    // harness session never registered — the deny-114 "register first"
    // class is RETIRED; a lost session_start has no governance consequence.
    HookSession* session_ptr = ensure_session(request, now_ms);
    if (session_ptr == nullptr)
    {
        // Transient mint failure: fail closed for THIS call; nothing is
        // cached, so the next call retries (no permanent poisoning).
        ack.verdict       = "deny";
        ack.reason_code   = static_cast<i64>(ipc::err_host_recovering);
        ack.reason_detail = "session identity could not be journaled (transient); "
                            "the next call retries";
        return ack;
    }
    HookSession& session = *session_ptr;
    ack.session_id       = session.id_hex;
    if (session.degraded_tools.count(request.tool_name) != 0)
    {
        // Per-tool manifest degradation (LL-2a): only the mismatched tool
        // class denies 113; unaffected tools govern normally.
        ack.verdict       = "deny";
        ack.reason_code   = static_cast<i64>(hook_reason_scope_mismatch);
        ack.reason_detail = session.degraded_detail;
        return ack;
    }

    ack.refresh            = refresh_state_now(now_ms); // informational on every hook event
    const std::string tool = request.tool_name;
    const bool freshness_expired =
        m_freshness_window_ms != 0 && m_last_refresh_ok_ms + m_freshness_window_ms <= now_ms;

    // Tool inventory lookup (profile data; unknown tools fail closed).
    std::string extraction;
    std::string detector;
    bool known_tool = false;
    {
        auto profile = cached_profile();
        if (profile.is_ok())
        {
            for (const auto& row : profile.value().tool_inventory)
            {
                if (row.tool == tool)
                {
                    extraction = row.extraction;
                    detector   = row.detector;
                    known_tool = true;
                    break;
                }
            }
        }
    }
    if (!known_tool)
    {
        ack.verdict       = "deny";
        ack.reason_code   = static_cast<i64>(hook_reason_unknown_tool);
        ack.reason_detail = "tool '" + tool +
                            "' is not in the accepted tool inventory -- fail closed";
        append_event(*m_journal, "hook_deny_unknown_tool", tool, now_ms);
        return ack;
    }
    if (session.outstanding.find(tool) != session.outstanding.end())
    {
        ack.verdict       = "deny";
        ack.reason_code   = static_cast<i64>(hook_reason_correlation);
        ack.reason_detail = "a previous '" + tool +
                            "' action awaits its PostToolUse observation "
                            "(one outstanding pre per tool -- complete correlation)";
        append_event(*m_journal, "hook_deny_outstanding", tool, now_ms);
        return ack;
    }

    // Judgment: is the extracted target inside the governed write scope?
    bool governed = false;
    std::string detail;
    if (extraction == "file_path")
    {
        if (request.file_path.empty())
        {
            // An extraction-shaped tool whose target could not be located
            // is unclassifiable -- fail closed, never allow-blind.
            ack.verdict       = "deny";
            ack.reason_code   = static_cast<i64>(adapter::hook_reason_payload);
            ack.reason_detail = "tool '" + tool +
                                "' exposed no file_path - target unverifiable, fail closed";
            append_event(*m_journal, "hook_deny_no_target", tool, now_ms);
            return ack;
        }
        {
            auto profile             = cached_profile(); // boot-cached; no disk under the mutex
            const std::string target = normalize_hook_path(request.file_path);
            if (target.find("..") != std::string::npos)
            {
                governed = true;
                detail   = "traversal form rejected: " + request.file_path;
            }
            else if (!profile.is_ok())
            {
                // The accepted profile is cached at boot; a cache miss here
                // is an uncertainty on the mediated path — FAIL CLOSED
                // (never allow-blind; the unknown-tool/no-target branches
                // deny for the same reason).
                ack.verdict     = "deny";
                ack.reason_code = static_cast<i64>(adapter::hook_reason_payload);
                ack.reason_detail =
                    "accepted profile unavailable - target unverifiable, fail closed";
                append_event(*m_journal, "hook_deny_profile_unavailable", tool, now_ms);
                return ack;
            }
            else
            {
                // 2026-09-26 simulated-gate finding (first dev run): the
                // old match required the governed path to be followed by
                // "/", so an absolute target naming a governed FILE
                // exactly (state/current.md) never matched -- only
                // children of DIR entries did. A governed file is now
                // matched exactly (relative form) and by its absolute
                // form inside the governed ROOT (the suffix bound is the
                // root prefix: an outside tree that repeats the relative
                // suffix is not governed by exact-file matching; the
                // pre-existing containment-anywhere clauses stay as the
                // documented over-approximation). Evidence: the rig's
                // B2/B6/B7 old-fail receipts vs the post-fix runs.
                const std::string root_norm = normalize_hook_path(m_repo_root.string());
                const bool in_root          = starts_with_ci(target, root_norm + "/");
                for (const auto& path : profile.value().governed_paths)
                {
                    if (target == path || target.rfind(path + "/", 0) == 0 ||
                        (in_root && ends_with_ci(target, "/" + path)) ||
                        target.rfind("/" + path + "/", 0) != std::string::npos ||
                        contains_ci(target, path + "/"))
                    {
                        governed = true;
                        detail   = "write target is inside the governed scope: " + path;
                        break;
                    }
                }
            }
        }
    }
    else if (extraction == "command")
    {
        if (request.command.empty())
        {
            ack.verdict       = "deny";
            ack.reason_code   = static_cast<i64>(adapter::hook_reason_payload);
            ack.reason_detail = "tool '" + tool +
                                "' exposed no command - target unverifiable, fail closed";
            append_event(*m_journal, "hook_deny_no_target", tool, now_ms);
            return ack;
        }
        {
            auto profile = cached_profile();
            if (profile.is_ok())
            {
                for (const auto& path : profile.value().governed_paths)
                {
                    if (contains_ci(request.command, path))
                    {
                        governed = true;
                        detail   = "command references governed scope '" + path +
                                 "' (conservative detector: over-approximates to deny)";
                        break;
                    }
                }
                if (!governed &&
                    contains_ci(request.command,
                                normalize_hook_path(m_repo_root.string())))
                {
                    governed = true;
                    detail   = "command references the governed checkout root "
                               "(conservative detector)";
                }
            }
        }
    }

    // Host-assigned action identity (unique per call: session ids never
    // repeat and the counter is per session -- exit gate row 4).
    const SortableId128 action = m_minter.next();
    const std::string action_hex =
        cognition::hex_lower(std::span<const std::byte>(action.bytes.data(), action.bytes.size()));
    ack.action_id = action_hex;

    journal::TransactionOpen open;
    open.correlation = session.id_hex + "|" + action_hex + "|" + tool;
    open.request_digest.sha256 =
        cognition::digest_of(request.payload_sha256 + "|" + request.tool_name + "|" +
                             request.command + "|" + request.file_path)
            .sha256;
    auto transaction = m_journal->open_transaction(open, now_ms);
    if (!transaction.is_ok())
    {
        return ipc::make_error(request.request_id, ipc::err_host_recovering,
                               "judgment journaling failed: " + transaction.reason().message);
    }

    if (governed && freshness_expired)
    {
        // The freshness LAW gates GOVERNED mutations (ARCH section 7.4 /
        // design section 6): not_governed telemetry stays available.
        ack.verdict       = "deny";
        ack.reason_code   = static_cast<i64>(hook_reason_cognition_expired);
        ack.reason_detail = "cognition freshness window exceeded and the last refresh failed "
                            "(ARCH section 7.4: governed mutations are denied)";
        append_event(*m_journal, "hook_deny_expired_cognition", tool, now_ms);
        return ack;
    }
    if (governed)
    {
        // Raw-tool governed writes are denied until the mediated typed path
        // exists (MVP-5); the deny carries the re-deliberate context shape.
        const i32 reason =
            extraction == "command" ? hook_reason_bash_reference : hook_reason_governed_write;
        ack.verdict       = "deny";
        ack.reason_code   = static_cast<i64>(reason);
        ack.reason_detail = detail + "; governed writes use the mediated record path "
                                     "(qiven-record; arrives with MVP-5) -- re-deliberate there";
        append_event(*m_journal, "hook_deny_governed",
                     std::to_string(reason) + "|" + detail + "|" + action_hex, now_ms);
        return ack;
    }

    // not_governed allow: bind + consume a single-use decision at admit
    // (the allow is consumed the moment ZCode is told "allow"; the journal
    // state machine carries single-use -- DESIGN section 3.4 delta 5).
    journal::DecisionBind bind;
    bind.id = m_minter.next();
    bind.token_hash.value =
        cognition::digest_of(action_hex + "|" + request.payload_sha256).sha256;
    bind.transaction    = transaction.value();
    bind.generation     = RuntimeGenerationId { m_generation };
    bind.binding_digest = open.request_digest;
    bind.expires_ms     = now_ms + hook_default_decision_ttl_ms;
    if (auto bound = m_journal->bind_decision(bind, now_ms); !bound.is_ok())
    {
        return ipc::make_error(request.request_id, ipc::err_host_recovering,
                               "decision binding failed: " + bound.reason().message);
    }
    if (auto consumed = m_journal->consume_decision(bind.id, now_ms); !consumed.is_ok())
    {
        return ipc::make_error(request.request_id, ipc::err_host_recovering,
                               "decision consumption failed: " + consumed.reason().message);
    }

    HookSession::Outstanding outstanding;
    outstanding.action_hex  = action_hex;
    outstanding.transaction = transaction.value();
    session.outstanding.emplace(tool, outstanding);

    ack.verdict       = "not_governed";
    ack.reason_detail = "action outside the governed write scope (observed, correlated)";
    append_event(*m_journal, "hook_admit", action_hex + "|" + tool, now_ms);
    return ack;
}

ipc::Reply RuntimeHost::handle_post_tool(const ipc::Request& request, u64 now_ms)
{
    using adapter::hook_reason_unknown_session;

    ipc::Reply ack;
    ack.kind       = ipc::Reply::Kind::HookAck;
    ack.request_id = request.request_id;
    ack.generation = m_generation;

    // First contact mints (LL-2a); an outcome for a never-seen harness
    // session is recorded as an honest unmatched observation.
    HookSession* session_ptr = ensure_session(request, now_ms);
    if (session_ptr == nullptr)
    {
        ack.verdict       = "degraded";
        ack.reason_code   = static_cast<i64>(adapter::hook_reason_correlation);
        ack.reason_detail = "session unavailable; outcome unobserved (transient)";
        return ack;
    }
    HookSession& session = *session_ptr;
    ack.session_id       = session.id_hex;
    ack.refresh          = refresh_state_now(now_ms); // informational

    auto outstanding = session.outstanding.find(request.tool_name);
    if (outstanding == session.outstanding.end())
    {
        // Unmatched post: Indeterminate, typed 115-class -- never guessed.
        ack.verdict       = "degraded";
        ack.reason_code   = static_cast<i64>(adapter::hook_reason_correlation);
        ack.reason_detail = "no outstanding pre observation for '" + request.tool_name +
                            "' -- outcome Indeterminate";
        append_event(*m_journal, "hook_outcome_unmatched",
                     request.tool_name, now_ms);
        return ack;
    }

    const SortableId128 action = m_minter.next();
    ack.action_id              = cognition::hex_lower(
        std::span<const std::byte>(action.bytes.data(), action.bytes.size()));
    append_event(*m_journal, "hook_outcome",
                 outstanding->second.action_hex + "|" + request.tool_name + "|" +
                     request.payload_sha256,
                 now_ms);
    session.outstanding.erase(outstanding);
    ack.verdict       = "allow";
    ack.reason_detail = "outcome correlated; transaction closed";
    return ack;
}

bool RuntimeHost::refresh_cognition(u64 now_ms, std::string& refresh_state,
                                    std::string& detail)
{
    // HOST-AUTONOMOUS attempt body (§6; called ONLY by the refresh worker —
    // no request path reaches this). The network fetch and the bundle
    // publish file work run OUTSIDE the state mutex; the generation swap
    // and all journal writes run under it (§5 locking: a verdict never
    // waits behind publish I/O; the swap is millisecond-scale).
    refresh_state = "current";
    detail.clear();

    bool fetched = false;
    if (!m_repo_root.empty() && !m_git_executable.empty())
    {
        processx::ProcessSpec spec;
        spec.executable  = m_git_executable;
        spec.argv        = { "git", "-C", m_repo_root.string(), "fetch", "--quiet", "origin",
                             "refs/heads/main" };
        spec.working_dir = m_repo_root;
        spec.deadline_ms = 5000;
        processx::ProcessRunner runner;
        auto run = runner.run(spec);
        fetched  = run.is_ok() && run.value().exit_code == 0;
        if (!fetched && run.is_ok())
        {
            detail = "fetch exit " + std::to_string(run.value().exit_code);
        }
        else if (!fetched)
        {
            detail = "fetch failed: " + run.reason().message;
        }
    }

    std::optional<cognition::PublishResult> published_outcome;
    if (fetched)
    {
        // Publish from the remote-tracking ref (the fetched canonical
        // state) — file work outside the state mutex (single worker path).
        processx::ProcessRunner runner;
        cognition::PublishRequest publish_request;
        publish_request.repo_root    = m_repo_root;
        publish_request.ref          = "refs/remotes/origin/main";
        publish_request.runtime_root = m_repo_root / ".qiven" / "runtime";
        auto profile                 = load_profile_file(m_profile_file);
        if (profile.is_ok())
        {
            publish_request.repository_url = profile.value().cognition.repository;
            publish_request.policy_path    = profile.value().cognition.policy_path;
            publish_request.source_paths   = profile.value().cognition.source_paths;
        }
        publish_request.publisher_build = m_build_id;
        publish_request.now_ms          = now_ms;
        publish_request.git_executable  = m_git_executable;
        publish_request.runner          = &runner;
        cognition::CanonicalCognitionPublisher publisher;
        auto published = publisher.publish(publish_request);
        if (published.is_ok())
        {
            published_outcome = published.value();
        }
        else
        {
            detail = "publish after fetch failed: " + published.reason().detail;
        }
    }

    // Everything below touches journal/host state: one mutex scope.
    std::lock_guard<std::mutex> guard(m_state_mutex);
    if (published_outcome.has_value())
    {
        // Only a CONTENT change advances the generation.
        if (published_outcome->bundle_digest != m_active_bundle_digest)
        {
            const ContentDigest profile_digest = cognition::digest_of(
                [&]() {
                    std::ifstream in(m_profile_file, std::ios::binary);
                    return std::string((std::istreambuf_iterator<char>(in)),
                                       std::istreambuf_iterator<char> {});
                }());
            auto generation = activate_generation(*m_journal, published_outcome->bundle_digest,
                                                  profile_digest, m_build_id, now_ms);
            if (generation.is_ok())
            {
                m_generation           = generation.value().value;
                m_bundle_revision      = published_outcome->manifest.source_revision;
                m_active_bundle_digest = published_outcome->bundle_digest;
            }
            else
            {
                detail = "generation activation failed: " + generation.reason().message;
            }
        }
        if (!detail.empty() && detail.rfind("generation activation failed", 0) == 0)
        {
            // The publish succeeded but the generation did NOT advance: the
            // ACTIVE bundle is stale. Report degraded honestly — never mask
            // it behind a fresh freshness clock; the next cadence retries.
            refresh_state = "degraded";
            append_event(*m_journal, "refresh_activation_failed",
                         m_bundle_revision + "|" + detail, now_ms);
            return false;
        }
        m_last_refresh_ok_ms = now_ms;
        (void)m_journal->set_meta("last_refresh_ok_ms", std::to_string(now_ms));
        append_event(*m_journal, "cognition_refreshed", m_bundle_revision, now_ms);
        refresh_state = "current";
        return true;
    }

    // Fetch/publish failed: local fallback inside the freshness window.
    if (m_last_refresh_ok_ms != 0 && m_freshness_window_ms != 0 &&
        now_ms < m_last_refresh_ok_ms + m_freshness_window_ms && m_last_refresh_ok_ms <= now_ms)
    {
        refresh_state = "local_fallback";
        detail        = "fetch failed (" + detail + "); using the verified local bundle";
        append_event(*m_journal, "cognition_local_fallback", detail, now_ms);
        return false;
    }
    refresh_state = "expired";
    detail        = "cognition expired: fetch failed (" + detail +
             ") and the freshness window is exhausted -- governed mutations deny";
    append_event(*m_journal, "cognition_expired", "window-exhausted", now_ms);
    return false;
}
} // namespace qiven::runtime::host
