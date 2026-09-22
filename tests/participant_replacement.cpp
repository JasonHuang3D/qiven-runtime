// ============================================================================
// participant_replacement.cpp — C-21 proof: changing the reasoning
// participant preserves control semantics (component ADL §89 RCA-15).
//
// The control plane is participant-independent BY CONSTRUCTION: actor
// identity is DATA from the accepted binding (never classification or
// derivation logic). This batch PROVES it on the real pipeline: two
// participants proposing the same action get identical requirements,
// identical phases and dispositions, independent correlation, per-
// participant failure memory — and one participant's decision can never
// purchase the other's action.
// ============================================================================

#include <qiven/runtime/admission.hpp>
#include <qiven/runtime/failure.hpp>
#include <qiven/runtime/port/cognition_port.hpp>
#include <qiven/runtime/scope.hpp>
#include <qiven/runtime/state.hpp>

#include <qiven/context/persistence.hpp>
#include <qiven/contracts.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>

namespace
{
using qiven::u64;
using qiven::usize;
using qiven::context::ActionKind;
using qiven::context::InvocationRule;
using qiven::context::RequirementBoundary;
using qiven::context::RequirementKind;
using qiven::context::RequirementStatus;
using qiven::context::Snapshot;
using qiven::runtime::BoundedIngressQueue;
using qiven::runtime::ControlCore;
using qiven::runtime::DecisionLedger;
using qiven::runtime::Disposition;
using qiven::runtime::Executor;
using qiven::runtime::FailureKey;
using qiven::runtime::FailureTracker;
using qiven::runtime::IngressMessage;
using qiven::runtime::IntentSet;
using qiven::runtime::StructuralFacts;
using qiven::runtime::TransactionMinter;
using qiven::runtime::TransactionPhase;

class TempFile
{
public:
    explicit TempFile(std::string name) :
    m_path(std::filesystem::temp_directory_path() / name)
    {
        std::error_code ignored;
        std::filesystem::remove(m_path, ignored);
    }
    ~TempFile()
    {
        std::error_code ignored;
        std::filesystem::remove(m_path, ignored);
    }
    void write(const qiven::context::Bytes& bytes) const
    {
        std::ofstream out(m_path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return m_path;
    }

private:
    std::filesystem::path m_path;
};

qiven::runtime::port::PinnedCognition pin_ruled()
{
    Snapshot snapshot;
    snapshot.invocation.present = true;
    InvocationRule rule {};
    rule.action      = ActionKind::BeginTask;
    rule.requirement = RequirementKind::MandatoryRecall;
    rule.subject     = "policy context";
    rule.boundary    = RequirementBoundary::BeforeJudgment;
    snapshot.invocation.rules.push_back(rule);

    static std::atomic<u64> counter { 0 };
    const TempFile file("qiven-rca15-" + std::to_string(counter.fetch_add(1)) + ".bin");
    file.write(qiven::context::serialize_snapshot(snapshot));
    qiven::runtime::port::DraftSnapshotReader reader;
    auto pinned = reader.pin(qiven::runtime::port::PinRequest { file.path(), "" });
    QIVEN_VERIFY(pinned.is_ok());
    return std::move(pinned).value();
}

// One participant's full pipeline pass: propose -> resolve -> allowed.
struct Outcome
{
    bool allowed = false;
    bool same_requirements_as(const Outcome& other) const
    {
        if (requirements.size() != other.requirements.size())
        {
            return false;
        }
        for (usize i = 0; i < requirements.size(); ++i)
        {
            if (requirements[i] != other.requirements[i])
            {
                return false;
            }
        }
        return true;
    }
    std::vector<std::string> requirements;
};

Outcome run_participant(u64 actor_token, const qiven::runtime::port::PinnedCognition& cognition)
{
    TransactionMinter minter;
    Executor pool { 2, 8 };
    BoundedIngressQueue ingress { 16 };

    qiven::runtime::resolver::ResolverBinding binding;
    binding.key.kind           = RequirementKind::MandatoryRecall;
    binding.key.type           = qiven::runtime::resolver::ResolverType::CanonicalRecall;
    binding.key.version        = 1;
    binding.key.cognition_view = "default";
    binding.resolver           = qiven::runtime::resolver::ResolverIdentity { "loopback", 1 };
    binding.accepted_evidence  = qiven::runtime::resolver::EvidenceType::CanonicalRecord;
    binding.trust              = qiven::runtime::resolver::TrustDomain::Mechanism;
    auto registry              = qiven::runtime::resolver::RequirementResolverRegistry::Builder {}
                        .add(binding)
                        .build();
    QIVEN_VERIFY(registry.is_ok());

    qiven::runtime::ResolverMechanism mechanism = [](const qiven::runtime::resolver::ResolverBinding&,
                                                     const qiven::runtime::RequirementIdentity& identity,
                                                     const qiven::runtime::resolver::ReceiptContext& context) {
        return std::make_optional(qiven::runtime::resolver::make_evidence_receipt(
            identity, qiven::runtime::resolver::ResolverIdentity { "loopback", 1 },
            qiven::runtime::resolver::ResolverType::CanonicalRecall,
            qiven::runtime::resolver::EvidenceType::CanonicalRecord, identity.subject, "participant-proof",
            std::span<const std::byte>(), context));
    };

    ControlCore core { minter, pool, registry.value(), std::move(mechanism), cognition, StructuralFacts {} };

    IngressMessage proposal;
    proposal.kind        = IngressMessage::Kind::Proposal;
    proposal.correlation = qiven::runtime::CorrelationKey { qiven::runtime::RuntimeGenerationId { 1 },
                                                            qiven::runtime::AdapterInstanceId { 1 },
                                                            qiven::runtime::HarnessSessionId { 1 },
                                                            qiven::runtime::ActorInstanceId { actor_token },
                                                            qiven::runtime::HarnessActionId { 1 } };
    const std::byte blob[] { std::byte { 0x50 } };
    proposal.action = qiven::runtime::observe_action(qiven::runtime::AdapterInstanceId { 1 },
                                                     qiven::runtime::HarnessSessionId { 1 },
                                                     qiven::runtime::ActorInstanceId { actor_token },
                                                     qiven::runtime::CapabilityId { 1 }, "op", "target",
                                                     std::span<const std::byte>(blob, 1));

    // §8.2: the participant presents cognition injected BEFORE the
    // judgment — a valid activation receipt pre-satisfies the
    // BeforeJudgment requirement at proposal time
    qiven::runtime::port::ActivationReceipt activation;
    activation.kind               = qiven::runtime::port::ActivationKind::SessionStart;
    activation.subject            = "policy context";
    activation.canonical_revision = cognition.revision;
    activation.injected_digest    = cognition.snapshot_digest_sha256;
    activation.generation         = proposal.correlation.generation;
    activation.actor_token        = actor_token;
    activation.session_token      = 1;
    activation.injection_event    = 1;
    proposal.activation_receipts.push_back(activation);
    QIVEN_VERIFY(ingress.try_push(std::move(proposal)));

    for (int spin = 0; spin < 400; ++spin)
    {
        core.drain(ingress);
        const auto views = core.views();
        if (!views.empty() && (views[0].phase == TransactionPhase::CognitiveAllowed ||
                               views[0].phase == TransactionPhase::Denied ||
                               views[0].phase == TransactionPhase::ReDeliberationRequired))
        {
            Outcome outcome;
            outcome.allowed         = views[0].phase == TransactionPhase::CognitiveAllowed;
            const auto* transaction = core.transaction(views[0].id);
            QIVEN_VERIFY(transaction != nullptr);
            for (const auto& prepared : transaction->packet.requirements)
            {
                outcome.requirements.push_back(prepared.requirement.subject);
            }
            return outcome;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    QIVEN_VERIFY(false && "pipeline did not settle");
    return {};
}
} // namespace

int main()
{
    const auto cognition = pin_ruled();

    // C-21 core: the same proposal content under participant A and
    // participant B produces IDENTICAL control semantics — same derived
    // requirements, same phase, same disposition
    {
        const Outcome participant_a = run_participant(101, cognition);
        const Outcome participant_b = run_participant(202, cognition);
        QIVEN_VERIFY(participant_a.allowed);
        QIVEN_VERIFY(participant_b.allowed);
        QIVEN_VERIFY(participant_a.same_requirements_as(participant_b));
        QIVEN_VERIFY(participant_a.requirements.size() == 1);
    }

    // derivation is participant-blind by construction: same snapshot +
    // same action content (identical bytes, only the actor FIELD differs)
    // derives the same requirement set
    {
        const std::byte blob[] { std::byte { 0x51 } };
        const auto action_a = qiven::runtime::observe_action(qiven::runtime::AdapterInstanceId { 1 },
                                                             qiven::runtime::HarnessSessionId { 1 },
                                                             qiven::runtime::ActorInstanceId { 101 },
                                                             qiven::runtime::CapabilityId { 1 }, "op", "target",
                                                             std::span<const std::byte>(blob, 1));
        const auto action_b = qiven::runtime::observe_action(qiven::runtime::AdapterInstanceId { 1 },
                                                             qiven::runtime::HarnessSessionId { 1 },
                                                             qiven::runtime::ActorInstanceId { 202 },
                                                             qiven::runtime::CapabilityId { 1 }, "op", "target",
                                                             std::span<const std::byte>(blob, 1));

        const IntentSet intents_a = classify_with_failure_memory(action_a, StructuralFacts {}, FailureTracker {});
        const IntentSet intents_b = classify_with_failure_memory(action_b, StructuralFacts {}, FailureTracker {});
        const auto set_a          = derive_requirement_set(*cognition.snapshot, intents_a);
        const auto set_b          = derive_requirement_set(*cognition.snapshot, intents_b);
        QIVEN_VERIFY(set_a.instances().size() == set_b.instances().size());
        QIVEN_VERIFY(set_a.find(set_b.instances()[0].identity) != nullptr);
    }

    // failure memory is PER-PARTICIPANT: A's recorded failure does not
    // follow B (the replacement starts clean); A still carries it
    {
        const std::byte blob[] { std::byte { 0x52 } };
        const auto action_a = qiven::runtime::observe_action(qiven::runtime::AdapterInstanceId { 1 },
                                                             qiven::runtime::HarnessSessionId { 1 },
                                                             qiven::runtime::ActorInstanceId { 101 },
                                                             qiven::runtime::CapabilityId { 5 }, "build", "src/x.cpp",
                                                             std::span<const std::byte>(blob, 1));
        const auto action_b = qiven::runtime::observe_action(qiven::runtime::AdapterInstanceId { 1 },
                                                             qiven::runtime::HarnessSessionId { 1 },
                                                             qiven::runtime::ActorInstanceId { 202 },
                                                             qiven::runtime::CapabilityId { 5 }, "build", "src/x.cpp",
                                                             std::span<const std::byte>(blob, 1));

        FailureTracker tracker;
        qiven::context::FailureFingerprint fingerprint;
        fingerprint.tool          = "msvc";
        fingerprint.stableMessage = "C2065";
        tracker.record(FailureKey { 101, 5, qiven::fnv1a64("src/x.cpp") }, fingerprint);

        const auto retry_a = classify_with_failure_memory(action_a, StructuralFacts {}, tracker);
        const auto retry_b = classify_with_failure_memory(action_b, StructuralFacts {}, tracker);
        QIVEN_VERIFY(retry_a.contains_kind(ActionKind::RetryFailure));  // A remembers
        QIVEN_VERIFY(!retry_b.contains_kind(ActionKind::RetryFailure)); // B starts clean
    }

    // correlation independence: identical action content under A and B
    // never collides (the actor dimension separates them), and each
    // participant's idempotent replay is its own
    {
        auto key_for = [](u64 actor) {
            return qiven::runtime::CorrelationKey { qiven::runtime::RuntimeGenerationId { 1 },
                                                    qiven::runtime::AdapterInstanceId { 1 },
                                                    qiven::runtime::HarnessSessionId { 1 },
                                                    qiven::runtime::ActorInstanceId { actor },
                                                    qiven::runtime::HarnessActionId { 7 } };
        };
        QIVEN_VERIFY(key_for(101) != key_for(202));
        QIVEN_VERIFY(qiven::runtime::correlation_hash(key_for(101)) != qiven::runtime::correlation_hash(key_for(202)));
    }

    // a decision bound to A's action can never purchase B's action (the
    // RCA-13 digest defense: the actor field is part of the binding)
    {
        const std::byte blob[] { std::byte { 0x53 } };
        const auto action_a = qiven::runtime::observe_action(qiven::runtime::AdapterInstanceId { 1 },
                                                             qiven::runtime::HarnessSessionId { 1 },
                                                             qiven::runtime::ActorInstanceId { 101 },
                                                             qiven::runtime::CapabilityId { 1 }, "op", "target",
                                                             std::span<const std::byte>(blob, 1));
        const auto action_b = qiven::runtime::observe_action(qiven::runtime::AdapterInstanceId { 1 },
                                                             qiven::runtime::HarnessSessionId { 1 },
                                                             qiven::runtime::ActorInstanceId { 202 },
                                                             qiven::runtime::CapabilityId { 1 }, "op", "target",
                                                             std::span<const std::byte>(blob, 1));
        QIVEN_VERIFY(qiven::runtime::action_digest_of(action_a) != qiven::runtime::action_digest_of(action_b));

        // and the admission coordinator refuses the mismatched pair
        qiven::runtime::port::NullExecutionAuthority authority;
        qiven::runtime::ReconciliationBarrier barriers;

        // a REAL single-use authorization for A's exact action (MVP-0:
        // tokens are cryptographic; hand-made tokens no longer consume)
        qiven::runtime::auth::SecretKey secret {};
        for (usize i = 0; i < secret.size(); ++i)
        {
            secret[i] = static_cast<std::byte>(0x77U ^ i);
        }
        DecisionLedger ledger { secret };

        TransactionMinter tx_minter;
        qiven::runtime::SortableIdMinter id_minter;
        qiven::runtime::profile::ProfileBuilder profile_builder;
        qiven::runtime::profile::GovernedActorSet actors;
        qiven::runtime::profile::ActorBinding binding;
        binding.adapter          = qiven::runtime::AdapterInstanceId { 1 };
        binding.session_token    = 1;
        binding.credential_token = 1;
        actors.actors.push_back(binding);
        auto profile = profile_builder.set_name("participant-decision")
                           .set_revision(qiven::runtime::ProfileRevision { 1 })
                           .set_actor_set(std::move(actors))
                           .set_conformance_evidence("mvp-0-local")
                           .build();
        QIVEN_VERIFY(profile.is_ok());
        qiven::runtime::GenerationMinter generation_minter;
        const auto generation = generation_minter.mint(std::move(profile).value(), {}, {});
        auto scope            = qiven::runtime::ResourceScope::Builder().add_path("target").build();
        QIVEN_VERIFY(scope.is_ok());

        auto intents = qiven::runtime::make_intent_set(
            { qiven::runtime::IntentClassification { qiven::context::ActionIntent {},
                                                     qiven::runtime::ClassificationBasis::Mechanical } });
        QIVEN_VERIFY(intents.is_ok());
        qiven::context::PreparationPacket packet;
        auto transaction = qiven::runtime::begin_control_transaction(
            tx_minter.next(), std::nullopt,
            qiven::runtime::CorrelationKey { qiven::runtime::RuntimeGenerationId { 1 },
                                             qiven::runtime::AdapterInstanceId { 1 },
                                             qiven::runtime::HarnessSessionId { 1 },
                                             qiven::runtime::ActorInstanceId { 101 },
                                             qiven::runtime::HarnessActionId { 1 } },
            action_a, std::move(intents).value(), qiven::runtime::port::PinnedCognition {}, std::move(packet));
        QIVEN_VERIFY(qiven::runtime::evaluate_before_judgment(transaction));
        QIVEN_VERIFY(qiven::runtime::evaluate_before_execution(transaction));

        auto bound = qiven::runtime::bind_allow(transaction, generation, scope.value().digest(), 1, {}, secret,
                                                id_minter, 1'000'000, 3'600'000);
        QIVEN_VERIFY(bound.is_ok());

        qiven::runtime::FreshnessFacts facts;
        facts.action_digest              = bound.value().action_digest;
        facts.generation                 = bound.value().generation;
        facts.cognition_revision         = bound.value().cognition_revision;
        facts.profile                    = bound.value().profile;
        facts.resolver_registry_revision = bound.value().resolver_registry_revision;
        facts.resource_scope_digest      = bound.value().resource_scope_digest;
        const auto consumed              = ledger.consume(bound.value(), facts, 1'000'001);
        QIVEN_VERIFY(consumed.outcome == qiven::runtime::ConsumeOutcome::Consumed);

        const auto result = qiven::runtime::ActionAdmissionCoordinator { authority, barriers }
                                .compose(bound.value(), consumed, action_b, // ...but B's ACTION is presented
                                         qiven::runtime::CorrelationKey { qiven::runtime::RuntimeGenerationId { 1 },
                                                                          qiven::runtime::AdapterInstanceId { 1 },
                                                                          qiven::runtime::HarnessSessionId { 1 },
                                                                          qiven::runtime::ActorInstanceId { 202 },
                                                                          qiven::runtime::HarnessActionId { 1 } },
                                         qiven::runtime::port::AdmissionPurpose::SimulatedProof);
        QIVEN_VERIFY(result.outcome == qiven::runtime::AdmissionOutcome::DeniedNoCognitiveAllow);
    }

    std::printf("[ OK ] participant-replacement\n");
    return 0;
}
