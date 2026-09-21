#include <qiven/runtime/state_store.hpp>

#include <qiven/contracts.hpp>

#include <cstdio>
#include <unordered_set>

namespace
{
using qiven::u64;
using qiven::runtime::BarrierCheck;
using qiven::runtime::ControlTransactionId;
using qiven::runtime::DecisionLedger;
using qiven::runtime::EffectScope;
using qiven::runtime::InMemoryRuntimeStateStore;
using qiven::runtime::MutationRelation;
using qiven::runtime::ReconciliationBarrier;
} // namespace

int main()
{
    // C-19: a host restart cannot forget an unresolved dispatched mutation
    // and then permit an unsafe equivalent retry - barriers survive
    {
        ReconciliationBarrier before;
        const EffectScope risky { 100 };
        before.raise(risky, ControlTransactionId { 9 });

        const std::unordered_set<u64> no_tokens;
        const auto image = qiven::runtime::serialize_control_state(no_tokens, before);

        // "crash + restart": a FRESH store and barrier reconstructed from
        // the persisted image alone
        InMemoryRuntimeStateStore store;
        QIVEN_VERIFY(store.save(image));
        const auto loaded = store.load();
        QIVEN_VERIFY(loaded.has_value());
        const auto restored_state = qiven::runtime::deserialize_control_state(*loaded);
        QIVEN_VERIFY(restored_state.has_value());
        QIVEN_VERIFY(restored_state->barrier_scopes.size() == 1);
        QIVEN_VERIFY(restored_state->barrier_scopes[0] == 100);

        ReconciliationBarrier after;
        qiven::runtime::restore_barriers(after, *restored_state);
        QIVEN_VERIFY(after.active(risky));
        QIVEN_VERIFY(after.check(risky, MutationRelation::Equivalent) == BarrierCheck::Blocked);
    }

    // C-12 across restarts: consumed tokens survive; a replayed old ALLOW
    // hits AlreadyConsumed after reconstruction
    {
        const u64 spent_token = 0xDEADBEEFull;

        ReconciliationBarrier none;
        const std::unordered_set<u64> tokens { spent_token };
        const auto image = qiven::runtime::serialize_control_state(tokens, none);

        InMemoryRuntimeStateStore store;
        QIVEN_VERIFY(store.save(image));
        const auto restored = qiven::runtime::deserialize_control_state(*store.load());
        QIVEN_VERIFY(restored.has_value());
        QIVEN_VERIFY(restored->consumed_tokens.size() == 1);
        QIVEN_VERIFY(restored->consumed_tokens[0] == spent_token);

        DecisionLedger after_ledger;
        qiven::runtime::seed_consumed_tokens(after_ledger, *restored);
        QIVEN_VERIFY(after_ledger.was_consumed(spent_token));

        qiven::runtime::ExecutionDecision old_allow;
        old_allow.token = qiven::runtime::DecisionToken { spent_token };
        qiven::runtime::FreshnessFacts facts;          // perfect freshness is irrelevant:
        facts.action_digest = old_allow.action_digest; // the token is spent forever
        const auto replay   = after_ledger.consume(old_allow, facts);
        QIVEN_VERIFY(replay.outcome == qiven::runtime::ConsumeOutcome::AlreadyConsumed);
    }

    // serialization is deterministic: same state -> identical bytes; a
    // different state -> different bytes
    {
        ReconciliationBarrier barriers;
        barriers.raise(EffectScope { 5 }, ControlTransactionId { 1 });
        barriers.raise(EffectScope { 6 }, ControlTransactionId { 2 });
        const std::unordered_set<u64> tokens { 3, 1, 2 };

        const auto first  = qiven::runtime::serialize_control_state(tokens, barriers);
        const auto second = qiven::runtime::serialize_control_state(tokens, barriers);
        QIVEN_VERIFY(first == second);

        ReconciliationBarrier fewer;
        fewer.raise(EffectScope { 5 }, ControlTransactionId { 1 });
        const auto changed = qiven::runtime::serialize_control_state(tokens, fewer);
        QIVEN_VERIFY(changed != first);
    }

    // corrupt/truncated images fail closed, never guessed
    {
        ReconciliationBarrier barriers;
        const auto image = qiven::runtime::serialize_control_state({ 7 }, barriers);

        auto truncated = image;
        truncated.resize(truncated.size() - 1);
        QIVEN_VERIFY(!qiven::runtime::deserialize_control_state(truncated).has_value());

        auto trailing = image;
        trailing.push_back(std::byte { 0x00 });
        QIVEN_VERIFY(!qiven::runtime::deserialize_control_state(trailing).has_value());

        auto bad_magic = image;
        bad_magic[0]   = std::byte { 0x00 };
        QIVEN_VERIFY(!qiven::runtime::deserialize_control_state(bad_magic).has_value());
    }

    // empty store loads nothing
    {
        InMemoryRuntimeStateStore store;
        QIVEN_VERIFY(!store.load().has_value());
    }

    std::printf("[ OK ] restart-recovery\n");
    return 0;
}
