// ============================================================================
// cognition_shadow_compare — WR-7 engineering-leg unit gates (ADR-0052
// doc 02 stage WR-7; design docs/design/ca1-activation-core.md §2 +
// the WR-7 comparator design in the v42 workflow records).
//
// Proves: tier-1 per-repo (commit, root tree) equivalence decides;
// StaleNode / NodeMissing / ContentIdentityConflict typed divergences;
// the ShadowOnlyNode annotation blocks cutover-grade evidence while
// leaving content equality true; the workspace-lock parser fails
// closed on duplicate keys; the receipt shape is deterministic and
// carries the workspace generation as provenance only.
// ============================================================================

#include <qiven/runtime/cognition/shadow_compare.hpp>

#include <cstdio>
#include <string>
#include <vector>

namespace
{
using qiven::runtime::cognition::LockEntry;
using qiven::runtime::cognition::ShadowDivergence;
using qiven::runtime::cognition::SourceLock;

SourceLock lock_of(const std::vector<LockEntry>& entries)
{
    SourceLock lock;
    lock.entries = entries;
    return lock;
}

LockEntry entry(const std::string& repository, const std::string& commit, const std::string& tree,
                const std::string& path)
{
    LockEntry item;
    item.repository = repository;
    item.commit     = commit;
    item.tree_oid   = tree;
    item.path       = path;
    item.sha256     = std::string(64, 'a');
    item.size_bytes = 10;
    return item;
}
} // namespace

