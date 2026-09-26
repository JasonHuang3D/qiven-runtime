#include <qiven/runtime/host/runtime_host.hpp>

#include <qiven/runtime/adapter/zcode_hook.hpp>
#include <qiven/runtime/host/generation_activation.hpp>
#include <qiven/runtime/ipc/named_pipe_server.hpp>
#include <qiven/runtime/processx/process_runner.hpp>
#include <qiven/runtime/scope.hpp>

#include <windows.h>

#include <cctype>
#include <chrono>
#include <fstream>
#include <iterator>
#include <locale>
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

qiven::Result<MutexHandle> acquire_singleton(const std::string& install_id)
{
    using MutexResult = qiven::Result<MutexHandle>;
    const std::wstring wide(install_id.begin(), install_id.end());
    const std::wstring name = L"Local\\qiven-runtime-" + wide;
    HANDLE handle           = CreateMutexW(nullptr, TRUE, name.c_str());
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
                                                    "another RuntimeHost owns this installation"));
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

    // Journal first: the install identity names the singleton and the pipe.
    // First boot creates; later boots open (a corruption never yields a
    // fresh mutating database -- the MVP-1 fresh-database guard).
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

    // 2. Process singleton (holds for the host lifetime).
    auto mutex = acquire_singleton(host->m_install_id);
    if (!mutex.is_ok())
    {
        host->m_failure_detail = mutex.reason().message;
        return HostResult::fail(mutex.reason());
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
    //    identity -- the ACCEPTED instance, content-identified).
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

    // MVP-4 members: the hook surface needs the boot facts, the active
    // bundle identity, and the durable freshness clock (last successful
    // publish -- boot publishes from the authorized LOCAL ref; a remote
    // refresh confirms currency at SessionStart, ARCH section 7.4).
    host->m_repo_root            = boot.repo_root;
    host->m_profile_file         = boot.profile_file;
    host->m_git_executable       = boot.git_executable;
    host->m_active_bundle_digest = published.value().bundle_digest;
    host->m_freshness_window_ms  = profile.value().freshness_window_ms;
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
    StatusSnapshot snapshot;
    snapshot.state           = m_state;
    snapshot.install_id      = m_install_id;
    snapshot.boot_epoch      = m_boot_epoch;
    snapshot.generation      = m_generation;
    snapshot.bundle_revision = m_bundle_revision;
    snapshot.quarantined     = m_quarantined;
    snapshot.failure_detail  = m_failure_detail;
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
        const StatusSnapshot snapshot = status();
        ipc::Reply reply;
        reply.kind            = ipc::Reply::Kind::StatusView;
        reply.request_id      = request.request_id;
        reply.state           = snapshot.state;
        reply.install_id      = snapshot.install_id;
        reply.boot_epoch      = snapshot.boot_epoch;
        reply.generation      = snapshot.generation;
        reply.bundle_revision = snapshot.bundle_revision;
        reply.journal_events  = snapshot.journal_events;
        reply.quarantined     = snapshot.quarantined;
        if (!snapshot.failure_detail.empty())
        {
            reply.findings.push_back(snapshot.failure_detail);
        }
        return reply;
    }
    case ipc::Request::Kind::Doctor:
    {
        ipc::Reply reply = doctor();
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
    }
    return ipc::make_error(request.request_id, ipc::err_frame, "unhandled request kind");
}

void RuntimeHost::request_shutdown() noexcept
{
    if (m_state == "running")
    {
        m_state = "draining";
    }
}

// --- MVP-4 hook surface (batch design section 3.4) ---------------------------

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

    // Idempotent registration: ZCode may re-fire SessionStart for one
    // harness session; the runtime identity is minted exactly once.
    auto found = m_hook_sessions.find(request.session_handle);
    if (found != m_hook_sessions.end())
    {
        ack.session_id = found->second.id_hex;
        ack.verdict    = found->second.degraded ? "degraded" : "allow";
        ack.reason_code =
            static_cast<i64>(found->second.degraded ? hook_reason_scope_mismatch : 0);
        ack.reason_detail = found->second.degraded_detail;
        ack.refresh       = m_last_refresh_ok_ms + m_freshness_window_ms > now_ms &&
                              !(m_last_refresh_ok_ms > now_ms)
                                ? "current"
                                : "expired";
        return ack;
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
        return ipc::make_error(request.request_id, ipc::err_host_recovering,
                               "session journaling failed: " + row.reason().message);
    }
    session.journal_row = row.value();

    // Capability handshake: the hook's declared mediated-tools surface must
    // match the profile's tool inventory (fail-closed for affected classes).
    {
        auto profile = load_profile_file(m_profile_file);
        if (!profile.is_ok())
        {
            session.degraded = true;
            session.degraded_detail =
                "profile reload failed: " + profile.reason().detail;
        }
        else
        {
            std::string declared = request.mediated_tools;
            for (const auto& entry : profile.value().tool_inventory)
            {
                const std::string token = entry.tool + ",";
                if (declared.find(entry.tool) == std::string::npos)
                {
                    session.degraded = true;
                    session.degraded_detail +=
                        (session.degraded_detail.empty() ? std::string()
                                                         : std::string("; ")) +
                        "hook manifest lacks declared tool '" + entry.tool + "'";
                }
            }
        }
    }
    // H-2: bounded remote refresh at SessionStart (the only refresh point).
    std::string refresh_state;
    std::string refresh_detail;
    const bool fresh_ok = refresh_cognition(now_ms, refresh_state, refresh_detail);
    (void)fresh_ok;
    if (refresh_state == "expired")
    {
        // Registration stands (activation is advisory), but the degraded
        // reason names the expired cognition so every pre_tool denies 117.
        session.degraded = true;
        session.degraded_detail +=
            (session.degraded_detail.empty() ? std::string() : std::string("; ")) +
            refresh_detail;
    }

    append_event(*m_journal, "session_registered",
                 session.id_hex + "|" + request.session_handle, now_ms);

    ack.session_id    = session.id_hex;
    ack.verdict       = session.degraded ? "degraded" : "allow";
    ack.reason_code   = static_cast<i64>(session.degraded ? hook_reason_scope_mismatch : 0);
    ack.reason_detail = session.degraded_detail;
    ack.refresh       = refresh_state;
    m_hook_sessions.emplace(request.session_handle, std::move(session));
    return ack;
}

