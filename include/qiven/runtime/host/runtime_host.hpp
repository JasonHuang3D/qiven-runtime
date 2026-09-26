#pragma once

// ============================================================================
// host/runtime_host.hpp — the production composition root
// (MVP-3 batch design section 3.4; ARCH section 6.1/§13.2; host-server
// redesign — docs/design/mvp4-host-server.md)
//
// RuntimeHost owns the fixed startup order (ROOT-keyed process singleton
// acquired BEFORE any durable touch → journal recovery → installation
// secret/client record → accepted profile → bundle publish/pin from the
// authorized local ref → generation activation → IPC endpoint) and serves
// `status`/`doctor` from boot, while every mutation-kind request is a
// typed HostRecovering (61) denial until the governed pipeline batches
// land (MVP-4/5). Fail-closed: any boot step that fails leaves the host
// Starting — status and doctor serve the failure detail, mutations deny,
// and nothing is guessed.
//
// Host-server redesign semantics (LL-2): sessions mint at FIRST CONTACT of
// ANY event kind (session_start is an advisory manifest carrier, never a
// precondition — deny-114 is retired); cognition refresh runs on a
// HOST-AUTONOMOUS worker (boot + cadence + coalesced operator trigger),
// never on a request path; one state mutex serializes all journal/host
// state (there is no second lock, so there is no lock-ordering surface);
// ALL request handling is thread-safe (ServeLoop serves connections
// concurrently); drain marks outstanding pre observations audit-Indeterminate
// (the journal's terminal-transition command is a pre-MVP-5 hardening item;
// recovery reconciles the open rows at next boot — MVP-1 law).
//
// User-mode only; one host per governed ROOT (root-identity named mutex +
// first pipe instance); shutdown checkpoints the WAL. Time is injected per
// call (no ambient clock; MVP-1 precedent).
// ============================================================================

#include <qiven/result.hpp>
#include <qiven/runtime/cognition/bundle.hpp>
#include <qiven/runtime/cognition/publisher.hpp>
#include <qiven/runtime/host/deployment_profile.hpp>
#include <qiven/runtime/identity.hpp>
#include <qiven/runtime/ipc/protocol.hpp>
#include <qiven/runtime/journal/runtime_journal.hpp>

#include <atomic>
#include <condition_variable>
#include <ctime>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>

namespace qiven::runtime::host
{
inline constexpr i32 err_singleton         = 101;
inline constexpr i32 err_generation        = 102;
inline constexpr i32 err_quarantine        = 103;
inline constexpr i32 err_cognition_expired = 66; // MVP-4 H-2 deny path (IPC 60-69)

struct HostBoot
{
    std::filesystem::path repo_root;    // the governed qiven-context checkout
    std::filesystem::path profile_file; // the accepted profile instance
    std::filesystem::path git_executable;
    std::string build_id;
    u64 now_ms = 0;
};

struct StatusSnapshot
{
    std::string state; // starting | running | draining | stopped
    std::string install_id;
    u64 boot_epoch = 0;
    u64 generation = 0;
    std::string bundle_revision; // git commit oid of the ACTIVE bundle
    u64 journal_events = 0;
    bool quarantined   = false;
    std::string failure_detail; // non-empty while Starting with a failed step
    std::string refresh_state;  // current | local_fallback | expired | degraded
    u64 last_refresh_ok_ms  = 0;
    u64 next_refresh_due_ms = 0;
};

class RuntimeHost
{
public:
    [[nodiscard]] static qiven::Result<std::unique_ptr<RuntimeHost>> boot(const HostBoot& boot);

    [[nodiscard]] StatusSnapshot status() const;
    [[nodiscard]] StatusSnapshot status_locked(u64 now_ms) const; // m_state held
    [[nodiscard]] ipc::Reply doctor() const;
    [[nodiscard]] ipc::Reply doctor_locked() const; // m_state held

    // The authenticated request surface (frames are decoded/verified by
    // the caller; this handles PROTOCOL semantics). Thread-safe: one state
    // mutex serializes all journal/host-state mutations (the ServeLoop
    // serves connections concurrently). Status/doctor always serve; hello
    // serves; mutation → typed 61 while not Running (and stays 61 until
    // the pipeline batches land); refresh triggers the worker and answers
    // immediately (the trigger is a convenience, never a dependency).
    [[nodiscard]] ipc::Reply handle(const ipc::Request& request, u64 now_ms);

    // Starts the host-autonomous refresh worker (boot + cadence + coalesced
    // operator triggers, §6). Called by boot(); exposed for tests that
    // construct the host directly.
    void start_refresh_worker();

    // Graceful drain: outstanding pre observations are audit-marked
    // Indeterminate, the worker stops, and the state flips to draining.
    // The exe calls this after its serve loop has drained.
    void drain(u64 now_ms);

    // Test seam ONLY (registry-eviction row): shrink the idle-eviction
    // bound so the sweep is exercisable in bounded test time. Production
    // uses the default (24 h); nothing else reads this setter.
    void set_session_eviction_bound_for_test(u64 ms) noexcept
    {
        m_session_idle_evict_ms = ms;
    }

