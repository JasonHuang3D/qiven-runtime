// ============================================================================
// host_lifecycle — RuntimeHost composition root against a REAL workroot:
// fixed startup order, singleton denial (101), HostRecovering mutation
// denial (61), status transitions, doctor verification, clean shutdown
// (journal reopens), and one full authenticated pipe round trip.
// ============================================================================

#include <qiven/contracts.hpp>
#include <qiven/runtime/cognition/policy.hpp>
#include <qiven/runtime/host/runtime_host.hpp>
#include <qiven/runtime/ipc/framing.hpp>
#include <qiven/runtime/ipc/named_pipe_server.hpp>
#include <qiven/runtime/ipc/protocol.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace
{
using qiven::runtime::host::HostBoot;
using qiven::runtime::host::RuntimeHost;

void write_file(const std::filesystem::path& file, const std::string& bytes)
{
    std::filesystem::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << bytes;
}

const char* fixture_policy =
    "schema: qiven-invocation-policy-v1\n"
    "version: 1\n"
    "policy:\n"
    "  present: true\n"
    "  rules:\n"
    "    - action: Commit\n"
    "      requirement: RunMechanicalCheck\n"
    "      boundary: BeforeExecution\n"
    "      subject: attribution lint\n"
    "      blocking: true\n"
    "resolvers:\n"
    "  - requirement: RunMechanicalCheck\n"
    "    type: mechanical_check_runner\n"
    "    min_version: 1\n"
    "freshness:\n"
    "  evidence_ttl_ms: 900000\n"
    "  bundle_freshness_ms: 604800000\n"
    "enforcement:\n"
    "  unsatisfied_before_judgment: redeliberate\n"
    "  unsatisfied_before_execution: deny\n";

bool git(const std::filesystem::path& repo, std::initializer_list<const char*> args,
         std::string* out = nullptr)
{
    qiven::runtime::processx::ProcessSpec spec;
    spec.executable = R"(C:\Program Files\Git\cmd\git.exe)";
    spec.argv       = { "git", "-C", repo.string() };
    for (const char* arg : args)
    {
        spec.argv.push_back(arg);
    }
    spec.working_dir = repo;
    qiven::runtime::processx::ProcessRunner runner;
    auto run = runner.run(spec);
    if (!run.is_ok() || run.value().exit_code != 0)
    {
        return false;
    }
    if (out != nullptr)
    {
        *out = run.value().out;
    }
    return true;
}

std::string trim(std::string value)
{
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r'))
    {
        value.pop_back();
    }
    return value;
}
} // namespace

