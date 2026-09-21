#pragma once

// ============================================================================
// reconciliation.hpp — ReconciliationBarrier and ReconciliationCoordinator
// (component ADL §49-§53; obligations C-15/C-16, design §11 second half)
//
// An Indeterminate outcome creates a barrier over the action's
// EffectScope: while active, equivalent mutations, conflicting mutations,
// retries and success/failure publication within that scope are BLOCKED
// unless recovery policy explicitly proves safety. INDEPENDENT scopes
// continue — no global stop for one uncertainty.
//
// Reconciliation is NOT retry (§53): unknown ≠ failed ≠ permission. The
// coordinator seeks AUTHORITATIVE observation and resolves
// ResolvedSucceeded / ResolvedFailed / StillIndeterminate; StillIndeterminate
// leaves the barrier in force. Only after resolution does the normal
// state machine resume (a ResolvedFailed hands a fingerprint to the
// FailureTracker path).
// ============================================================================

#include <qiven/runtime/identity.hpp>
#include <qiven/runtime/outcome.hpp>

#include <qiven/context/runtime.hpp>

#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace qiven::runtime
{
enum class BarrierCheck : u8
{
    Allowed, // no active barrier touches the scope
    Blocked, // an active barrier over this scope forbids the operation
};

enum class MutationRelation : u8
{
    Independent, // a different EffectScope entirely (§51: continues)
    Equivalent,  // same world region as the uncertain action
};

class ReconciliationBarrier
{
public:
    // An indeterminate action raises the barrier over its scope (§51).
    void raise(EffectScope scope, const ControlTransactionId& transaction);

    // §51 blocking rule. Equivalent mutations in a barrier scope are
    // blocked; independent scopes are allowed.
    [[nodiscard]] BarrierCheck check(const EffectScope& scope, MutationRelation relation) const noexcept;

    [[nodiscard]] bool active(const EffectScope& scope) const noexcept;

    [[nodiscard]] usize active_count() const noexcept
    {
        return m_scopes.size();
    }

    // Sorted active scope ids (state-store serialization order).
    [[nodiscard]] std::vector<u64> active_scope_ids() const;

    // Lift the barrier (only the coordinator resolves; §52/§53).
    void lift(const EffectScope& scope) noexcept;

private:
    std::unordered_map<u64, ControlTransactionId> m_scopes;
};

enum class ReconciliationResult : u8
{
    ResolvedSucceeded,
    ResolvedFailed,
    StillIndeterminate, // the barrier stays (§52)
};

enum class ReconciliationEvidence : u8
{
    AuthoritativeObservation, // host journal, git state, filesystem identity...
    None,                     // no authoritative source answered
};

class ReconciliationCoordinator
{
public:
    explicit ReconciliationCoordinator(ReconciliationBarrier& barrier);

    // §52/§53: resolution from AUTHORITATIVE observation only. Authoritative
    // success lifts the barrier and returns completion evidence;
    // authoritative failure lifts it and mints a normalized fingerprint
    // (retry discipline resumes through the FailureTracker path);
    // no authoritative answer is StillIndeterminate — barrier stays, and
    // nothing about it is treated as failure or permission.
    struct Outcome
    {
        ReconciliationResult result = ReconciliationResult::StillIndeterminate;
        std::optional<qiven::context::FailureFingerprint> fingerprint; // ResolvedFailed only
    };

    [[nodiscard]] Outcome reconcile(const EffectScope& scope, ReconciliationEvidence evidence,
                                    const std::string& detail);

    [[nodiscard]] u64 resolutions() const noexcept
    {
        return m_resolutions;
    }

private:
    ReconciliationBarrier& m_barrier;
    u64 m_resolutions = 0;
};
} // namespace qiven::runtime
