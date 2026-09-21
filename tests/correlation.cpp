#include <qiven/runtime/identity.hpp>

#include <qiven/contracts.hpp>

#include <cstdio>
#include <unordered_set>

namespace
{
using qiven::u64;
using qiven::runtime::ActorInstanceId;
using qiven::runtime::AdapterInstanceId;
using qiven::runtime::CorrelationKey;
using qiven::runtime::HarnessActionId;
using qiven::runtime::HarnessSessionId;
using qiven::runtime::RuntimeGenerationId;

CorrelationKey sample_key()
{
    return CorrelationKey { RuntimeGenerationId { 3 }, AdapterInstanceId { qiven::fnv1a64("loopback") },
                            HarnessSessionId { 12 }, ActorInstanceId { 40 }, HarnessActionId { 99001 } };
}
} // namespace

int main()
{
    // C-18: the key is a value — copies are independent, equality is the
    // five-tuple, and there is no ambient "currentAction"
    {
        const CorrelationKey key  = sample_key();
        const CorrelationKey copy = key;
        QIVEN_VERIFY(key == copy);

        CorrelationKey mutated = copy;
        mutated.action.value   = 99002;
        QIVEN_VERIFY(mutated != key);
        QIVEN_VERIFY(copy == key); // the copy was never touched
    }

    // each tuple element participates in identity
    {
        const CorrelationKey base = sample_key();

        CorrelationKey next_generation   = base;
        next_generation.generation.value = 4;
        QIVEN_VERIFY(next_generation != base);

        CorrelationKey other_adapter = base;
        other_adapter.adapter.fnv    = 42;
        QIVEN_VERIFY(other_adapter != base);

        CorrelationKey other_session = base;
        other_session.session.value  = 13;
        QIVEN_VERIFY(other_session != base);

        CorrelationKey other_actor = base;
        other_actor.actor.value    = 41;
        QIVEN_VERIFY(other_actor != base);

        CorrelationKey other_action = base;
        other_action.action.value   = 99002;
        QIVEN_VERIFY(other_action != base);
    }

    // hashing contract: equal keys hash equal; the key is usable in hashed
    // containers for the replay/conflict checks of later batches
    {
        const CorrelationKey key = sample_key();

        const std::hash<CorrelationKey> hasher {};
        QIVEN_VERIFY(hasher(key) == hasher(sample_key()));
        QIVEN_VERIFY(qiven::runtime::correlation_hash(key) == qiven::runtime::correlation_hash(sample_key()));

        std::unordered_set<CorrelationKey> seen;
        seen.insert(key);
        QIVEN_VERIFY(seen.contains(sample_key()));

        CorrelationKey other = key;
        other.action.value   = 99002;
        seen.insert(other);
        QIVEN_VERIFY(seen.size() == 2);
    }

    // the hash is deterministic identity, not integrity: stable across the
    // whole lifetime of one key (fnv1a64 over the tuple fields)
    {
        const CorrelationKey key = sample_key();
        u64 first                = qiven::runtime::correlation_hash(key);
        u64 second               = qiven::runtime::correlation_hash(sample_key());
        QIVEN_VERIFY(first == second);
    }

    std::printf("[ OK ] correlation\n");
    return 0;
}
