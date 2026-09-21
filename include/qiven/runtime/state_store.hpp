#pragma once

// ============================================================================
// state_store.hpp — RuntimeStateStore: restart/recovery state (component
// ADL §57/§58; obligation C-19)
//
// What must survive a RuntimeHost restart: the single-use decision tokens
// already consumed (an old ALLOW must not resurrect), and the active
// reconciliation barriers (an unresolved dispatched mutation must not be
// forgotten and then permit an unsafe equivalent retry — C-19).
//
// Acceptance levels are explicit (§57): the in-memory store proves
// single-process semantics (unit tests, reference implementation,
// non-mutating demonstrations); a PRODUCTION mutating profile requires a
// restart-capable store or an accepted authoritative journal with
// reconstruction evidence. The interface is the contract; the in-memory
// implementation is the proof vehicle, and its serialized form is what a
// durable store would persist.
// ============================================================================

#include <qiven/runtime/decision.hpp>
#include <qiven/runtime/reconciliation.hpp>

#include <cstddef>
#include <memory>
#include <span>
#include <vector>

namespace qiven::runtime
{
// Canonical serialized control state: deterministic bytes (fixed-width
// little-endian, same representation law as every other runtime digest
// preimage). What a durable store persists; what restart reconstructs
// from.
[[nodiscard]] std::vector<std::byte> serialize_control_state(const std::unordered_set<u64>& consumed_tokens,
                                                             const ReconciliationBarrier& barriers);

struct ControlStateImage
{
    std::vector<u64> consumed_tokens;
    std::vector<u64> barrier_scopes;
};

// Inverse of serialize_control_state; fails closed (nullopt) on
// truncated/corrupt input rather than guessing.
[[nodiscard]] std::optional<ControlStateImage> deserialize_control_state(std::span<const std::byte> bytes);

class RuntimeStateStore
{
public:
    virtual ~RuntimeStateStore() = default;

    // Persist the state image; returns false when the store refuses
    // (capacity/health) — never silently pretends success.
    [[nodiscard]] virtual bool save(std::span<const std::byte> image) = 0;

    // Latest persisted image; nullopt when nothing was ever saved.
    [[nodiscard]] virtual std::optional<std::vector<std::byte>> load() const = 0;
};

// §57 acceptance level: in-memory, single-process proof vehicle.
class InMemoryRuntimeStateStore final : public RuntimeStateStore
{
public:
    [[nodiscard]] bool save(std::span<const std::byte> image) override;
    [[nodiscard]] std::optional<std::vector<std::byte>> load() const override;

private:
    std::vector<std::byte> m_image;
    bool m_has_image = false;
};

// C-19 restart reconstruction: rebuild the post-restart barrier set (and
// consumed-token ledger) from a persisted image. The caller applies the
// tokens to its DecisionLedger via was_consumed-style replay (the ledger
// accepts pre-seeded consumed tokens through its store-backed
// reconstruction).
void restore_barriers(ReconciliationBarrier& barriers, const ControlStateImage& image);

class DecisionLedger; // forward: seeding declared in decision.hpp terms

// Pre-seed a ledger with tokens consumed before the restart: a replayed
// old ALLOW hits AlreadyConsumed (C-12 across restarts).
void seed_consumed_tokens(DecisionLedger& ledger, const ControlStateImage& image);
} // namespace qiven::runtime
