#pragma once

// WR-7 engineering leg (ADR-0052 doc 02 stage WR-7): the shadow
// comparison of the TCA source lock's closure against the
// WorkspaceGeneration. PURE and NON-MUTATING: nothing here builds an
// index, switches an ACTIVE pointer, or emits any digest that enters
// the TCA source lock or ActivationGeneration (the tool execution
// projection is provenance only). Divergence fails the WR-7 migration
// gate; it never falsifies an otherwise valid CA-1 source-lock result
// (spec section 5 compatibility-window law).

#include <qiven/runtime/cognition/source_lock.hpp>

#include <map>
#include <string>
#include <vector>

namespace qiven::runtime::cognition
{
// The workspace side of the comparison: the nodes read from
// workspace.lock.json (minimal JSON subset; the lock's digest-bound
// declaration fields are provenance, never compared content).
struct WorkspaceNodeState
{
    std::string commit;
    std::string tree;
    bool shadow_only = false;
    bool present     = false;
};

struct WorkspaceLockState
{
    std::string generation;
    std::map<std::string, WorkspaceNodeState> nodes;
};

// Parse the workspace lock JSON (strict minimal parser; duplicate keys
// rejected). Typed failure message on malformed input.
[[nodiscard]] WorkspaceLockState parse_workspace_lock(std::string_view bytes);

enum class ShadowDivergence : u8
{
    None              = 0, // commit+tree equal (a ShadowOnlyNode annotation may still apply)
    StaleNode         = 1, // node commit != lock commit
    NodeMissing       = 2, // corpus repository with no workspace node
    FilterSetMismatch = 3, // corpus table absent/different at the node revision (tier-2 observation)
    ContentConflict   = 4, // tier-1 equal but tier-2 content differs (loud; should be impossible)
};

// Tier-2 input: the corpus filter sets on both sides of the comparison.
// The per-file digests of a closure are DETERMINED by (commit, root
// tree) + the filter set (git content addressing): equal (commit,
// tree) AND equal filters implies equal per-file digests by
// construction. Tier 2 therefore verifies the FILTER dimension the
// tree identity cannot express (spec WR-7: "every path filter,
// commit/tree, and per-file digest").
struct ShadowFilterSets
{
    std::map<std::string, std::vector<std::string>> filters; // repository -> path filters
    bool carrier_table_present = false;                      // the corpus table existed at its revision
};

struct ShadowRepoResult
{
    std::string repository;
    std::string lock_commit;
    std::string lock_tree;
    std::string node_commit;
    std::string node_tree;
    ShadowDivergence divergence = ShadowDivergence::None;
    bool shadow_only_node       = false; // census/shadow declaration: equality proven, cutover-grade evidence blocked
    std::string detail;
};

struct ShadowCompareResult
{
    std::string workspace_generation;   // provenance
    std::string source_lock_sha256;     // the compared TCA lock digest
    std::string workspace_lock_binding; // how the TCA lock was obtained ("active:<gen>" | "fresh-build")
    std::vector<ShadowRepoResult> repositories;

    [[nodiscard]] bool all_equal() const;
    [[nodiscard]] bool cutover_grade() const; // all_equal && no shadow_only annotations
};

// Tier-1 comparison: per corpus repository in the lock, the distinct
// (commit, root tree) pair must equal the workspace node's. Repos in
// the lock with no workspace node = NodeMissing. Filter sets are NOT
// compared by this overload (call the tier-2 overload for the spec's
// complete comparison).
[[nodiscard]] ShadowCompareResult shadow_compare(const SourceLock& lock, const WorkspaceLockState& workspace);

// Tier-2 comparison (the spec's complete form): tier 1 plus the filter
// dimension — the corpus table's path filters used to build the lock
// vs the same table re-read at the workspace node's carrier revision.
// FilterSetMismatch fires per repository whose filters differ (or are
// uncovered/absent at the node revision).
[[nodiscard]] ShadowCompareResult shadow_compare(const SourceLock& lock, const WorkspaceLockState& workspace,
                                                 const ShadowFilterSets& filters_used,
                                                 const ShadowFilterSets& filters_at_node);

// Receipt serialization (qiven-tca-shadow-compare-v1; deterministic
// key order; envelope-only fields, never a hashed payload input).
[[nodiscard]] std::string shadow_compare_receipt_json(const ShadowCompareResult& result);

[[nodiscard]] std::string_view shadow_divergence_text(ShadowDivergence divergence) noexcept;
} // namespace qiven::runtime::cognition
