#pragma once

// ============================================================================
// state_store.hpp — TEST-ONLY control-state image (component ADL §57/§58;
// obligation C-19; production-MVP architecture §14.2, MVP-0)
//
// STATUS (MVP-0, ADR-0047): this image is NOT production recovery state.
// It contains only consumed-token hashes and barrier scopes — far too
// thin to recover dispatches, outcomes, leases, or correlations
// (production-MVP §2: "the current image ... cannot recover dispatches,
// outcomes, leases, or correlations"). Production recovery state lives
// behind IRuntimeJournalPort (port/runtime_journal.hpp), implemented by
// the SQLite control journal in MVP-1. This header remains the
// single-process test vehicle: unit tests, reference semantics, and
// the in-memory proof store.
//
// What the test image proves: consumed decision-token HASHES (an old
// ALLOW must not resurrect) and active reconciliation barriers (an
// unresolved dispatched mutation must not be forgotten, C-19). The
// acceptance levels of §57 stay explicit: the in-memory store proves
// single-process semantics only; a PRODUCTION mutating profile requires
// the RuntimeJournal path.
// ============================================================================

#include <qiven/runtime/decision.hpp>
#include <qiven/runtime/reconciliation.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace qiven::runtime
{
// Canonical serialized TEST-ONLY control state: deterministic bytes
// (fixed-width little-endian, same representation law as every other
// runtime digest preimage). Shape version "QST2": token entries are
// 32-byte SHA-256 token hashes (QST1's u64 token values are retired —
// a 64-bit value is not a single-use authorization identity).
[[nodiscard]] std::vector<std::byte> serialize_test_state_image(const std::unordered_set<TokenHash>& consumed,
                                                                const ReconciliationBarrier& barriers);

// TEST-ONLY image (see the header status note).
struct TestOnlyStateImage
{
    std::vector<TokenHash> consumed_token_hashes;
    std::vector<u64> barrier_scopes;
};

// Inverse of serialize_test_state_image; fails closed (nullopt) on
// truncated/corrupt input rather than guessing.
[[nodiscard]] std::optional<TestOnlyStateImage> deserialize_test_state_image(std::span<const std::byte> bytes);

// In-memory single-process store: TEST acceptance level (§57). Not a
// production recovery substrate.
class InMemoryTestStateStore
{
public:
    // Persist the image; returns false when the store refuses
    // (capacity/health) — never silently pretends success.
    [[nodiscard]] bool save(std::span<const std::byte> image);

    // Latest persisted image; nullopt when nothing was ever saved.
    [[nodiscard]] std::optional<std::vector<std::byte>> load() const;

private:
    std::vector<std::byte> m_image;
    bool m_has_image = false;
};

// C-19 restart reconstruction (test vehicle): rebuild the post-restart
// barrier set (and consumed-token ledger) from a persisted image.
void restore_barriers(ReconciliationBarrier& barriers, const TestOnlyStateImage& image);

// Pre-seed a ledger with token hashes consumed before the restart: a
// replayed old ALLOW hits AlreadyConsumed (C-12 across restarts).
void seed_consumed_tokens(DecisionLedger& ledger, const TestOnlyStateImage& image);
} // namespace qiven::runtime