int main()
{
    const std::filesystem::path workroot = QIVEN_RUNTIME_TEST_WORKROOT;
    const std::filesystem::path case_dir = workroot / "host-lifecycle";
    std::error_code ec;
    std::filesystem::remove_all(case_dir, ec);
    const std::filesystem::path repo = case_dir / "repo";
    std::filesystem::create_directories(repo);

    // Fixture: a git repo whose policy file matches the test profile pin.
    write_file(repo / "runtime" / "invocation-policy.yaml", fixture_policy);
    write_file(repo / "memory" / "index.yaml", "schema_version: 1\nrecords: []\n");
    QIVEN_VERIFY(git(repo, { "init", "-q", "--initial-branch=main" }));
    QIVEN_VERIFY(git(repo, { "config", "user.email", "test@qiven.invalid" }));
    QIVEN_VERIFY(git(repo, { "config", "user.name", "Qiven Test" }));
    QIVEN_VERIFY(git(repo, { "add", "-A" }));
    QIVEN_VERIFY(git(repo, { "commit", "-q", "-m", "fixture" }));

    // The accepted profile instance for the fixture: source paths cover
    // the policy; the policy digest pins the fixture bytes (computed via
    // the runtime's own digest, cross-checked by the host at boot).
    std::string policy_hex;
    {
        const auto digest =
            qiven::runtime::cognition::digest_of(fixture_policy);
        policy_hex = qiven::runtime::cognition::hex_lower(digest.sha256);
    }
    const std::filesystem::path profile_file = case_dir / "profile.yaml";
    write_file(
        profile_file,
        "schema: qiven-deployment-profile-v1\n"
        "profile_id: host-lifecycle-test\n"
        "revision: 1\n"
        "control_version: 1\n"
        "resolver_registry_revision: 1\n"
        "classifier_contract_revision: 1\n"
        "conformance_evidence: evidence/audits/host-lifecycle.md\n"
        "freshness_window_ms: 604800000\n"
        "governed_paths:\n"
        "  - memory/records\n"
        "actors:\n"
        "  - adapter: 1\n"
        "    session_token: 1\n"
        "    credential_token: 1\n"
        "capabilities:\n"
        "  - id: 1\n"
        "    adapter: 1\n"
        "    operation_class: FileSystemWrite\n"
        "    can_mutate_world: true\n"
        "    can_publish_claims: false\n"
        "    requires_execution_authority: true\n"
        "mediation:\n"
        "  - capability: 1\n"
        "    kind: ActionInterception\n"
        "claims:\n"
        "  tool_mediated: true\n"
        "  free_text: false\n"
        "cognition:\n"
        "  repository: https://example.invalid/qiven-context.git\n"
        "  authorized_ref: refs/heads/main\n"
        "  policy_path: runtime/invocation-policy.yaml\n"
        "  policy_sha256: " +
            policy_hex +
            "\n"
            "  source_paths:\n"
            "    - runtime/invocation-policy.yaml\n"
            "    - memory/index.yaml\n"
            "git_executable: C:/Program Files/Git/cmd/git.exe\n"
            "git_min_version: 2.40\n");

    HostBoot boot;
    boot.repo_root      = repo;
    boot.profile_file   = profile_file;
    boot.git_executable = R"(C:\Program Files\Git\cmd\git.exe)";
    boot.build_id       = "host-lifecycle-test-build";
    boot.now_ms         = 7'000'000;

    // Boot: the fixed startup order yields a Running host with a live
    // generation pinned to the fixture commit.
    auto host = RuntimeHost::boot(boot);
    QIVEN_VERIFY(host.is_ok());
    const auto status = host.value()->status();
    QIVEN_VERIFY(status.state == "running");
    QIVEN_VERIFY(status.generation >= 1);
    std::string head;
    QIVEN_VERIFY(git(repo, { "rev-parse", "HEAD" }, &head));
    QIVEN_VERIFY(status.bundle_revision == trim(head));

    // Doctor: chain verifies, bundle active, no findings.
    {
        auto report = host.value()->doctor();
        QIVEN_VERIFY(report.audit_chain_ok);
        QIVEN_VERIFY(report.bundle_active_ok);
        QIVEN_VERIFY(report.findings.empty());
    }

    // Mutation kind is a typed HostRecovering denial in MVP-3.
    {
        qiven::runtime::ipc::Request request;
        request.kind        = qiven::runtime::ipc::Request::Kind::Mutation;
        request.request_id  = 9;
        request.deadline_ms = 1000;
        auto reply          = host.value()->handle(request, boot.now_ms + 1);
        QIVEN_VERIFY(reply.kind == qiven::runtime::ipc::Reply::Kind::ErrorView);
        QIVEN_VERIFY(reply.error_code == qiven::runtime::ipc::err_host_recovering);
    }

    // Status/doctor through the request surface.
    {
        qiven::runtime::ipc::Request request;
        request.kind       = qiven::runtime::ipc::Request::Kind::Status;
        request.request_id = 10;
        auto reply         = host.value()->handle(request, boot.now_ms + 2);
        QIVEN_VERIFY(reply.kind == qiven::runtime::ipc::Reply::Kind::StatusView);
        QIVEN_VERIFY(reply.state == "running");
    }

    // One full authenticated pipe round trip: create the server under the
    // host's install id, connect as a client, hello + status.
    {
        auto secret = qiven::runtime::ipc::InstallationSecret::ensure(repo / ".qiven" / "runtime");
        QIVEN_VERIFY(secret.is_ok());
        const qiven::runtime::ipc::FrameCodec codec(secret.value());
        auto server = qiven::runtime::ipc::NamedPipeServer::create(status.install_id);
        QIVEN_VERIFY(server.is_ok());

        // Serve side runs on this thread; exercise the client inline.
        auto client = qiven::runtime::ipc::PipeClient::connect(
            qiven::runtime::ipc::pipe_name(status.install_id));
        QIVEN_VERIFY(client.is_ok());
        qiven::runtime::ipc::Request request;
        request.kind        = qiven::runtime::ipc::Request::Kind::Status;
        request.request_id  = 77;
        request.deadline_ms = 3000;
        qiven::runtime::ipc::FrameHeader header;
        header.request_id     = 77;
        header.connection_seq = 1;
        QIVEN_VERIFY(client.value().write_bytes(
            codec.encode(header, qiven::runtime::ipc::encode_request_body(request))));

        auto connection = server.value().accept();
        QIVEN_VERIFY(connection.is_ok());
        auto frame = connection.value().read_frame();
        QIVEN_VERIFY(frame.has_value());
        auto verified = codec.decode(frame.value());
        QIVEN_VERIFY(verified.is_ok());
        auto decoded = qiven::runtime::ipc::decode_request(verified.value().body);
        QIVEN_VERIFY(decoded.is_ok());
        auto reply = host.value()->handle(decoded.value(), boot.now_ms + 3);
        qiven::runtime::ipc::FrameHeader reply_header;
        reply_header.request_id     = verified.value().header.request_id;
        reply_header.connection_seq = 1;
        QIVEN_VERIFY(connection.value().write_bytes(
            codec.encode(reply_header, qiven::runtime::ipc::encode_reply(reply))));

        auto response = client.value().read_frame();
        QIVEN_VERIFY(response.has_value());
        auto verified_reply = codec.decode(response.value());
        QIVEN_VERIFY(verified_reply.is_ok());
        auto parsed = qiven::runtime::ipc::decode_reply(verified_reply.value().body);
        QIVEN_VERIFY(parsed.is_ok());
        QIVEN_VERIFY(parsed.value().kind == qiven::runtime::ipc::Reply::Kind::StatusView);
        QIVEN_VERIFY(parsed.value().state == "running");
        QIVEN_VERIFY(parsed.value().generation == status.generation);
    }

    // Singleton: a second boot of the SAME install (the journal exists)
    // while the first host holds the mutex is denied typed 101.
    {
        boot.now_ms += 10;
        auto second = RuntimeHost::boot(boot);
        QIVEN_VERIFY(!second.is_ok());
        QIVEN_VERIFY(second.reason().code == qiven::runtime::host::err_singleton);
    }

    // Clean shutdown: the host destructs (WAL checkpoint) and the journal
    // reopens clean.
    host.value()->request_shutdown();
    host.value().reset();
    {
        auto reopened = qiven::runtime::journal::RuntimeJournal::open(
            repo / ".qiven" / "runtime" / "journal.sqlite3",
            qiven::runtime::journal::JournalOpenIntent::OpenExisting, boot.now_ms + 20);
        QIVEN_VERIFY(reopened.is_ok());
        QIVEN_VERIFY(reopened.value()->verify_audit_chain().is_ok());
    }

    std::printf("[ OK ] host-lifecycle\n");
    return 0;
}
