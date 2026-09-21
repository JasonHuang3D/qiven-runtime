#pragma once

// ============================================================================
// failure.hpp — FailureTracker: known failures change the NEXT equivalent
// control state without participant memory (component ADL §48/§17;
// obligation C-17)
//
// A mechanically established failure is indexed by its normalized
// fingerprint (actor/tool/target keys). When an equivalent proposal
// arrives, classification gains a RetryFailure intent carrying the prior
// fingerprint — the frozen v4 semantics then demand InspectKnownPit /
// new evidence before the retry may proceed (§53 retry discipline).
// The participant does not need to remember the failure; the runtime
// does.
// ============================================================================

#include <qiven/runtime/intent.hpp>
#include <qiven/runtime/observed_action.hpp>
#include <qiven/runtime/outcome.hpp>

#include <qiven/context/runtime.hpp>

#include <optional>
#include <unordered_map>
#include <vector>

namespace qiven::runtime
{
// Index key: the triple a retry is "equivalent" over. Stable across
// incidental text by construction (the fingerprint is normalized).
struct FailureKey
{
    u64 actor      = 0;
    u64 capability = 0;
    u64 target     = 0;

    [[nodiscard]] bool operator==(const FailureKey& other) const noexcept
    {
        return actor == other.actor && capability == other.capability && target == other.target;
    }
};

class FailureTracker
{
public:
    // Record an established failure (§48). Duplicate fingerprints for the
    // same key increment a recurrence count.
    void record(const FailureKey& key, qiven::context::FailureFingerprint fingerprint);

    // C-17: does an equivalent proposal carry a known failure? When yes,
    // the RetryFailure intent carrying the MOST RECENT fingerprint is
    // returned for the classifier to include.
    [[nodiscard]] std::optional<qiven::context::ActionIntent> retry_intent_for(const FailureKey& key) const;

    [[nodiscard]] usize distinct_failures() const noexcept
    {
        return m_entries.size();
    }

    void clear() noexcept
    {
        m_entries.clear();
    }

private:
    struct Entry
    {
        qiven::context::FailureFingerprint fingerprint;
        u64 recurrence = 1;
    };

    struct KeyHash
    {
        [[nodiscard]] usize operator()(const FailureKey& key) const noexcept
        {
            u64 seed = key.actor;
            seed ^= key.capability + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
            seed ^= key.target + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
            return static_cast<usize>(seed);
        }
    };

    std::unordered_map<FailureKey, Entry, KeyHash> m_entries;
};

// The C-17 classification hook: an action whose failure triple has a
// recorded failure classifies with the RetryFailure intent ADDED to the
// mechanical floor (the set is never narrowed, ADL §21 - retry discipline
// is an ADDITIONAL demand, not a replacement).
[[nodiscard]] IntentSet classify_with_failure_memory(const ObservedAction& action, const StructuralFacts& facts,
                                                     const FailureTracker& failures);
} // namespace qiven::runtime
