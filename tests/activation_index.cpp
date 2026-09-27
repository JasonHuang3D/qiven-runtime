// ============================================================================
// activation_index — CA-1 PR-1 index gates (design §4; TCA-ARCH §9)
// Deterministic ActivationGeneration, exact-key reuse, manifest binding,
// ACTIVE pointer, index-loss rebuild.
// ============================================================================

#include <qiven/runtime/cognition/activation_index.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace
{
using qiven::runtime::cognition::ActivationIndexBuilder;
using qiven::runtime::cognition::ActivationPolicy;
using qiven::runtime::cognition::ActivationRule;
using qiven::runtime::cognition::IndexBuildRequest;
using qiven::runtime::cognition::LockEntry;
using qiven::runtime::cognition::SourceLock;

SourceLock sample_lock()
{
    SourceLock lock;
    for (const char* path : { "decisions/ADR-0024.md", "memory/records/x.md" })
    {
        LockEntry entry;
        entry.repository = "qiven-context";
        entry.commit     = "0123456789abcdef0123456789abcdef01234567";
        entry.tree_oid   = "89abcdef0123456789abcdef0123456789abcdef";
        entry.path       = path;
        entry.sha256     = std::string(64, 'a');
        entry.size_bytes = 100;
        lock.entries.push_back(entry);
    }
    return lock;
}

ActivationPolicy sample_policy()
{
    ActivationPolicy policy;
    policy.schema_version = 1;
    ActivationRule rule;
    rule.rule_id              = "LAW-SEMANTIC-OWNERSHIP";
    rule.source               = { "qiven-context", "decisions/ADR-0024.md", "body" };
    rule.selectors.phases     = { "design" };
    rule.selectors.risk       = { "R2" };
    rule.expected_controls    = { "semantic owner law" };
    rule.independent_evidence = { "mechanical" };
    policy.rules.push_back(rule);
    return policy;
}

std::optional<std::string> read_text(const std::filesystem::path& file)
{
    std::ifstream in(file, std::ios::binary);
    if (!in)
    {
        return std::nullopt;
    }
    in.seekg(0, std::ios::end);
    const auto size = in.tellg();
    std::string text;
    text.resize(static_cast<std::size_t>(size));
    in.seekg(0, std::ios::beg);
    if (!text.empty())
    {
        in.read(text.data(), static_cast<std::streamsize>(text.size()));
    }
    if (!in)
    {
        return std::nullopt;
    }
    return text;
}
} // namespace

int main()
{
    const std::string eol(1, char(10));
    const auto workroot = std::filesystem::path(QIVEN_RUNTIME_TEST_WORKROOT);
    const ActivationIndexBuilder builder;

    IndexBuildRequest request;
    request.runtime_root            = workroot / "index";
    request.source_lock             = sample_lock();
    request.policy                  = sample_policy();
    request.canonical_bundle_digest = std::string(64, 'b');
    request.runtime_generation_id   = "gen-1";
    request.publisher_build         = "test-build";

    // ---- deterministic generation + build + reuse ----
    const std::string generation = qiven::runtime::cognition::activation_generation_of(request);
    auto first                   = builder.build(request);
    if (!first.is_ok())
    {
        std::printf("[FAIL] build failed: %s%s",
                    qiven::runtime::cognition::index_error_text(first.reason()).data(),
                    eol.c_str());
        return 1;
    }
    if (first.value().activation_generation != generation ||
        first.value().source_count != 2 || first.value().rule_count != 1)
    {
        std::printf("[FAIL] build result wrong%s", eol.c_str());
        return 2;
    }
    // identical request → identical generation + exact-key reuse (no rebuild)
    auto again = builder.build(request);
    if (!again.is_ok() ||
        again.value().activation_generation != generation ||
        again.value().index_dir != first.value().index_dir)
    {
        std::printf("[FAIL] exact-key reuse failed%s", eol.c_str());
        return 3;
    }
    // changed closure → DIFFERENT generation (a moved source input)
    IndexBuildRequest moved             = request;
    moved.source_lock.entries[0].sha256 = std::string(64, 'c');
    const std::string moved_generation =
        qiven::runtime::cognition::activation_generation_of(moved);
    if (moved_generation == generation)
    {
        std::printf("[FAIL] source movement did not change ActivationGeneration%s", eol.c_str());
        return 4;
    }
    // runtime generation participates but is NOT the activation identity
    IndexBuildRequest other_runtime     = request;
    other_runtime.runtime_generation_id = "gen-2";
    if (qiven::runtime::cognition::activation_generation_of(other_runtime) == generation)
    {
        std::printf("[FAIL] RuntimeGeneration does not key the identity%s", eol.c_str());
        return 5;
    }
    std::printf("[ OK ] deterministic generation, exact-key reuse, movement invalidation%s",
                eol.c_str());

    // ---- sidecar shape: manifest bindings + ACTIVE pointer + lock copy ----
    const auto manifest_text =
        read_text(first.value().index_dir / "index-manifest.json").value_or("");
    if (manifest_text.find("\"canonical_bundle_sha256\":\"" + std::string(64, 'b') + "\"") ==
            std::string::npos ||
        manifest_text.find("\"runtime_generation\":\"gen-1\"") == std::string::npos ||
        manifest_text.find("\"external_source_lock_sha256\":\"" + request.source_lock.digest_hex() +
                           "\"") == std::string::npos ||
        manifest_text.find("\"activation_generation\":\"" + generation + "\"") ==
            std::string::npos)
    {
        std::printf("[FAIL] manifest bindings wrong: %s%s", manifest_text.c_str(), eol.c_str());
        return 6;
    }
    if (builder.active_generation(request.runtime_root) != generation)
    {
        std::printf("[FAIL] ACTIVE pointer wrong%s", eol.c_str());
        return 7;
    }
    if (!std::filesystem::exists(first.value().index_dir / "index.sqlite") ||
        !std::filesystem::exists(first.value().index_dir / "source-lock.json"))
    {
        std::printf("[FAIL] sidecar files missing%s", eol.c_str());
        return 8;
    }
    if (std::filesystem::exists(request.runtime_root / "activation-generations" /
                                (".tmp-" + generation)))
    {
        std::printf("[FAIL] staging dir survived the switch%s", eol.c_str());
        return 9;
    }
    std::printf("[ OK ] sidecar shape, manifest bindings, ACTIVE pointer%s", eol.c_str());

    // ---- index loss is recoverable: delete the generation, rebuild equal ----
    std::error_code ec;
    std::filesystem::remove_all(first.value().index_dir, ec);
    auto rebuilt = builder.build(request);
    if (!rebuilt.is_ok() || rebuilt.value().activation_generation != generation ||
        rebuilt.value().index_dir != first.value().index_dir)
    {
        std::printf("[FAIL] loss rebuild not identity-equal%s", eol.c_str());
        return 10;
    }
    std::printf("[ OK ] index loss recovered from pinned inputs%s", eol.c_str());

    // ---- invalid request typed-rejected ----
    IndexBuildRequest empty = request;
    empty.policy.rules.clear();
    if (builder.build(empty).is_ok())
    {
        std::printf("[FAIL] empty policy accepted%s", eol.c_str());
        return 11;
    }
    std::printf("[ OK ] invalid request typed-rejected%s", eol.c_str());

    std::printf("ACTIVATION-INDEX PASS%s", eol.c_str());
    return 0;
}
