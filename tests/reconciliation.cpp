#include <qiven/runtime/reconciliation.hpp>

#include <qiven/contracts.hpp>

#include <cstdio>

namespace
{
using qiven::runtime::BarrierCheck;
using qiven::runtime::ControlTransactionId;
using qiven::runtime::EffectScope;
using qiven::runtime::MutationRelation;
using qiven::runtime::ReconciliationBarrier;
using qiven::runtime::ReconciliationCoordinator;
using qiven::runtime::ReconciliationEvidence;
using qiven::runtime::ReconciliationResult;
} // namespace

int main()
{
    // C-15: an indeterminate action barriers its scope; equivalent
    // mutations, retries and publication are blocked there
    {
        ReconciliationBarrier barrier;
        const EffectScope docs { 10 };
        barrier.raise(docs, ControlTransactionId { 1 });

        QIVEN_VERIFY(barrier.active(docs));
        QIVEN_VERIFY(barrier.check(docs, MutationRelation::Equivalent) == BarrierCheck::Blocked);

        // independent scopes continue (51: no global stop)
        const EffectScope other { 20 };
        QIVEN_VERIFY(barrier.check(other, MutationRelation::Equivalent) == BarrierCheck::Allowed);
        QIVEN_VERIFY(barrier.check(docs, MutationRelation::Independent) == BarrierCheck::Allowed);
        QIVEN_VERIFY(barrier.active_count() == 1);
    }

    // C-16: unknown outcome is never failure and never retry permission;
    // no authoritative answer leaves the barrier in force
    {
        ReconciliationBarrier barrier;
        const EffectScope scope { 30 };
        barrier.raise(scope, ControlTransactionId { 2 });

        ReconciliationCoordinator coordinator { barrier };
        const auto still = coordinator.reconcile(scope, ReconciliationEvidence::None, "no journal entry");
        QIVEN_VERIFY(still.result == ReconciliationResult::StillIndeterminate);
        QIVEN_VERIFY(barrier.active(scope));          // barrier stays
        QIVEN_VERIFY(!still.fingerprint.has_value()); // no failure minted

        // even AUTHORITATIVE evidence with empty detail proves nothing
        const auto empty = coordinator.reconcile(scope, ReconciliationEvidence::AuthoritativeObservation, "");
        QIVEN_VERIFY(empty.result == ReconciliationResult::StillIndeterminate);
        QIVEN_VERIFY(barrier.active(scope));
    }

    // 52/53: authoritative success resolves, lifts the barrier, and the
    // scope becomes workable again
    {
        ReconciliationBarrier barrier;
        const EffectScope scope { 40 };
        barrier.raise(scope, ControlTransactionId { 3 });

        ReconciliationCoordinator coordinator { barrier };
        const auto resolved = coordinator.reconcile(scope, ReconciliationEvidence::AuthoritativeObservation,
                                                    "journal: object exists at head");
        QIVEN_VERIFY(resolved.result == ReconciliationResult::ResolvedSucceeded);
        QIVEN_VERIFY(!barrier.active(scope));
        QIVEN_VERIFY(barrier.check(scope, MutationRelation::Equivalent) == BarrierCheck::Allowed);
        QIVEN_VERIFY(coordinator.resolutions() == 1);
    }

    // authoritative failure resolves, lifts the barrier, and hands a
    // NORMALIZED fingerprint to the retry-discipline path (53: only after
    // resolution does the normal state machine resume)
    {
        ReconciliationBarrier barrier;
        const EffectScope scope { 50 };
        barrier.raise(scope, ControlTransactionId { 4 });

        ReconciliationCoordinator coordinator { barrier };
        const auto failed = coordinator.reconcile(scope, ReconciliationEvidence::AuthoritativeObservation,
                                                  "failed: exit 3 at 2026-09-21T18:00:01Z");
        QIVEN_VERIFY(failed.result == ReconciliationResult::ResolvedFailed);
        QIVEN_VERIFY(!barrier.active(scope));
        QIVEN_VERIFY(failed.fingerprint.has_value());
        QIVEN_VERIFY(failed.fingerprint->stableMessage.find("18:00:01") == std::string::npos); // normalized
        QIVEN_VERIFY(!failed.fingerprint->stableMessage.empty());
    }

    // repeated StillIndeterminate keeps the barrier across many attempts
    {
        ReconciliationBarrier barrier;
        const EffectScope scope { 60 };
        barrier.raise(scope, ControlTransactionId { 5 });
        ReconciliationCoordinator coordinator { barrier };
        for (int attempt = 0; attempt < 5; ++attempt)
        {
            const auto result = coordinator.reconcile(scope, ReconciliationEvidence::None, "silent");
            QIVEN_VERIFY(result.result == ReconciliationResult::StillIndeterminate);
        }
        QIVEN_VERIFY(barrier.active(scope));
        QIVEN_VERIFY(coordinator.resolutions() == 5);
    }

    std::printf("[ OK ] reconciliation\n");
    return 0;
}
