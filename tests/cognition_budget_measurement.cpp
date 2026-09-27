// ============================================================================
// cognition_budget_measurement — CA-1 latency evidence (design §5; core
// instance latency_objectives: cold rebuild < 5000 ms, warm activation
// p95 < 250 ms on the reference corpus)
//
// Builds a synthetic corpus at the CA-0 sealed scale (~120 tracked files,
// ~300 KB) in a REAL git fixture, then measures: (a) COLD = source-lock
// resolve + index build; (b) WARM = 50 activations through the shared
// service (exact-key index reuse). Assertions run at 2x headroom for CI
// stability; the measured numbers are printed as evidence rows (the
// exact objective numbers are read from them, not from the assertion).
// ============================================================================

#include <qiven/runtime/cognition/activation_index.hpp>
#include <qiven/runtime/cognition/activation_service.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
using qiven::runtime::cognition::ActivationPolicy;
using qiven::runtime::cognition::ActivationRule;
using qiven::runtime::cognition::CognitionCore;
using qiven::runtime::cognition::PriorityClass;
using qiven::runtime::cognition::TaskEnvelope;
using qiven::runtime::cognition::TaskPhase;
using qiven::runtime::cognition::TaskRisk;

bool git(const std::filesystem::path& repo, std::vector<std::string> args)
{
    qiven::runtime::processx::ProcessSpec spec;
    spec.executable = R"(C:\Program Files\Git\cmd\git.exe)";
    spec.argv       = { "git", "-C", repo.string() };
    spec.argv.insert(spec.argv.end(), args.begin(), args.end());
    spec.working_dir = repo;
    spec.deadline_ms = 30'000;
    qiven::runtime::processx::ProcessRunner runner;
    auto run = runner.run(spec);
    return run.is_ok() && run.value().exit_code == 0;
}

void write_file(const std::filesystem::path& file, const std::string& bytes)
{
    std::filesystem::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

qiven::u64 now_ms()
{
    return static_cast<qiven::u64>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}
} // namespace