ipc::Reply RuntimeHost::handle_pre_tool(const ipc::Request& request, u64 now_ms)
{
    using adapter::hook_reason_bash_reference;
    using adapter::hook_reason_cognition_expired;
    using adapter::hook_reason_correlation;
    using adapter::hook_reason_governed_write;
    using adapter::hook_reason_unknown_session;
    using adapter::hook_reason_unknown_tool;

    ipc::Reply ack;
    ack.kind       = ipc::Reply::Kind::HookAck;
    ack.request_id = request.request_id;
    ack.generation = m_generation;

    auto found = m_hook_sessions.find(request.session_handle);
    if (found == m_hook_sessions.end())
    {
        ack.verdict       = "deny";
        ack.reason_code   = static_cast<i64>(hook_reason_unknown_session);
        ack.reason_detail = "no registered runtime session for this harness session "
                            "(SessionStart must fire first)";
        return ack;
    }
    HookSession& session = found->second;
    ack.session_id       = session.id_hex;
    if (session.degraded)
    {
        // Degraded sessions deny affected tools; the reason carries the
        // degradation detail unless the freshness window expired (117).
        const bool expired =
            m_last_refresh_ok_ms + m_freshness_window_ms <= now_ms && m_freshness_window_ms != 0;
        ack.verdict = "deny";
        ack.reason_code =
            static_cast<i64>(expired ? hook_reason_cognition_expired
                                     : adapter::hook_reason_scope_mismatch);
        ack.reason_detail = session.degraded_detail;
        return ack;
    }

    const std::string tool = request.tool_name;
    const bool freshness_expired =
        m_freshness_window_ms != 0 && m_last_refresh_ok_ms + m_freshness_window_ms <= now_ms;

    // Tool inventory lookup (profile data; unknown tools fail closed).
    std::string extraction;
    std::string detector;
    bool known_tool = false;
    {
        auto profile = load_profile_file(m_profile_file);
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
    if (freshness_expired)
    {
        ack.verdict       = "deny";
        ack.reason_code   = static_cast<i64>(hook_reason_cognition_expired);
        ack.reason_detail = "cognition freshness window exceeded and the last refresh failed "
                            "(ARCH section 7.4: governed mutations are denied)";
        append_event(*m_journal, "hook_deny_expired_cognition", tool, now_ms);
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
            auto profile             = load_profile_file(m_profile_file);
            const std::string target = normalize_hook_path(request.file_path);
            if (target.find("..") != std::string::npos)
            {
                governed = true;
                detail   = "traversal form rejected: " + request.file_path;
            }
            else if (profile.is_ok())
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
            auto profile = load_profile_file(m_profile_file);
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
    ipc::Reply ack;
    ack.kind       = ipc::Reply::Kind::HookAck;
    ack.request_id = request.request_id;
    ack.generation = m_generation;

    auto found = m_hook_sessions.find(request.session_handle);
    if (found == m_hook_sessions.end())
    {
        ack.verdict       = "degraded";
        ack.reason_code   = static_cast<i64>(adapter::hook_reason_unknown_session);
        ack.reason_detail = "outcome for an unregistered session recorded as Indeterminate";
        return ack;
    }
    HookSession& session = found->second;
    ack.session_id       = session.id_hex;

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
    // H-2 (ARCH section 7.4): bounded fetch -> resolve -> validate ->
    // publish -> generation. Fetch failure falls back to the verified
    // local bundle while inside the freshness window; past it, every
    // governed mutation denies (117) and never the dirty checkout.
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

    if (fetched)
    {
        // Publish from the remote-tracking ref (the fetched canonical
        // state); only a CONTENT change advances the generation.
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
            if (published.value().bundle_digest != m_active_bundle_digest)
            {
                const ContentDigest profile_digest = cognition::digest_of(
                    [&]() {
                        std::ifstream in(m_profile_file, std::ios::binary);
                        return std::string((std::istreambuf_iterator<char>(in)),
                                           std::istreambuf_iterator<char> {});
                    }());
                auto generation =
                    activate_generation(*m_journal, published.value().bundle_digest,
                                        profile_digest, m_build_id, now_ms);
                if (generation.is_ok())
                {
                    m_generation           = generation.value().value;
                    m_bundle_revision      = published.value().manifest.source_revision;
                    m_active_bundle_digest = published.value().bundle_digest;
                }
                else
                {
                    detail = "generation activation failed: " + generation.reason().message;
                }
            }
            m_last_refresh_ok_ms = now_ms;
            (void)m_journal->set_meta("last_refresh_ok_ms", std::to_string(now_ms));
            append_event(*m_journal, "cognition_refreshed",
                         m_bundle_revision, now_ms);
            refresh_state = "current";
            return true;
        }
        detail = "publish after fetch failed: " + published.reason().detail;
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
