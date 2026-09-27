// ============================================================================
// source_lock — CA-1 PR-1 unit gates against REAL git fixtures (design
// docs/design/ca1-activation-core.md §2; TCA-ARCH §9.2)
//
// Proves: clean-checkout lock determinism (two builds byte-identical),
// canonical JSON shape, component-wise filter matching, dirty-checkout
// typed rejection, non-HEAD-ref typed rejection, filter exclusion.
// ============================================================================

#include <qiven/runtime/cognition/source_lock.hpp>
#include <qiven/runtime/processx/process_runner.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
using qiven::runtime::cognition::LockError;
using qiven::runtime::cognition::LockRepository;
using qiven::runtime::cognition::SourceLockBuilder;
using qiven::runtime::cognition::SourceLockRequest;
using qiven::runtime::processx::ProcessRunner;
using qiven::runtime::processx::ProcessSpec;

bool git(const ProcessRunner& runner, const std::filesystem::path& repo,
         std::vector<std::string> args)
{
    ProcessSpec spec;
    spec.executable = R"(C:\Program Files\Git\cmd\git.exe)";
    spec.argv       = { "git", "-C", repo.string() };
    spec.argv.insert(spec.argv.end(), args.begin(), args.end());
    spec.working_dir = repo;
    spec.deadline_ms = 30'000;
    auto run         = runner.run(spec);
    return run.is_ok() && run.value().exit_code == 0;
}

void write_file(const std::filesystem::path& file, const std::string& bytes)
{
    std::filesystem::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

bool make_fixture(const ProcessRunner& runner, const std::filesystem::path& repo)
{
    std::error_code ec;
    std::filesystem::remove_all(repo, ec);
    std::filesystem::create_directories(repo / "memory" / "records", ec);
    std::filesystem::create_directories(repo / "docs", ec);
    write_file(repo / "memory" / "records" / "a.md", "record-a-body\n");
    write_file(repo / "memory" / "records" / "b.md", "record-b-body\n");
    write_file(repo / "memory" / "index.yaml", "records: []\n");
    write_file(repo / "docs" / "outside.md", "not selected\n");
    if (!git(runner, repo, { "init", "--quiet" }) ||
        !git(runner, repo, { "config", "user.email", "t@t" }) ||
        !git(runner, repo, { "config", "user.name", "t" }) ||
        !git(runner, repo, { "add", "-A" }) ||
        !git(runner, repo, { "commit", "--quiet", "-m", "fixture" }))
    {
        return false;
    }
    return true;
}

SourceLockRequest make_request(const std::filesystem::path& repo, const ProcessRunner& runner)
{
    LockRepository locked;
    locked.repository   = "qiven-fixture";
    locked.path_filters = { "memory/records" };
    locked.checkout     = repo;
    locked.ref          = "HEAD";
    SourceLockRequest request;
    request.repositories.push_back(locked);
    request.git_executable = R"(C:\Program Files\Git\cmd\git.exe)";
    request.runner         = &runner;
    return request;
}
} // namespace

