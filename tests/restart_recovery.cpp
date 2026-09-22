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
using qiven::runtime::InMemoryTestStateStore;
using qiven::runtime::MutationRelation;
using qiven::runtime::ReconciliationBarrier;
using qiven::runtime::TokenHash;

qiven::runtime::auth::SecretKey fixed_secret()
{
    qiven::runtime::auth::SecretKey key {};
    for (qiven::usize i = 0; i < key.size(); ++i)
    {
        key[i] = static_cast<std::byte>(0xC3U ^ i);
    }
    return key;
}
} // namespace

int main()
{
    // C-19: a host restart cannot forget an unresolved dispatched mutation
    // and then permit an unsafe equivalent retry - barriers survive
    {
        ReconciliationBarrier before;
        const EffectScope risky { 100 };
        before.raise(risky, ControlTransactionId { 9 });

        const std::unordered_set<TokenHash> no_tokens;
        const auto image = qiven::runtime::serialize_test_state_image(no_tokens, before);

        // "crash + restart": a FRESH store and barrier reconstructed from
        // the persisted image alone
        InMemoryTestStateStore store;
        QIVEN_VERIFY(store.save(image));
        const auto loaded = store.load();
        QIVEN_VERIFY(loaded.has_value());
        const auto restored_state = qiven::runtime::deserialize_test_state_image(*loaded);
        QIVEN_VERIFY(restored_state.has_value());
        QIVEN_VERIFY(restored_state->barrier_scopes.size() == 1);
        QIVEN_VERIFY(restored_state->barrier_scopes[0] == 100);

        ReconciliationBarrier after;
        qiven::runtime::restore_barriers(after, *restored_state);
        QIVEN_VERIFY(after.active(risky));
        QIVEN_VERIFY(after.check(risky, MutationRelation::Equivalent) == BarrierCheck::Blocked);
    }

    // C-12 across restarts: consumed token HASHES survive; a replayed old
    // ALLOW hits AlreadyConsumed after reconstruction
    {
        qiven::runtime::DecisionToken spent {};
        for (qiven::usize i = 0; i < spent.mac.size(); ++i)
        {
            spent.mac[i] = static_cast<std::byte>(0xDEU);
        }
        for (qiven::usize i = 0; i < spent.nonce.size(); ++i)
        {
            spent.nonce[i] = static_cast<std::byte>(0xADU);
        }
        const TokenHash spent_hash = qiven::runtime::token_hash_of(spent);

        ReconciliationBarrier none;
        const std::unordered_set<TokenHash> tokens { spent_hash };
        const auto image = qiven::runtime::serialize_test_state_image(tokens, none);

        InMemoryTestStateStore store;
        QIVEN_VERIFY(store.save(image));
        const auto restored = qiven::runtime::deserialize_test_state_image(*store.load());
        QIVEN_VERIFY(restored.has_value());
        QIVEN_VERIFY(restored->consumed_token_hashes.size() == 1);
        QIVEN_VERIFY(restored->consumed_token_hashes[0] == spent_hash);

        DecisionLedger after_ledger { fixed_secret() };
        qiven::runtime::seed_consumed_tokens(after_ledger, *restored);
        QIVEN_VERIFY(after_ledger.was_consumed(spent_hash));

        qiven::runtime::ExecutionDecision old_allow;
        old_allow.token      = spent;
        old_allow.token_hash = spent_hash;
        qiven::runtime::FreshnessFacts facts;          // perfect freshness is irrelevant:
        facts.action_digest = old_allow.action_digest; // the token is spent forever
        const auto replay   = after_ledger.consume(old_allow, facts, 1);
        QIVEN_VERIFY(replay.outcome == qiven::runtime::ConsumeOutcome::AlreadyConsumed);
    }

    // serialization is deterministic: same state -> identical bytes; a
    // different state -> different bytes
    {
        ReconciliationBarrier barriers;
        barriers.raise(EffectScope { 5 }, ControlTransactionId { 1 });
        barriers.raise(EffectScope { 6 }, ControlTransactionId { 2 });
        const qiven::runtime::DecisionToken one {};
        const qiven::runtime::DecisionToken two {};
        const std::unordered_set<TokenHash> tokens { qiven::runtime::token_hash_of(one),
                                                     qiven::runtime::token_hash_of(two) };

        const auto first  = qiven::runtime::serialize_test_state_image(tokens, barriers);
        const auto second = qiven::runtime::serialize_test_state_image(tokens, barriers);
        QIVEN_VERIFY(first == second);

        ReconciliationBarrier fewer;
        fewer.raise(EffectScope { 5 }, ControlTransactionId { 1 });
        const auto changed = qiven::runtime::serialize_test_state_image(tokens, fewer);
        QIVEN_VERIFY(changed != first);
    }

    // corrupt/truncated images fail closed, never guessed; the retired
    // QST1 shape (u64 tokens) also fails closed against the QST2 reader
    {
        ReconciliationBarrier barriers;
        const qiven::runtime::DecisionToken token {};
        const auto image = qiven::runtime::serialize_test_state_image({ qiven::runtime::token_hash_of(token) },
                                                                      barriers);

        auto truncated = image;
        truncated.resize(truncated.size() - 1);
        QIVEN_VERIFY(!qiven::runtime::deserialize_test_state_image(truncated).has_value());

        auto trailing = image;
        trailing.push_back(std::byte { 0x00 });
        QIVEN_VERIFY(!qiven::runtime::deserialize_test_state_image(trailing).has_value());

        auto bad_magic = image;
        bad_magic[0]   = std::byte { 0x00 };
        QIVEN_VERIFY(!qiven::runtime::deserialize_test_state_image(bad_magic).has_value());
    }

    // empty store loads nothing
    {
        InMemoryTestStateStore store;
        QIVEN_VERIFY(!store.load().has_value());
    }

    std::printf("[ OK ] restart-recovery\n");
    return 0;
}
