#include <qiven/runtime/host/runtime_host.hpp>

#include <qiven/runtime/host/generation_activation.hpp>
#include <qiven/runtime/ipc/named_pipe_server.hpp>
#include <qiven/runtime/scope.hpp>

#include <windows.h>

#include <chrono>
#include <fstream>
#include <iterator>
#include <utility>

namespace qiven::runtime::host
{
namespace
{
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
} // namespace

RuntimeHost::~RuntimeHost()
{
    if (m_journal != nullptr)
    {
        // Graceful-shutdown checkpoint (WAL truncate); recovery at next
        // boot is still authoritative (MVP-1 law).
        m_journal->checkpoint();
    }
    m_state = "stopped";
}

qiven::Result<std::unique_ptr<RuntimeHost>> RuntimeHost::boot(const HostBoot& boot)
{
    using HostResult = qiven::Result<std::unique_ptr<RuntimeHost>>;

    // Journal first: the install identity names the singleton and the pipe.
    // First boot creates; later boots open (a corruption never yields a
    // fresh mutating database — the MVP-1 fresh-database guard).
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
    //    identity — the ACCEPTED instance, content-identified).
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

ipc::Reply RuntimeHost::handle(const ipc::Request& request, u64 /*now_ms*/)
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
} // namespace qiven::runtime::host
