#pragma once

// ============================================================================
// cognition/source_lock.hpp — CA-1 external source lock (TCA-ARCH §9.2;
// design docs/design/ca1-activation-core.md §2)
//
// Resolves the activation corpus table (cognition-core.yaml) against
// validated LOCAL checkouts: for every repository + path filter, reads
// ONE EXACT Git commit through plumbing (rev-parse / ls-tree / cat-file
// style, exactly like CanonicalCognitionPublisher) and records each
// selected file's content sha256. NEVER reads the working tree; a dirty
// checkout is a typed failure (validated-local law, CA-0 declaration);
// no network.
//
// The lock is the WR-7-critical deliverable: per-entry (repository,
// commit, tree oid, path, content sha256), entries sorted by
// (repository, path); the canonical serialization is deterministic
// JSON (sorted keys, no whitespace) and its sha256 IS
// external_source_lock_sha256 — bound into the index manifest, the
// task-bundle manifest, and every receipt. Movement inside the selected
// closure changes the digest; movement outside does not.
// ============================================================================

#include <qiven/result.hpp>
#include <qiven/runtime/cognition/policy.hpp>
#include <qiven/runtime/processx/process_runner.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace qiven::runtime::cognition
{
inline constexpr usize max_lock_entries     = 4096;
inline constexpr u64 max_locked_file_bytes  = 2 * 1024 * 1024;
inline constexpr u64 max_locked_total_bytes = 64 * 1024 * 1024;

enum class LockError : u8
{
    GitUnavailable     = 1, // plumbing failed to start/complete
    RevisionUnresolved = 2, // ref does not resolve to a commit
    DirtyCheckout      = 3, // working tree differs from the pinned commit
    BoundExceeded      = 4, // entries/bytes ceiling
    PathOutsideFilter  = 5, // resolved path escapes the declared filter
    MalformedEntry     = 6, // unusable ls-tree output
};

[[nodiscard]] std::string_view lock_error_text(LockError error) noexcept;

struct LockRepository
{
    std::string repository;                // canonical repository name
    std::vector<std::string> path_filters; // root-relative file or directory filters
    std::filesystem::path checkout;        // LOCAL checkout root (plumbing -C)
    std::string ref;                       // exact commit oid or resolvable refspec
};

struct LockEntry
{
    std::string repository;
    std::string commit;   // full oid the tree was read at
    std::string tree_oid; // enclosing tree oid (ls-tree row)
    std::string path;     // repository-root-relative, '/' separators
    std::string sha256;   // lowercase hex over the blob bytes
    u64 size_bytes = 0;
};

struct SourceLock
{
    std::vector<LockEntry> entries; // sorted by (repository, path)

    // Deterministic canonical JSON: object keys sorted, no insignificant
    // whitespace, UTF-8 — byte-identical for identical closures.
    [[nodiscard]] std::string canonical_json() const;

    // sha256 hex over canonical_json() — external_source_lock_sha256.
    [[nodiscard]] std::string digest_hex() const;
};

struct SourceLockRequest
{
    std::vector<LockRepository> repositories;
    std::filesystem::path git_executable;
    const processx::ProcessRunner* runner = nullptr;
};

class SourceLockBuilder
{
public:
    // Validates each checkout (clean vs the pinned commit), resolves the
    // exact commit oid, enumerates filtered paths at that commit, hashes
    // every selected blob. Fails closed on the first typed fault.
    [[nodiscard]] qiven::Result<SourceLock, LockError> build(
        const SourceLockRequest& request) const;
};
} // namespace qiven::runtime::cognition