int main()
{
    const std::string eol(1, char(10));
    const auto workroot = std::filesystem::path(QIVEN_RUNTIME_TEST_WORKROOT);
    const auto repo     = workroot / "budget" / "corpus";
    std::error_code ec;
    std::filesystem::remove_all(repo, ec);
    std::filesystem::create_directories(repo, ec);

    // ~120 files ≈ 2.5 KB each ≈ 300 KB (CA-0 sealed scale)
    const std::string body(2560, 'x');
    for (int group = 0; group < 6; ++group)
    {
        for (int index = 0; index < 20; ++index)
        {
            write_file(repo / ("records/group-" + std::to_string(group)) /
                           ("record-" + std::to_string(index) + ".md"),
                       "# record\n\n" + body + "\n");
        }
    }
    if (!git(repo, { "init", "--quiet" }) || !git(repo, { "config", "user.email", "t@t" }) ||
        !git(repo, { "config", "user.name", "t" }) || !git(repo, { "add", "-A" }) ||
        !git(repo, { "commit", "--quiet", "-m", "corpus" }))
    {
        std::printf("[FAIL] corpus fixture setup%s", eol.c_str());
        return 1;
    }
    std::printf("[ OK ] synthetic corpus at CA-0 scale (120 files, ~300 KB)%s", eol.c_str());

    // policy: one protected rule + evaluator workload at the bootstrap size
    ActivationPolicy policy;
    policy.schema_version = 1;
    for (int index = 0; index < 16; ++index)
    {
        ActivationRule rule;
        rule.rule_id              = "RULE-" + std::to_string(index);
        rule.priority_class       = index == 0 ? PriorityClass::P0Core : PriorityClass::P3Supporting;
        rule.source               = { "qiven-context", "records/group-0/record-" + std::to_string(index % 20) + ".md", "body" };
        rule.selectors.phases     = { "all" };
        rule.expected_controls    = { "control statement " + std::to_string(index) };
        rule.independent_evidence = { "mechanical" };
        policy.rules.push_back(std::move(rule));
    }

    qiven::runtime::cognition::SourceLockRequest lock_request;
    qiven::runtime::cognition::LockRepository locked;
    locked.repository   = "qiven-context";
    locked.path_filters = { "records" };
    locked.checkout     = repo;
    locked.ref          = "HEAD";
    lock_request.repositories.push_back(locked);
    lock_request.git_executable = R"(C:\Program Files\Git\cmd\git.exe)";
    qiven::runtime::processx::ProcessRunner runner;
    lock_request.runner = &runner;

    const auto runtime_root = workroot / "budget" / "runtime";
    CognitionCore core;
    core.schema_version               = 1;
    core.compact_core_max_bytes       = 12288;
    core.task_payload_max_bytes       = 65536;
    core.inlined_body_max_bytes       = 8192;
    core.supporting_share_max_percent = 25;
    core.cold_rebuild_max_ms          = 5000;
    core.warm_activation_p95_max_ms   = 250;
    core.consumer_profiles            = { "zcode-jason" };

    // ---- COLD: lock resolve + index build ----
    const qiven::u64 cold_begin = now_ms();
    const qiven::runtime::cognition::SourceLockBuilder lock_builder;
    auto lock = lock_builder.build(lock_request);
    if (!lock.is_ok())
    {
        std::printf("[FAIL] lock build failed%s", eol.c_str());
        return 2;
    }
    qiven::runtime::cognition::IndexBuildRequest build;
    build.runtime_root            = runtime_root;
    build.source_lock             = std::move(lock.value());
    build.policy                  = policy;
    build.canonical_bundle_digest = std::string(64, 'b');
    build.runtime_generation_id   = "gen-1";
    build.publisher_build         = "budget-test";
    const qiven::runtime::cognition::ActivationIndexBuilder index_builder;
    auto index = index_builder.build(build);
    if (!index.is_ok())
    {
        std::printf("[FAIL] index build failed%s", eol.c_str());
        return 3;
    }
    const qiven::u64 cold_ms = now_ms() - cold_begin;
    std::printf("  measured: cold rebuild %llu ms (objective <%llu)%s",
                static_cast<unsigned long long>(cold_ms),
                static_cast<unsigned long long>(core.cold_rebuild_max_ms), eol.c_str());
    if (cold_ms > core.cold_rebuild_max_ms * 2)
    {
        std::printf("[FAIL] cold rebuild beyond 2x objective headroom%s", eol.c_str());
        return 4;
    }
    std::printf("[ OK ] cold rebuild within budget (2x headroom asserted)%s", eol.c_str());

    // ---- WARM: 50 activations (exact-key index reuse; in-memory policy) ----
    TaskEnvelope envelope;
    envelope.objective     = "routine task touching the corpus records";
    envelope.repository    = "qiven-context";
    envelope.revision      = "head";
    envelope.changed_paths = { "records/group-1/record-0.md" };
    envelope.phase         = TaskPhase::Implementation;
    envelope.risk          = TaskRisk::R2;

    std::vector<qiven::u64> samples;
    const qiven::runtime::cognition::ActivationService service;
    for (int iteration = 0; iteration < 50; ++iteration)
    {
        qiven::runtime::cognition::ActivationRequest request;
        request.envelope                    = envelope;
        request.core                        = core;
        request.policy                      = policy;
        request.activation_generation       = index.value().activation_generation;
        request.runtime_generation_id       = "gen-1";
        request.external_source_lock_sha256 = std::string(64, 'a');
        request.activation_policy_sha256    = std::string(64, 'e');
        request.consumer_profile            = "zcode-jason";
        request.runtime_root                = workroot / "budget" / "warm";
        request.now_ms                      = 1'000'000 + static_cast<qiven::u64>(iteration);
        const qiven::u64 begin              = now_ms();
        auto outcome                        = service.activate(request);
        if (!outcome.is_ok())
        {
            std::printf("[FAIL] warm activation %d failed%s", iteration, eol.c_str());
            return 5;
        }
        samples.push_back(now_ms() - begin);
    }
    std::sort(samples.begin(), samples.end());
    const qiven::u64 p95 = samples[samples.size() * 95 / 100];
    std::printf("  measured: warm activation p95 %llu ms over 50 runs (objective <%llu)%s",
                static_cast<unsigned long long>(p95),
                static_cast<unsigned long long>(core.warm_activation_p95_max_ms), eol.c_str());
    if (p95 > core.warm_activation_p95_max_ms * 2)
    {
        std::printf("[FAIL] warm p95 beyond 2x objective headroom%s", eol.c_str());
        return 6;
    }
    std::printf("[ OK ] warm activation p95 within budget (2x headroom asserted)%s",
                eol.c_str());

    std::printf("BUDGET-MEASUREMENT PASS%s", eol.c_str());
    return 0;
}
