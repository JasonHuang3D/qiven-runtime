// ============================================================================
// bundle_publish_pin — MVP-2 exit gates 1/2/3/5 against REAL git fixtures
// (batch design section 6). Gate 4 (generation staleness) lives in
// generation_stale.cpp.
//
//   gate 1: a dirty checkout cannot affect a published/pinned bundle;
//   gate 2: modifying any bundle file makes loading fail;
//   gate 3: git revision and content digest are separately visible;
//   gate 5: rich records ride byte-exact, never lossy-converted into the
//           v8 snapshot.
// ============================================================================

#include <qiven/contracts.hpp>
#include <qiven/runtime/cognition/bundle.hpp>
#include <qiven/runtime/cognition/policy.hpp>
#include <qiven/runtime/cognition/publisher.hpp>
#include <qiven/runtime/port/cognition_port.hpp>
#include <qiven/runtime/processx/process_runner.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
using qiven::runtime::ContentDigest;
using qiven::runtime::cognition::BundleStore;
using qiven::runtime::cognition::CanonicalCognitionPublisher;
using qiven::runtime::cognition::PublishRequest;
using qiven::runtime::port::BundleErrorKind;
using qiven::runtime::processx::ProcessRunner;
using qiven::runtime::processx::ProcessSpec;

const char* policy_yaml =
    "schema: qiven-invocation-policy-v1\n"
    "version: 1\n"
    "policy:\n"
    "  present: true\n"
    "  rules:\n"
    "    - action: CreateCppSymbol\n"
    "      requirement: MandatoryRecall\n"
    "      boundary: BeforeJudgment\n"
    "      subject: naming policy\n"
    "      blocking: true\n"
    "    - action: Publish\n"
    "      requirement: RequestReview\n"
    "      boundary: BeforeExecution\n"
    "      subject: H2 exact-delta review\n"
    "      blocking: true\n"
    "resolvers:\n"
    "  - requirement: MandatoryRecall\n"
    "    type: snapshot_resolver\n"
    "    min_version: 1\n"
    "  - requirement: RequestReview\n"
    "    type: review_receipt_resolver\n"
    "    min_version: 1\n"
    "freshness:\n"
    "  evidence_ttl_ms: 900000\n"
    "  bundle_freshness_ms: 604800000\n"
    "enforcement:\n"
    "  unsatisfied_before_judgment: redeliberate\n"
    "  unsatisfied_before_execution: deny\n";

// A rich record the frozen v4 Snapshot cannot represent losslessly: YAML
// front matter with fields no v4 MemoryRecord carries.
const char* rich_memory =
    "---\n"
    "id: MEM-20260923T000000Z-000000\n"
    "title: A rich record with fields v4 cannot represent\n"
    "status: active\n"
    "mood: contemplative\n"
    "---\n"
    "# Rich body\n\n"
    "Prose with `code` and - lists that a lossy conversion would flatten.\n";

const char* rich_adr =
    "---\n"
    "id: ADR-9999\n"
    "status: accepted\n"
    "---\n"
    "# A rich decision record\n\n"
    "## Context\n\nRich markdown context.\n";

void write_file(const std::filesystem::path& file, const std::string& bytes)
{
    std::filesystem::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << bytes;
}

std::string read_file(const std::filesystem::path& file)
{
    std::ifstream in(file, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char> {});
}

// Run git in the fixture repo through the SAME bounded runner the
// publisher uses (no shell anywhere in this test's plumbing).
bool git(const ProcessRunner& runner, const std::filesystem::path& repo,
         std::vector<std::string> args, std::string* out = nullptr)
{
    ProcessSpec spec;
    spec.executable = R"(C:\Program Files\Git\cmd\git.exe)";
    spec.argv       = { "git", "-C", repo.string() };
    spec.argv.insert(spec.argv.end(), args.begin(), args.end());
    spec.working_dir = repo;
    spec.deadline_ms = 30'000;
    auto run         = runner.run(spec);
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

PublishRequest make_request(const std::filesystem::path& repo,
                            const std::filesystem::path& runtime_root, const std::string& ref,
                            const ProcessRunner& runner)
{
    PublishRequest request;
    request.repo_root       = repo;
    request.ref             = ref;
    request.runtime_root    = runtime_root;
    request.repository_url  = "https://example.invalid/qiven-context.git";
    request.policy_path     = "runtime/invocation-policy.yaml";
    request.source_paths    = { "runtime/invocation-policy.yaml", "memory/index.yaml",
                                "memory/records", "decisions" };
    request.publisher_build = "test-build";
    request.now_ms          = 1'800'000'000'000;
    request.git_executable  = R"(C:\Program Files\Git\cmd\git.exe)";
    request.runner          = &runner;
    return request;
}
} // namespace

