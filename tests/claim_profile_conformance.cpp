// ============================================================================
// claim_profile_conformance.cpp — C-23 proof: claim-channel honesty of the
// first landing (component ADL §89 RCA-16, §16/§67/§76).
//
// The first hard-governed profile is hard-enforcement-first, NOT
// advisory (§76), and its claim coverage is EXPLICIT DATA: tool-mediated
// governed, free-text not claimed. A profile claiming free-text coverage
// is rejected at construction. Routing never lets a free-response claim
// masquerade as governed, and the disposition vocabulary's NotGoverned
// is producible only by claim-coverage analysis — no phase ever emits it.
// ============================================================================

#include <qiven/runtime/adapter/loopback.hpp>
#include <qiven/runtime/claims.hpp>
#include <qiven/runtime/profile.hpp>
#include <qiven/runtime/transaction.hpp>

#include <qiven/contracts.hpp>

#include <cstdio>

namespace
{
using qiven::runtime::Disposition;
using qiven::runtime::TransactionPhase;
using qiven::runtime::port::ClaimChannel;
using qiven::runtime::port::ClaimChannelResult;
using qiven::runtime::port::OutboundClaim;
using qiven::runtime::profile::ClaimChannelCoverage;
using qiven::runtime::profile::ProfileBuilder;

ClaimChannelCoverage tool_only()
{
    return ClaimChannelCoverage { true, false };
}
} // namespace

int main()
{
    // the first landing's coverage is explicit profile DATA: tool-mediated
    // governed, free text NOT claimed (§67 first-landing report)
    {
        const ClaimChannelCoverage coverage = tool_only();
        QIVEN_VERIFY(coverage.tool_mediated);
        QIVEN_VERIFY(!coverage.free_text);
    }

    // C-23 builder enforcement: a profile that would CLAIM free-text hard
    // enforcement is rejected at construction — the honesty rule is
    // structural, not a runtime promise
    {
        auto honest = ProfileBuilder {}
                          .set_name("c23-honest")
                          .set_revision(qiven::runtime::ProfileRevision { 1 })
                          .set_claims(tool_only())
                          .set_conformance_evidence("rca-16")
                          .build();
        QIVEN_VERIFY(honest.is_ok());

        auto claiming = ProfileBuilder {}
                            .set_name("c23-claiming")
                            .set_revision(qiven::runtime::ProfileRevision { 1 })
                            .set_claims(ClaimChannelCoverage { true, true })
                            .set_conformance_evidence("rca-16")
                            .build();
        QIVEN_VERIFY(!claiming.is_ok());
        QIVEN_VERIFY(claiming.reason().code == 6);
    }

    // routing honesty: tool-mediated governed; free-response NotGoverned —
    // never a silent Allow (both the loopback and the port contract)
    {
        qiven::runtime::adapter::LoopbackAdapter adapter("c23-loop", 1);

        OutboundClaim commit_message;
        commit_message.channel          = ClaimChannel::ToolMediated;
        commit_message.channel_identity = "commit-message";
        QIVEN_VERIFY(adapter.route_claim(commit_message) == ClaimChannelResult::Governed);

        OutboundClaim assistant_text;
        assistant_text.channel = ClaimChannel::FreeResponse;
        QIVEN_VERIFY(adapter.route_claim(assistant_text) == ClaimChannelResult::NotGoverned);
    }

    // NotGoverned is producible ONLY by claim-coverage analysis: no
    // transaction phase maps to it in the disposition projection — a
    // hard-governed channel can never let NotGoverned masquerade as
    // ALLOW (§75)
    {
        const TransactionPhase phases[] {
            TransactionPhase::Observed,
            TransactionPhase::Classified,
            TransactionPhase::PolicyPinned,
            TransactionPhase::RequirementsDerived,
            TransactionPhase::PreparingBeforeJudgment,
            TransactionPhase::Denied,
            TransactionPhase::ReDeliberationRequired,
            TransactionPhase::PreparedForJudgment,
            TransactionPhase::PreparingBeforeExecution,
            TransactionPhase::AwaitingRequirement,
            TransactionPhase::CognitiveAllowed,
            TransactionPhase::Stale,
        };

        qiven::runtime::TransactionMinter minter;
        // disposition_for only reads the phase; construct a minimal
        // transaction-like view per phase
        for (const TransactionPhase phase : phases)
        {
            const std::byte blob[] { std::byte { 0x00 } };
            auto action = qiven::runtime::observe_action(
                qiven::runtime::AdapterInstanceId { 1 }, qiven::runtime::HarnessSessionId { 1 },
                qiven::runtime::ActorInstanceId { 1 }, qiven::runtime::CapabilityId { 1 }, "op", "t",
                std::span<const std::byte>(blob, 1));
            auto intents = qiven::runtime::make_intent_set(
                { qiven::runtime::IntentClassification { qiven::context::ActionIntent {},
                                                         qiven::runtime::ClassificationBasis::Mechanical } });
            QIVEN_VERIFY(intents.is_ok());
            const qiven::runtime::CorrelationKey key { qiven::runtime::RuntimeGenerationId { 1 },
                                                       qiven::runtime::AdapterInstanceId { 1 },
                                                       qiven::runtime::HarnessSessionId { 1 },
                                                       qiven::runtime::ActorInstanceId { 1 },
                                                       qiven::runtime::HarnessActionId { 1 } };
            auto transaction = qiven::runtime::begin_control_transaction(
                minter.next(), std::nullopt, key, std::move(action), std::move(intents).value(),
                qiven::runtime::port::PinnedCognition {}, qiven::context::PreparationPacket {});
            transaction.phase = phase;
            QIVEN_VERIFY(qiven::runtime::disposition_for(transaction) != Disposition::NotGoverned);
        }
    }

    // §76 posture check: the default first profile is hard-enforcement-
    // first — evidenced by the handshake failing closed (RCA-1) and the
    // admission composition (RCA-13); here we assert the profile data
    // carries a real conformance citation (no advisory-only profile
    // without evidence is constructible)
    {
        auto no_evidence = ProfileBuilder {}
                               .set_name("advisory-no-evidence")
                               .set_revision(qiven::runtime::ProfileRevision { 1 })
                               .set_claims(tool_only())
                               .build();
        QIVEN_VERIFY(!no_evidence.is_ok());
    }

    std::printf("[ OK ] claim-profile-conformance\n");
    return 0;
}