int main()
{
    const std::string eol(1, char(10));
    const std::string commit_a = std::string(40, 'a');
    const std::string commit_b = std::string(40, 'b');
    const std::string tree_a   = std::string(40, 'c');
    const std::string tree_b   = std::string(40, 'd');

    // ---- equal closure: every repo's (commit, tree) matches its node ----
    {
        SourceLock lock                  = lock_of({ entry("qiven-runtime", commit_a, tree_a, "docs/architecture/x.md"),
                                                     entry("qiven-foundation", commit_b, tree_b, "include/qiven/y.hpp") });
        const std::string workspace_json = std::string(R"({"generation":"sha256:gen1","nodes":{)"
                                                       R"("qiven-runtime":{"commit":")" +
                                                       commit_a +
                                                       R"(","tree":")" + tree_a +
                                                       R"(","declaration":{"shadow_only":false}},)"
                                                       R"("qiven-foundation":{"commit":")" +
                                                       commit_b +
                                                       R"(","tree":")" + tree_b +
                                                       R"(","declaration":{"shadow_only":false}}}})");
        auto workspace                   = qiven::runtime::cognition::parse_workspace_lock(workspace_json);
        if (workspace.generation != "sha256:gen1" || workspace.nodes.size() != 2)
        {
            std::printf("[FAIL] workspace lock parse wrong%s", eol.c_str());
            return 1;
        }
        auto result = qiven::runtime::cognition::shadow_compare(lock, workspace);
        if (!result.all_equal() || !result.cutover_grade() || result.repositories.size() != 2)
        {
            std::printf("[FAIL] equal closure not recognized%s", eol.c_str());
            return 2;
        }
        if (result.source_lock_sha256 != lock.digest_hex() ||
            result.workspace_generation != "sha256:gen1")
        {
            std::printf("[FAIL] provenance fields wrong%s", eol.c_str());
            return 3;
        }
        const std::string receipt = qiven::runtime::cognition::shadow_compare_receipt_json(result);
        if (receipt.find("\"schema\":\"qiven-tca-shadow-compare-v1\"") == std::string::npos ||
            receipt.find("\"all_equal\":true") == std::string::npos ||
            receipt.find("\"cutover_grade\":true") == std::string::npos ||
            receipt.find("\"divergence\":\"none\"") == std::string::npos)
        {
            std::printf("[FAIL] receipt shape wrong: %s%s", receipt.c_str(), eol.c_str());
            return 4;
        }
        std::printf("[ OK ] equal closure, cutover grade, receipt shape%s", eol.c_str());
    }

    // ---- stale node: content divergence typed ----
    {
        SourceLock lock                  = lock_of({ entry("qiven-context", commit_a, tree_a, "state/x.md") });
        const std::string workspace_json = std::string(R"({"generation":"sha256:gen2","nodes":{)"
                                                       R"("qiven-context":{"commit":")" +
                                                       commit_b +
                                                       R"(","tree":")" + tree_b +
                                                       R"(","declaration":{"shadow_only":true}}}})");
        auto workspace                   = qiven::runtime::cognition::parse_workspace_lock(workspace_json);
        auto result                      = qiven::runtime::cognition::shadow_compare(lock, workspace);
        if (result.all_equal() || result.repositories.size() != 1 ||
            result.repositories[0].divergence != ShadowDivergence::StaleNode)
        {
            std::printf("[FAIL] stale node not typed%s", eol.c_str());
            return 5;
        }
        if (result.cutover_grade())
        {
            std::printf("[FAIL] diverged result must never be cutover grade%s", eol.c_str());
            return 6;
        }
        std::printf("[ OK ] stale node typed StaleNode%s", eol.c_str());
    }

    // ---- shadow-only annotation: equal content, blocked cutover evidence ----
    {
        SourceLock lock = lock_of({ entry("qiven-devkit", commit_a, tree_a, "docs/schemas/x.json") });
        const std::string workspace_json =
            std::string(R"({"generation":"sha256:gen3","nodes":{"qiven-devkit":{"commit":")") +
            commit_a + R"(","tree":")" + tree_a + R"(","declaration":{"shadow_only":true}}}})";
        auto workspace = qiven::runtime::cognition::parse_workspace_lock(workspace_json);
        auto result    = qiven::runtime::cognition::shadow_compare(lock, workspace);
        if (!result.all_equal() || result.cutover_grade())
        {
            std::printf("[FAIL] shadow-only annotation semantics wrong%s", eol.c_str());
            return 7;
        }
        const std::string receipt = qiven::runtime::cognition::shadow_compare_receipt_json(result);
        if (receipt.find("\"shadow_only_node\":true") == std::string::npos)
        {
            std::printf("[FAIL] receipt lacks the shadow annotation%s", eol.c_str());
            return 8;
        }
        std::printf("[ OK ] shadow-only equal-but-not-cutover-grade%s", eol.c_str());
    }

    // ---- node missing ----
    {
        SourceLock lock =
            lock_of({ entry("qiven-orphan", commit_a, tree_a, "docs/x.md") });
        auto workspace = qiven::runtime::cognition::parse_workspace_lock(
            R"({"generation":"sha256:g","nodes":{"qiven-runtime":{"commit":"x","tree":"y"}}})");
        auto result = qiven::runtime::cognition::shadow_compare(lock, workspace);
        if (result.all_equal() ||
            result.repositories[0].divergence != ShadowDivergence::NodeMissing)
        {
            std::printf("[FAIL] missing node not typed%s", eol.c_str());
            return 9;
        }
        std::printf("[ OK ] missing node typed NodeMissing%s", eol.c_str());
    }

    // ---- content identity conflict: multiple (commit, tree) per repository ----
    {
        SourceLock lock = lock_of({ entry("qiven-runtime", commit_a, tree_a, "docs/a.md"),
                                    entry("qiven-runtime", commit_b, tree_b, "docs/b.md") });
        auto workspace  = qiven::runtime::cognition::parse_workspace_lock(
            std::string(R"({"generation":"g","nodes":{"qiven-runtime":{"commit":")") + commit_a +
            R"(","tree":")" + tree_a + R"(","declaration":{"shadow_only":false}}}})");
        auto result       = qiven::runtime::cognition::shadow_compare(lock, workspace);
        bool saw_conflict = false;
        for (const auto& repo : result.repositories)
        {
            if (repo.divergence == ShadowDivergence::ContentConflict)
            {
                saw_conflict = true;
            }
        }
        if (!saw_conflict)
        {
            std::printf("[FAIL] multi-pair lock not loud%s", eol.c_str());
            return 10;
        }
        std::printf("[ OK ] multi-pair lock typed ContentIdentityConflict%s", eol.c_str());
    }

    // ---- workspace lock parser fails closed on duplicate keys ----
    {
        auto workspace = qiven::runtime::cognition::parse_workspace_lock(
            R"({"generation":"a","generation":"b","nodes":{}})");
        if (!workspace.generation.empty() || !workspace.nodes.empty())
        {
            std::printf("[FAIL] duplicate-key workspace lock accepted%s", eol.c_str());
            return 11;
        }
        std::printf("[ OK ] duplicate-key workspace lock rejected%s", eol.c_str());
    }

    // ---- tier 2: filter-set mismatch (tier-1 equal, filters differ) ----
    {
        using qiven::runtime::cognition::ShadowFilterSets;
        SourceLock lock = lock_of({ entry("qiven-devkit", commit_a, tree_a, "docs/schemas/x.json") });
        const std::string workspace_json =
            std::string(R"({"generation":"g","nodes":{"qiven-devkit":{"commit":")") + commit_a +
            R"(","tree":")" + tree_a + R"(","declaration":{"shadow_only":false}}}})";
        auto workspace = qiven::runtime::cognition::parse_workspace_lock(workspace_json);
        ShadowFilterSets used;
        used.carrier_table_present   = true;
        used.filters["qiven-devkit"] = { "docs/engineering", "docs/schemas" };
        ShadowFilterSets at_node;
        at_node.carrier_table_present   = true;
        at_node.filters["qiven-devkit"] = { "docs/schemas" }; // narrowed at the node revision
        auto result                     = qiven::runtime::cognition::shadow_compare(lock, workspace, used, at_node);
        if (result.all_equal() ||
            result.repositories[0].divergence != ShadowDivergence::FilterSetMismatch)
        {
            std::printf("[FAIL] filter mismatch not typed%s", eol.c_str());
            return 12;
        }
        std::printf("[ OK ] filter-set mismatch typed FilterSetMismatch%s", eol.c_str());
    }

    // ---- tier 2: carrier table absent at the node revision ----
    {
        using qiven::runtime::cognition::ShadowFilterSets;
        SourceLock lock = lock_of({ entry("qiven-context", commit_a, tree_a, "state/x.md") });
        const std::string workspace_json =
            std::string(R"({"generation":"g","nodes":{"qiven-context":{"commit":")") + commit_a +
            R"(","tree":")" + tree_a + R"(","declaration":{"shadow_only":true}}}})";
        auto workspace = qiven::runtime::cognition::parse_workspace_lock(workspace_json);
        ShadowFilterSets used;
        used.carrier_table_present    = true;
        used.filters["qiven-context"] = { "state" };
        ShadowFilterSets at_node; // carrier_table_present = false (absent at the node)
        auto result = qiven::runtime::cognition::shadow_compare(lock, workspace, used, at_node);
        if (result.all_equal() ||
            result.repositories[0].divergence != ShadowDivergence::FilterSetMismatch)
        {
            std::printf("[FAIL] absent carrier table not typed%s", eol.c_str());
            return 13;
        }
        std::printf("[ OK ] absent carrier table typed FilterSetMismatch%s", eol.c_str());
    }

    std::printf("SHADOW-COMPARE PASS%s", eol.c_str());
    return 0;
}