    // Test seam ONLY (eviction row): run the sweep directly (the worker
    // cadence is the production driver).
    void evict_idle_sessions_for_test(u64 now_ms)
    {
        std::lock_guard<std::mutex> guard(m_state_mutex);
        evict_idle_sessions(now_ms);
    }

    void request_shutdown() noexcept;

    ~RuntimeHost(); // stop worker + checkpoint_wal + release

    RuntimeHost(const RuntimeHost&)            = delete;
    RuntimeHost& operator=(const RuntimeHost&) = delete;

private:
    RuntimeHost() = default;

    // One registry entry per harness session handle; the host — never the
    // client — owns the runtime session identity and the action counter
    // (ARCH section 12.2). First-contact minting (LL-2a): ANY event mints.
    struct HookSession
    {
        u64 journal_row = 0;                  // open_session row id
        std::string id_hex;                   // rendered runtime session id
        u64 action_next = 1;                  // per-session action counter
        std::set<std::string> degraded_tools; // per-tool 113 scope (LL-2a)
        std::string degraded_detail;
        u64 last_seen_ms = 0; // eviction clock (§5 registry eviction)
        struct Outstanding
        {
            std::string action_hex;
            ControlTransactionId transaction {};
        };
        std::map<std::string, Outstanding> outstanding; // one pre per tool
    };

    [[nodiscard]] ipc::Reply handle_hook_event(const ipc::Request& request, u64 now_ms);
    [[nodiscard]] ipc::Reply handle_session_start(const ipc::Request& request, u64 now_ms);
    [[nodiscard]] ipc::Reply handle_pre_tool(const ipc::Request& request, u64 now_ms);
    [[nodiscard]] ipc::Reply handle_post_tool(const ipc::Request& request, u64 now_ms);
    // First-contact minting: finds or creates the session for a handle.
    [[nodiscard]] HookSession& ensure_session(const ipc::Request& request, u64 now_ms);
    [[nodiscard]] bool refresh_cognition(u64 now_ms, std::string& refresh_state,
                                         std::string& detail);
    void refresh_worker_body();
    [[nodiscard]] bool refresh_expired(u64 now_ms) const;
    [[nodiscard]] std::string refresh_state_now(u64 now_ms) const;
    void evict_idle_sessions(u64 now_ms);
    // The accepted profile CACHED at boot: static configuration — request
    // paths must not touch the disk under the state mutex.
    [[nodiscard]] qiven::Result<ProfileFile, port::ProfileAcceptError> cached_profile() const
    {
        if (m_profile_cache.has_value())
        {
            return qiven::Result<ProfileFile, port::ProfileAcceptError>(*m_profile_cache);
        }
        return load_profile_file(m_profile_file);
    }

    journal::RuntimeJournal* journal() const noexcept
    {
        return m_journal.get();
    }

    std::unique_ptr<journal::RuntimeJournal> m_journal;
    std::string m_install_id;
    u64 m_boot_epoch = 0;
    u64 m_generation = 0;
    std::string m_bundle_revision;
    std::string m_build_id;
    std::string m_state = "starting";
    std::string m_failure_detail;
    bool m_quarantined      = false;
    void* m_singleton_mutex = nullptr; // named-mutex HANDLE, held for life
    // MVP-4 members
    std::optional<ProfileFile> m_profile_cache;
    std::filesystem::path m_repo_root;
    std::filesystem::path m_profile_file;
    std::filesystem::path m_git_executable;
    ContentDigest m_active_bundle_digest {};
    SortableIdMinter m_minter; // session/decision ids (never-repeating)
    std::map<std::string, HookSession> m_hook_sessions;
    u64 m_last_refresh_ok_ms    = 0; // last successful fetch/publish
    u64 m_freshness_window_ms   = 0;
    u64 m_refresh_interval_ms   = 900'000;    // profile revision 3 cadence
    u64 m_refresh_coalesce_ms   = 30'000;     // min interval between attempts
    u64 m_session_idle_evict_ms = 86'400'000; // §5 registry eviction bound
    bool m_shutting_down        = false;
    // The ONE state mutex: request handling, status, eviction, and the
    // refresh swap all serialize here (no second lock → no lock ordering).
    mutable std::mutex m_state_mutex;
    // Refresh worker (host-autonomous, off every request path).
    std::thread m_refresh_worker;
    std::mutex m_refresh_mutex;
    std::condition_variable m_refresh_cv;
    bool m_refresh_pending      = false; // coalesced operator trigger
    bool m_worker_running       = false;
    u64 m_last_attempt_ms       = 0;
    std::string m_refresh_state = "current";
};

// Wall clock in ms UTC for callers that need "now" (journal-comparable).
[[nodiscard]] inline u64 wall_now_ms() noexcept
{
    return static_cast<u64>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}
} // namespace qiven::runtime::host
