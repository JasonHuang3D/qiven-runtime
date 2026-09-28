#pragma once

// ============================================================================
// cognition/activation_index.hpp — CA-1 activation-index sidecar (TCA-ARCH
// §9; design docs/design/ca1-activation-core.md §4)
//
// Builds the immutable, rebuildable activation index for one exact
// closure: pinned canonical bundle + RuntimeGeneration + external source
// lock + activation policy. Own ActivationGeneration identity (the
// §9.4 cache key) — NEVER the execution RuntimeGeneration. Sidecar root
// <runtime_root>/activation-generations/<id>/ with source-lock.json,
// index.sqlite, index-manifest.json; built in .tmp-<id> staging and
// switched by atomic rename; crash before switch = deletable candidate.
// Full rebuild per generation (v1; incremental prohibited until
// measurement demands — TCA §9.4).
// ============================================================================

#include <qiven/result.hpp>
#include <qiven/runtime/cognition/activation_policy.hpp>
#include <qiven/runtime/cognition/source_lock.hpp>

#include <filesystem>
#include <string>

namespace qiven::runtime::cognition
{
enum class IndexError : u8
{
    InvalidRequest   = 1,
    BuildFailed      = 2, // sqlite/index construction fault
    StagingFault     = 3, // filesystem staging/rename fault
    ManifestMismatch = 4, // existing index disagrees with the closure
};

[[nodiscard]] std::string_view index_error_text(IndexError error) noexcept;

struct IndexBuildRequest
{
    std::filesystem::path runtime_root;  // <...>/.qiven/runtime
    SourceLock source_lock;              // the resolved external closure
    ActivationPolicy policy;             // the typed rule table
    std::string canonical_bundle_digest; // hex sha256 of the pinned bundle manifest
    std::string runtime_generation_id;   // the EXECUTION generation (never conflated)
    std::string publisher_build;         // runtime build identity
    u32 index_schema_version = 1;
};

struct IndexBuildResult
{
    std::string activation_generation; // the §9.4 cache-key hex identity
    std::filesystem::path index_dir;   // .../activation-generations/<id>
    u64 source_count = 0;
    u64 rule_count   = 0;
};

// §9.4 identity: sha256 over (canonical bundle digest ‖ runtime
// generation ‖ sorted source-lock digest ‖ policy digest ‖ schema
// version ‖ publisher build). Exact-equality reuse only.
[[nodiscard]] std::string activation_generation_of(const IndexBuildRequest& request);

// Deterministic canonical digest over the parsed rule table (rule-sorted
// JSON; the same serialization the index manifest and receipts bind).
[[nodiscard]] std::string activation_policy_digest(const ActivationPolicy& policy);

class ActivationIndexBuilder
{
public:
    // Build into staging, validate, then atomically switch the ACTIVE
    // pointer. If the generation dir already exists with a matching
    // manifest, returns it unchanged (exact-key reuse).
    [[nodiscard]] qiven::Result<IndexBuildResult, IndexError> build(
        const IndexBuildRequest& request) const;

    // Pointer recovery/classification without building: names the ACTIVE
    // generation dir, or empty when none.
    [[nodiscard]] std::string active_generation(const std::filesystem::path& runtime_root) const;
};
} // namespace qiven::runtime::cognition