int main()
{
    const std::filesystem::path workroot = QIVEN_RUNTIME_TEST_WORKROOT;
    const std::filesystem::path repo     = workroot / "bundle-publish-pin" / "repo";
    const std::filesystem::path runtime_root =
        workroot / "bundle-publish-pin" / "runtime";
    std::filesystem::remove_all(workroot / "bundle-publish-pin");
    std::filesystem::create_directories(repo);
    std::filesystem::create_directories(runtime_root);

    const ProcessRunner runner;

    // --- fixture: a real repository with one content commit ---------------
    QIVEN_VERIFY(git(runner, repo, { "init", "-q", "--initial-branch=main" }));
    QIVEN_VERIFY(git(runner, repo, { "config", "user.email", "test@qiven.invalid" }));
    QIVEN_VERIFY(git(runner, repo, { "config", "user.name", "Qiven Test" }));
    write_file(repo / "runtime" / "invocation-policy.yaml", policy_yaml);
    write_file(repo / "memory" / "index.yaml", "schema_version: 1\nrecords: []\n");
    write_file(repo / "memory" / "records" / "MEM-20260923T000000Z-000000.md", rich_memory);
    write_file(repo / "decisions" / "ADR-9999.md", rich_adr);
    QIVEN_VERIFY(git(runner, repo, { "add", "-A" }));
    QIVEN_VERIFY(git(runner, repo, { "commit", "-q", "-m", "fixture content" }));

    std::string commit_oid;
    QIVEN_VERIFY(git(runner, repo, { "rev-parse", "HEAD" }, &commit_oid));
    commit_oid = trim(commit_oid);

    // --- publish from the exact commit ------------------------------------
    CanonicalCognitionPublisher publisher;
    auto first = publisher.publish(make_request(repo, runtime_root, commit_oid, runner));
    QIVEN_VERIFY(first.is_ok());
    const std::string first_revision = first.value().manifest.source_revision;
    QIVEN_VERIFY(first_revision == commit_oid);

    BundleStore store(runtime_root);
    auto active = store.load_active();
    QIVEN_VERIFY(active.is_ok());

    // Gate 1: dirty the working tree; the pinned generation is unaffected.
    write_file(repo / "memory" / "records" / "MEM-20260923T000000Z-000000.md",
               "DIRTY CONTENT - uncommitted edit\n");
    write_file(repo / "memory" / "records" / "UNTRACKED.md", "untracked noise\n");
    {
        auto republish = publisher.publish(make_request(repo, runtime_root, commit_oid, runner));
        QIVEN_VERIFY(republish.is_ok());
        QIVEN_VERIFY(republish.value().manifest.source_revision == commit_oid);
        // Content identity is commit-deterministic: every member digest is
        // identical to the first publish.
        QIVEN_VERIFY(republish.value().manifest.snapshot_digest ==
                     first.value().manifest.snapshot_digest);
        QIVEN_VERIFY(republish.value().manifest.policy_digest ==
                     first.value().manifest.policy_digest);
        QIVEN_VERIFY(republish.value().manifest.source_files.size() ==
                     first.value().manifest.source_files.size());
        bool dirty_file_in_bundle = false;
        for (const auto& source : republish.value().manifest.source_files)
        {
            if (source.path.find("UNTRACKED") != std::string::npos)
            {
                dirty_file_in_bundle = true;
            }
        }
        QIVEN_VERIFY(!dirty_file_in_bundle);
        // The rich record's digest still matches the COMMITTED bytes.
        std::string committed_rich;
        QIVEN_VERIFY(git(runner, repo,
                         { "cat-file", "blob",
                           commit_oid + ":memory/records/MEM-20260923T000000Z-000000.md" },
                         &committed_rich));
        const std::filesystem::path bundled_rich =
            republish.value().bundle_dir / "source" / "memory" / "records" /
            "MEM-20260923T000000Z-000000.md";
        QIVEN_VERIFY(read_file(bundled_rich) == committed_rich);
    }

    // Gate 3: revision identity and content identity are separately visible.
    {
        // A second commit with the SAME tree: content digests identical,
        // source_revision distinct.
        QIVEN_VERIFY(git(runner, repo, { "commit", "-q", "--allow-empty", "-m", "empty" }));
        std::string second_oid;
        QIVEN_VERIFY(git(runner, repo, { "rev-parse", "HEAD" }, &second_oid));
        second_oid = trim(second_oid);
        QIVEN_VERIFY(second_oid != commit_oid);
        auto second = publisher.publish(make_request(repo, runtime_root, second_oid, runner));
        QIVEN_VERIFY(second.is_ok());
        QIVEN_VERIFY(second.value().manifest.source_revision == second_oid);
        QIVEN_VERIFY(second.value().manifest.snapshot_digest ==
                     first.value().manifest.snapshot_digest);
        QIVEN_VERIFY(second.value().manifest.policy_digest ==
                     first.value().manifest.policy_digest);

        // The pinned cognition carries BOTH, distinctly.
        BundleStore active_store(runtime_root);
        auto pin = active_store.pin_from_bundle(second.value().manifest);
        QIVEN_VERIFY(pin.is_ok());
        QIVEN_VERIFY(pin.value().pinned.source_revision == second_oid);
        QIVEN_VERIFY(pin.value().pinned.revision.value.rfind("sha256:", 0) == 0);
        QIVEN_VERIFY(pin.value().pinned.source_revision !=
                     pin.value().pinned.revision.value);
    }

    // Gate 2: modifying any bundle file makes loading fail.
    {
        const auto bundle_dir = store.active_digest();
        QIVEN_VERIFY(bundle_dir.is_ok());
        const std::filesystem::path dir =
            runtime_root / "bundles" / qiven::runtime::cognition::hex_lower(bundle_dir.value().sha256);

        const std::filesystem::path member =
            dir / "source" / "memory" / "index.yaml";
        const std::string original = read_file(member);
        write_file(member, original + "tampered\n");
        auto broken = store.load_active();
        QIVEN_VERIFY(!broken.is_ok());
        QIVEN_VERIFY(broken.reason().kind == BundleErrorKind::DigestMismatch);
        write_file(member, original);

        const std::filesystem::path snapshot = dir / "snapshot.qvs";
        const std::string snapshot_original  = read_file(snapshot);
        write_file(snapshot, snapshot_original + "x");
        broken = store.load_active();
        QIVEN_VERIFY(!broken.is_ok());
        QIVEN_VERIFY(broken.reason().kind == BundleErrorKind::DigestMismatch);
        write_file(snapshot, snapshot_original);

        const std::filesystem::path policy_member = dir / "policy.yaml";
        const std::string policy_original         = read_file(policy_member);
        write_file(policy_member, policy_original + "x");
        broken = store.load_active();
        QIVEN_VERIFY(!broken.is_ok());
        QIVEN_VERIFY(broken.reason().kind == BundleErrorKind::DigestMismatch);
        write_file(policy_member, policy_original);

        // A manifest flip breaks the directory-name binding itself.
        const std::filesystem::path manifest = dir / "manifest.json";
        const std::string manifest_original  = read_file(manifest);
        write_file(manifest, manifest_original + " ");
        broken = store.load_active();
        QIVEN_VERIFY(!broken.is_ok());
        QIVEN_VERIFY(broken.reason().kind == BundleErrorKind::DigestMismatch);
        write_file(manifest, manifest_original);

        // Restored: the bundle loads again.
        QIVEN_VERIFY(store.load_active().is_ok());
    }

    // Gate 5: rich records are NOT lossy-converted into the v8 snapshot.
    {
        auto verified = store.load_active();
        QIVEN_VERIFY(verified.is_ok());
        // Materialize through the frozen draft reader (the runtime never
        // reimplements it) and prove the snapshot is the policy-only floor.
        qiven::runtime::port::DraftSnapshotReader reader;
        qiven::runtime::port::PinRequest pin_request;
        pin_request.file = verified.value().dir / "snapshot.qvs";
        auto pinned      = reader.pin(pin_request);
        QIVEN_VERIFY(pinned.is_ok());
        QIVEN_VERIFY(pinned.value().snapshot != nullptr);
        QIVEN_VERIFY(pinned.value().snapshot->memory.empty());
        QIVEN_VERIFY(pinned.value().snapshot->decisions.empty());
        QIVEN_VERIFY(pinned.value().snapshot->invocation.present);
        QIVEN_VERIFY(pinned.value().snapshot->invocation.rules.size() == 2);
        // The rich records ride as byte-exact source files (proven in the
        // gate-1 block for the memory record; the ADR here).
        std::string committed_adr;
        QIVEN_VERIFY(git(runner, repo,
                         { "cat-file", "blob", commit_oid + ":decisions/ADR-9999.md" },
                         &committed_adr));
        QIVEN_VERIFY(read_file(verified.value().dir / "source" / "decisions" / "ADR-9999.md") ==
                     committed_adr);
    }

    // Publisher fail-closed: an unresolvable ref denies.
    {
        auto denied = publisher.publish(make_request(repo, runtime_root, "deadbeef", runner));
        QIVEN_VERIFY(!denied.is_ok());
    }
    // Publisher fail-closed: a policy path absent from the commit denies.
    {
        auto request        = make_request(repo, runtime_root, commit_oid, runner);
        request.policy_path = "no/such/policy.yaml";
        auto denied         = publisher.publish(request);
        QIVEN_VERIFY(!denied.is_ok());
    }

    std::printf("[ OK ] bundle-publish-pin\n");
    return 0;
}