int main()
{
    const std::string eol(1, char(10));
    const ProcessRunner runner;
    const auto workroot = std::filesystem::path(QIVEN_RUNTIME_TEST_WORKROOT);
    const auto repo     = workroot / "source-lock" / "repo";
    const SourceLockBuilder builder;

    if (!make_fixture(runner, repo))
    {
        std::printf("[FAIL] fixture setup%s", eol.c_str());
        return 1;
    }

    // ---- clean determinism: two builds byte-identical; sorted; filtered ----
    auto first = builder.build(make_request(repo, runner));
    if (!first.is_ok())
    {
        std::printf("[FAIL] clean build failed: %s%s",
                    qiven::runtime::cognition::lock_error_text(first.reason()).data(), eol.c_str());
        return 2;
    }
    auto second = builder.build(make_request(repo, runner));
    if (!second.is_ok() ||
        second.value().digest_hex() != first.value().digest_hex())
    {
        std::printf("[FAIL] determinism: digests differ%s", eol.c_str());
        return 3;
    }
    const auto& entries = first.value().entries;
    if (entries.size() != 2 || entries[0].path != "memory/records/a.md" ||
        entries[1].path != "memory/records/b.md")
    {
        std::printf("[FAIL] filter shape: %zu entries%s", entries.size(), eol.c_str());
        return 4;
    }
    for (const auto& entry : entries)
    {
        if (entry.commit.size() != 40 || entry.tree_oid.size() != 40 ||
            entry.sha256.size() != 64 || entry.size_bytes != 14)
        {
            std::printf("[FAIL] entry shape at %s%s", entry.path.c_str(), eol.c_str());
            return 5;
        }
    }
    // hand-verified canonical prefix (schema + first entry key order)
    const std::string json = first.value().canonical_json();
    if (json.compare(0, 44, "{\"schema\":\"qiven-source-lock-v1\",\"") != 0 ||
        json.find("\"repository\":\"qiven-fixture\",\"commit\":") == std::string::npos ||
        json.find("docs/outside.md") != std::string::npos ||
        json.find("memory/index.yaml") != std::string::npos)
    {
        std::printf("[FAIL] canonical json shape%s", eol.c_str());
        return 6;
    }
    std::printf("[ OK ] clean lock deterministic, sorted, filter-exact%s", eol.c_str());

    // ---- content change without path change moves the digest (A.3) ----
    write_file(repo / "memory" / "records" / "a.md", "record-a-body-CHANGED\n");
    if (!git(runner, repo, { "add", "-A" }) ||
        !git(runner, repo, { "commit", "--quiet", "-m", "change" }))
    {
        std::printf("[FAIL] change commit%s", eol.c_str());
        return 7;
    }
    auto changed = builder.build(make_request(repo, runner));
    if (!changed.is_ok() ||
        changed.value().digest_hex() == first.value().digest_hex())
    {
        std::printf("[FAIL] content change did not move the lock digest%s", eol.c_str());
        return 8;
    }
    std::printf("[ OK ] content change moves the digest%s", eol.c_str());

    // ---- dirty checkout: typed rejection ----
    write_file(repo / "memory" / "records" / "b.md", "dirty\n");
    auto dirty = builder.build(make_request(repo, runner));
    if (dirty.is_ok() || dirty.reason() != LockError::DirtyCheckout)
    {
        std::printf("[FAIL] dirty checkout not typed-rejected%s", eol.c_str());
        return 9;
    }
    // restore cleanliness for later suites
    if (!git(runner, repo, { "checkout", "--", "." }))
    {
        std::printf("[FAIL] restore failed%s", eol.c_str());
        return 10;
    }
    std::printf("[ OK ] dirty checkout typed-rejected%s", eol.c_str());

    // ---- non-HEAD ref: typed rejection (pinned-to-exact-revision law) ----
    // HEAD~1 exists (the first commit); locking at it from a checkout whose
    // HEAD is newer must fail RevisionUnresolved.
    SourceLockRequest stale_request   = make_request(repo, runner);
    stale_request.repositories[0].ref = "HEAD~1";
    auto stale                        = builder.build(stale_request);
    if (stale.is_ok() || stale.reason() != LockError::RevisionUnresolved)
    {
        std::printf("[FAIL] non-HEAD ref not typed-rejected%s", eol.c_str());
        return 11;
    }
    std::printf("[ OK ] non-HEAD ref typed-rejected%s", eol.c_str());

    // ---- filter is component-wise: a sibling file with a longer name is
    // not under a FILE filter, and an unrelated directory is excluded ----
    SourceLockRequest file_request            = make_request(repo, runner);
    file_request.repositories[0].path_filters = { "memory/index.yaml" };
    auto file_lock                            = builder.build(file_request);
    if (!file_lock.is_ok() || file_lock.value().entries.size() != 1 ||
        file_lock.value().entries[0].path != "memory/index.yaml")
    {
        std::printf("[FAIL] file filter shape%s", eol.c_str());
        return 12;
    }
    std::printf("[ OK ] file filter exact%s", eol.c_str());

    std::printf("SOURCE-LOCK PASS%s", eol.c_str());
    return 0;
}
