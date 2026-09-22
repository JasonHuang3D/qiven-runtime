#pragma once

// ============================================================================
// host/runtime_host.hpp — the production composition root
// (MVP-3 batch design section 3.4; ARCH section 6.1/§13.2)
//
// RuntimeHost owns the fixed startup order (process singleton → journal
// recovery → installation secret/client record → accepted profile →
// bundle publish/pin from the authorized local ref → generation
// activation → IPC endpoint) and serves `status`/`doctor` from boot,
// while every mutation-kind request is a typed HostRecovering (61)
// denial until the governed pipeline batches land (MVP-4/5). Fail-closed:
// any boot step that fails leaves the host Starting — status and doctor
// serve the failure detail, mutations deny, and nothing is guessed.
//
// User-mode only; one host per installation (named mutex + first pipe
// instance); shutdown checkpoints the WAL. Time is injected per call
// (no ambient clock; MVP-1 precedent).
// ============================================================================

#include <qiven/result.hpp>
#include <qiven/runtime/cognition/bundle.hpp>
#include <qiven/runtime/cognition/publisher.hpp>
#include <qiven/runtime/host/deployment_profile.hpp>
#include <qiven/runtime/identity.hpp>
#include <qiven/runtime/ipc/protocol.hpp>
#include <qiven/runtime/journal/runtime_journal.hpp>

#include <ctime>
#include <filesystem>
#include <memory>
#include <string>

namespace qiven::runtime::host
{
inline constexpr i32 err_singleton  = 101;
inline constexpr i32 err_generation = 102;
inline constexpr i32 err_quarantine = 103;

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
};

class RuntimeHost
{
public:
    [[nodiscard]] static qiven::Result<std::unique_ptr<RuntimeHost>> boot(const HostBoot& boot);

    [[nodiscard]] StatusSnapshot status() const;
    [[nodiscard]] ipc::Reply doctor() const;

    // The authenticated request surface (frames are decoded/verified by
    // the caller; this handles PROTOCOL semantics). Status/doctor always
    // serve; hello serves; mutation → typed 61 while not Running (and
    // stays 61 in MVP-3 until the pipeline batches land).
    [[nodiscard]] ipc::Reply handle(const ipc::Request& request, u64 now_ms);

    void request_shutdown() noexcept;

    ~RuntimeHost(); // checkpoint_wal + release

    RuntimeHost(const RuntimeHost&)            = delete;
    RuntimeHost& operator=(const RuntimeHost&) = delete;

private:
    RuntimeHost() = default;

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
