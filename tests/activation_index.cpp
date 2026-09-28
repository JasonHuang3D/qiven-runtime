// ============================================================================
// activation_index — CA-1 PR-1 index gates (design §4; TCA-ARCH §9)
// Deterministic ActivationGeneration, exact-key reuse, manifest binding,
// ACTIVE pointer, index-loss rebuild.
// ============================================================================

#include <qiven/runtime/cognition/activation_index.hpp>

#include <sqlite3.h>

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
    rule.selectors.phases     = { "design", "implementation", "review" };
    rule.selectors.risk       = { "R1", "R2" };
    rule.expected_controls    = { "semantic owner law" };
    rule.independent_evidence = { "mechanical" };
    policy.rules.push_back(rule);
    // P4-on-demand with an OUT-of-closure source: the designed shape (the
    // CA-1 P4 law - deliberation references ride the policy table, never
    // a fabricated closure row). The build must accept it.
    ActivationRule on_demand;
    on_demand.rule_id                = "ONDEMAND-TEST";
    on_demand.priority_class         = qiven::runtime::cognition::PriorityClass::P4OnDemand;
    on_demand.source                 = { "qiven-docs", "accepted", "records" };
    on_demand.selectors.explicit_ids = { "ADR-0050" };
    on_demand.expected_controls      = { "on-demand reference" };
    policy.rules.push_back(on_demand);
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
        first.value().source_count != 2 || first.value().rule_count != 2)
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

    // ---- sidecar content: every selector VALUE persists and rule_sources
    // binds the REAL sources row (the sidecar must not misrepresent the
    // rule table; a placeholder join is a misrepresentation) ----
    {
        sqlite3* db = nullptr;
        if (sqlite3_open_v2((first.value().index_dir / "index.sqlite").string().c_str(), &db,
                            SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
        {
            std::printf("[FAIL] cannot open sidecar index for read-back%s", eol.c_str());
            sqlite3_close(db);
            return 20;
        }
        auto count_values = [&](const char* kind) -> int {
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(db,
                                   "SELECT COUNT(*) FROM selectors WHERE "
                                   "rule_id='LAW-SEMANTIC-OWNERSHIP' AND selector_kind=?",
                                   -1, &stmt, nullptr) != SQLITE_OK)
            {
                return -1;
            }
            sqlite3_bind_text(stmt, 1, kind, -1, SQLITE_TRANSIENT);
            const int rows = sqlite3_step(stmt) == SQLITE_ROW ? sqlite3_column_int(stmt, 0) : -1;
            sqlite3_finalize(stmt);
            return rows;
        };
        if (count_values("phase") != 3 || count_values("risk") != 2)
        {
            std::printf("[FAIL] selector values truncated in sidecar (phase=%d risk=%d)%s",
                        count_values("phase"), count_values("risk"), eol.c_str());
            sqlite3_close(db);
            return 21;
        }
        sqlite3_stmt* join = nullptr;
        if (sqlite3_prepare_v2(db,
                               "SELECT rs.source_id, s.path FROM rule_sources rs "
                               "JOIN sources s ON s.source_id = rs.source_id "
                               "WHERE rs.rule_id='LAW-SEMANTIC-OWNERSHIP'",
                               -1, &join, nullptr) != SQLITE_OK ||
            sqlite3_step(join) != SQLITE_ROW)
        {
            std::printf("[FAIL] rule_sources does not join a real sources row%s", eol.c_str());
            sqlite3_finalize(join);
            sqlite3_close(db);
            return 22;
        }
        const char* joined_path = reinterpret_cast<const char*>(sqlite3_column_text(join, 1));
        if (joined_path == nullptr ||
            std::string(joined_path) != "decisions/ADR-0024.md" ||
            sqlite3_column_int(join, 0) != 1)
        {
            std::printf("[FAIL] rule_sources joined the wrong source row%s", eol.c_str());
            sqlite3_finalize(join);
            sqlite3_close(db);
            return 23;
        }
        sqlite3_finalize(join);
        sqlite3_close(db);
    }
    std::printf("[ OK ] sidecar carries every selector value; rule_sources joins the real "
                "source row%s",
                eol.c_str());

    // ---- a rule whose source is OUTSIDE the locked closure: typed
    // rejection (no dangling rule_sources reference) ----
    {
        IndexBuildRequest dangling           = request;
        dangling.policy.rules[0].source.path = "decisions/ADR-9999.md";
        dangling.canonical_bundle_digest     = std::string(64, 'd');
        auto failed                          = builder.build(dangling);
        if (failed.is_ok() || failed.reason() !=
                                  qiven::runtime::cognition::IndexError::InvalidRequest)
        {
            std::printf("[FAIL] out-of-closure rule source not typed-rejected%s", eol.c_str());
            return 24;
        }
        std::error_code sweep;
        std::filesystem::remove_all(request.runtime_root / "activation-generations" /
                                        (".tmp-" +
                                         qiven::runtime::cognition::activation_generation_of(
                                             dangling)),
                                    sweep);
    }
    std::printf("[ OK ] out-of-closure rule source typed-rejected%s", eol.c_str());

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

    // ---- WR-7 cutover provenance (ADR-0058 d6): the workspace
    // generation is PROVENANCE ONLY — a different workspace generation
    // with identical content must NOT change ActivationGeneration, and
    // the sidecar records it + refreshes on exact-key reuse ----
    {
        IndexBuildRequest provenance      = request;
        provenance.workspace_generation   = "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
        const std::string with_provenance = qiven::runtime::cognition::activation_generation_of(
            provenance);
        if (with_provenance != generation)
        {
            std::printf("[FAIL] workspace provenance changed ActivationGeneration%s", eol.c_str());
            return 30;
        }
        auto built = builder.build(provenance);
        if (!built.is_ok() || built.value().activation_generation != generation ||
            built.value().index_dir != first.value().index_dir)
        {
            std::printf("[FAIL] provenance build failed%s", eol.c_str());
            return 31;
        }
        const auto sidecar =
            read_text(built.value().index_dir / "workspace-provenance.json").value_or("");
        if (sidecar.find("\"workspace_generation\":\"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"") ==
                std::string::npos ||
            sidecar.find("\"binding\":\"provenance-only-never-content\"") == std::string::npos)
        {
            std::printf("[FAIL] provenance sidecar wrong: %s%s", sidecar.c_str(), eol.c_str());
            return 32;
        }
        // content finality: the manifest does NOT carry the provenance
        if (read_text(built.value().index_dir / "index-manifest.json")
                ->find("workspace_generation") != std::string::npos)
        {
            std::printf("[FAIL] provenance leaked into the content manifest%s", eol.c_str());
            return 33;
        }
        // reuse drift refreshes ONLY the provenance sidecar
        IndexBuildRequest moved_provenance    = provenance;
        moved_provenance.workspace_generation = "sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
        auto refreshed                        = builder.build(moved_provenance);
        if (!refreshed.is_ok() ||
            refreshed.value().activation_generation != generation ||
            refreshed.value().index_dir != first.value().index_dir)
        {
            std::printf("[FAIL] provenance-drift reuse failed%s", eol.c_str());
            return 34;
        }
        const auto refreshed_sidecar =
            read_text(refreshed.value().index_dir / "workspace-provenance.json").value_or("");
        if (refreshed_sidecar.find("sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb") ==
            std::string::npos)
        {
            std::printf("[FAIL] provenance sidecar not refreshed on reuse%s", eol.c_str());
            return 35;
        }
        std::printf("[ OK ] workspace provenance recorded, digest-neutral, refresh-on-reuse%s",
                    eol.c_str());
    }

    // ---- corruption of an existing generation is NEVER silently reused:
    // a tampered manifest mismatches the re-derived bytes → typed fault
    // (Profile A.8; rebuild happens only from a clean state) ----
    {
        const auto manifest_file = first.value().index_dir / "index-manifest.json";
        std::ofstream tamper(manifest_file, std::ios::binary | std::ios::trunc);
        tamper << "{\"schema\":\"qiven-activation-index-manifest-v1\",\"tampered\":true}";
        tamper.close();
        auto reused = builder.build(request);
        if (reused.is_ok() ||
            reused.reason() != qiven::runtime::cognition::IndexError::ManifestMismatch)
        {
            std::printf("[FAIL] corrupted manifest silently reused%s", eol.c_str());
            return 12;
        }
        // recovery: remove the corrupt generation; rebuild is identity-equal
        std::error_code remove_ec;
        std::filesystem::remove_all(first.value().index_dir, remove_ec);
        auto recovered = builder.build(request);
        if (!recovered.is_ok() ||
            recovered.value().activation_generation != generation)
        {
            std::printf("[FAIL] corruption recovery not identity-equal%s", eol.c_str());
            return 13;
        }
        std::printf("[ OK ] manifest corruption typed-rejected; rebuild identity-equal%s",
                    eol.c_str());
    }

    std::printf("ACTIVATION-INDEX PASS%s", eol.c_str());
    return 0;
}
