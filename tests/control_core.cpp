#include <qiven/runtime/state.hpp>

#include <qiven/context/persistence.hpp>
#include <qiven/contracts.hpp>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

namespace
{
using qiven::u64;
using qiven::context::ActionKind;
using qiven::context::InvocationRule;
using qiven::context::RequirementBoundary;
using qiven::context::RequirementKind;
using qiven::context::Snapshot;
using qiven::runtime::BoundedIngressQueue;
using qiven::runtime::ControlCore;
using qiven::runtime::DrainResult;
using qiven::runtime::Executor;
using qiven::runtime::IngressMessage;
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

Snapshot ruled_snapshot()
{
    Snapshot snapshot;
    snapshot.invocation.present = true;

    InvocationRule before_judgment {};
    before_judgment.action      = ActionKind::BeginTask;
    before_judgment.requirement = RequirementKind::MandatoryRecall;
    before_judgment.subject     = "policy context";
    before_judgment.boundary    = RequirementBoundary::BeforeJudgment;
    snapshot.invocation.rules.push_back(before_judgment);

    InvocationRule before_execution {};
    before_execution.action      = ActionKind::BeginTask;
    before_execution.requirement = RequirementKind::RunMechanicalCheck;
    before_execution.subject     = "gate receipt";
    before_execution.boundary    = RequirementBoundary::BeforeExecution;
    snapshot.invocation.rules.push_back(before_execution);

    return snapshot;
}

qiven::runtime::port::PinnedCognition pin_of(const Snapshot& snapshot, const char* name)
{
    static std::atomic<u64> counter { 0 };
    const TempFile file(std::string(name) + std::to_string(counter.fetch_add(1)) + ".bin");
    file.write(qiven::context::serialize_snapshot(snapshot));
    qiven::runtime::port::DraftSnapshotReader reader;
    auto pinned = reader.pin(qiven::runtime::port::PinRequest { file.path(), "" });
    QIVEN_VERIFY(pinned.is_ok());
    return std::move(pinned).value(); // pin owns the snapshot; file may vanish
}

qiven::runtime::resolver::RequirementResolverRegistry make_registry()
{
    qiven::runtime::resolver::ResolverBinding recall;
    recall.key.kind           = RequirementKind::MandatoryRecall;
    recall.key.type           = qiven::runtime::resolver::ResolverType::CanonicalRecall;
    recall.key.version        = 1;
    recall.key.cognition_view = "default";
    recall.resolver           = qiven::runtime::resolver::ResolverIdentity { "loopback", 1 };
    recall.accepted_evidence  = qiven::runtime::resolver::EvidenceType::CanonicalRecord;
    recall.trust              = qiven::runtime::resolver::TrustDomain::Mechanism;

    qiven::runtime::resolver::ResolverBinding check;
    check.key.kind           = RequirementKind::RunMechanicalCheck;
    check.key.type           = qiven::runtime::resolver::ResolverType::MechanicalCheck;
    check.key.version        = 1;
    check.key.cognition_view = "default";
    check.resolver           = qiven::runtime::resolver::ResolverIdentity { "loopback", 1 };
    check.accepted_evidence  = qiven::runtime::resolver::EvidenceType::MechanicalCheckReceipt;
    check.trust              = qiven::runtime::resolver::TrustDomain::Mechanism;

    auto built = qiven::runtime::resolver::RequirementResolverRegistry::Builder()
                     .add(std::move(recall))
                     .add(std::move(check))
                     .build();
    QIVEN_VERIFY(built.is_ok());
    return std::move(built).value();
}

qiven::runtime::ResolverMechanism succeed_mechanism()
{
    return [](const qiven::runtime::resolver::ResolverBinding& binding,
              const qiven::runtime::RequirementIdentity& identity,
              const qiven::runtime::resolver::ReceiptContext& context) {
        return std::make_optional(qiven::runtime::resolver::make_evidence_receipt(
            identity, binding.resolver, binding.key.type, binding.accepted_evidence, identity.subject, "loopback-core",
            std::span<const std::byte>(), context));
    };
}

qiven::runtime::CorrelationKey key_numbered(u64 n)
{
    return qiven::runtime::CorrelationKey { qiven::runtime::RuntimeGenerationId { 1 },
                                            qiven::runtime::AdapterInstanceId { 1 },
                                            qiven::runtime::HarnessSessionId { 1 },
                                            qiven::runtime::ActorInstanceId { 1 },
                                            qiven::runtime::HarnessActionId { n } };
}

// §8.2 step 7: a valid activation receipt for the BeforeJudgment
// requirement, injected before the judgment opens.
qiven::runtime::port::ActivationReceipt activation_for(const qiven::runtime::port::PinnedCognition& cognition,
                                                       const qiven::runtime::CorrelationKey& key,
                                                       u64 injection_event)
{
    qiven::runtime::port::ActivationReceipt activation;
    activation.kind               = qiven::runtime::port::ActivationKind::SessionStart;
    activation.subject            = "policy context";
    activation.canonical_revision = cognition.revision;
    activation.injected_digest    = cognition.snapshot_digest_sha256;
    activation.evidence_type      = qiven::runtime::resolver::EvidenceType::CanonicalRecord;
    activation.generation         = key.generation;
    activation.actor_token        = key.actor.value;
    activation.session_token      = key.session.value;
    activation.injection_event    = injection_event;
    return activation;
}

IngressMessage proposal_for(const qiven::runtime::CorrelationKey& key, std::byte payload)
{
    IngressMessage message;
    message.kind        = IngressMessage::Kind::Proposal;
    message.correlation = key;
    const std::byte blob[] { payload };
    message.action = qiven::runtime::observe_action(key.adapter, key.session, key.actor,
                                                    qiven::runtime::CapabilityId { 1 }, "op", "target",
                                                    std::span<const std::byte>(blob, 1));
    return message;
}

bool settled(const ControlCore& core)
{
    const auto all = core.views();
    if (all.empty())
    {
        return false;
    }
    for (const auto& view : all)
    {
        if (view.phase != TransactionPhase::CognitiveAllowed && view.phase != TransactionPhase::Denied &&
            view.phase != TransactionPhase::ReDeliberationRequired)
        {
            return false;
        }
    }
    return true;
}

DrainResult drain_until(ControlCore& core, BoundedIngressQueue& ingress, bool (*done)(const ControlCore&))
{
    DrainResult total;
    for (int spin = 0; spin < 600 && !done(core); ++spin)
    {
        const DrainResult round = core.drain(ingress);
        total.processed += round.processed;
        total.dropped_unknown += round.dropped_unknown;
        total.integrity_failures += round.integrity_failures;
        total.replays += round.replays;
        total.receipt_rejections += round.receipt_rejections;
        if (round.processed == 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    QIVEN_VERIFY(done(core));
    return total;
}
} // namespace

int main()
{
    const auto cognition = pin_of(ruled_snapshot(), "qiven-mvp0-core-");
    const auto registry  = make_registry();

    // end to end WITH pre-satisfied BeforeJudgment cognition (§8.2 step 7):
    // the activation receipt pre-satisfies at proposal time; the remaining
    // BeforeExecution requirement resolves in flight; CognitiveAllowed
    {
        TransactionMinter minter;
        Executor pool { 2, 16 };
        BoundedIngressQueue ingress { 32 };
        ControlCore core { minter, pool, registry, succeed_mechanism(), cognition,
                           qiven::runtime::StructuralFacts {} };

        auto proposal = proposal_for(key_numbered(1), std::byte { 0x01 });
        proposal.activation_receipts.push_back(activation_for(cognition, proposal.correlation, 1));
        QIVEN_VERIFY(ingress.try_push(std::move(proposal)));
        drain_until(core, ingress, settled);

        const auto all = core.views();
        QIVEN_VERIFY(all.size() == 1);
        QIVEN_VERIFY(all[0].phase == TransactionPhase::CognitiveAllowed);
        QIVEN_VERIFY(all[0].disposition == qiven::runtime::Disposition::Allow);
        QIVEN_VERIFY(all[0].blocking_pending == 0);

        const auto* transaction = core.transaction(all[0].id);
        QIVEN_VERIFY(transaction != nullptr);
        QIVEN_VERIFY(transaction->packet.requirements.size() == 2);
        for (const auto& prepared : transaction->packet.requirements)
        {
            QIVEN_VERIFY(prepared.status == qiven::context::RequirementStatus::Satisfied);
            QIVEN_VERIFY(!prepared.evidence.empty());
        }
    }

    // §8.2 WITHOUT pre-injected cognition: the in-flight-resolved
    // BeforeJudgment requirement ends the transaction ReDeliberate —
    // the evidence authorizes the NEXT judgment, never this proposal
    {
        TransactionMinter minter;
        Executor pool { 2, 16 };
        BoundedIngressQueue ingress { 32 };
        ControlCore core { minter, pool, registry, succeed_mechanism(), cognition,
                           qiven::runtime::StructuralFacts {} };

        QIVEN_VERIFY(ingress.try_push(proposal_for(key_numbered(2), std::byte { 0x02 })));
        drain_until(core, ingress, settled);

        const auto all = core.views();
        QIVEN_VERIFY(all.size() == 1);
        QIVEN_VERIFY(all[0].phase == TransactionPhase::ReDeliberationRequired);
        QIVEN_VERIFY(all[0].disposition == qiven::runtime::Disposition::ReDeliberate);
        QIVEN_VERIFY(!core.resume_receipts(all[0].id).empty()); // §8.2 step 4 resume context
    }

    // §29 fail-closed: a resolver that cannot produce trusted evidence
    // denies the transaction (Blocked -> Failed, never Satisfied)
    {
        TransactionMinter minter;
        Executor pool { 2, 16 };
        BoundedIngressQueue ingress { 32 };
        ControlCore core {
            minter, pool, registry,
            [](const qiven::runtime::resolver::ResolverBinding&, const qiven::runtime::RequirementIdentity&,
               const qiven::runtime::resolver::ReceiptContext&)
                -> std::optional<qiven::runtime::resolver::EvidenceReceipt> { return std::nullopt; },
            cognition, qiven::runtime::StructuralFacts {}
        };

        QIVEN_VERIFY(ingress.try_push(proposal_for(key_numbered(3), std::byte { 0x03 })));
        drain_until(core, ingress, settled);
        const auto all = core.views();
        QIVEN_VERIFY(all.size() == 1);
        QIVEN_VERIFY(all[0].phase == TransactionPhase::Denied);
    }

    // §8.3 fail-closed: a receipt from the WRONG resolver type cannot
    // satisfy the accepted binding — the requirement fails closed
    {
        TransactionMinter minter;
        Executor pool { 2, 16 };
        BoundedIngressQueue ingress { 32 };
        ControlCore core {
            minter, pool, registry,
            [](const qiven::runtime::resolver::ResolverBinding&, const qiven::runtime::RequirementIdentity& identity,
               const qiven::runtime::resolver::ReceiptContext& context) {
                auto receipt = qiven::runtime::resolver::make_evidence_receipt(
                    identity, qiven::runtime::resolver::ResolverIdentity { "loopback", 1 },
                    qiven::runtime::resolver::ResolverType::TypedHumanHandoff, // wrong type for every binding
                    qiven::runtime::resolver::EvidenceType::TypedHumanHandoff, identity.subject, "loopback-core",
                    std::span<const std::byte>(), context);
                return std::make_optional(std::move(receipt));
            },
            cognition, qiven::runtime::StructuralFacts {}
        };

        QIVEN_VERIFY(ingress.try_push(proposal_for(key_numbered(4), std::byte { 0x04 })));
        const auto total = drain_until(core, ingress, settled);
        const auto all   = core.views();
        QIVEN_VERIFY(all.size() == 1);
        QIVEN_VERIFY(all[0].phase == TransactionPhase::Denied);
        QIVEN_VERIFY(total.receipt_rejections >= 1);
    }

    // §60 idempotent replay: the same correlation + identical content never
    // creates a second transaction; conflicting content never overwrites
    {
        TransactionMinter minter;
        Executor pool { 2, 16 };
        BoundedIngressQueue ingress { 32 };
        ControlCore core { minter, pool, registry, succeed_mechanism(), cognition,
                           qiven::runtime::StructuralFacts {} };

        const auto key = key_numbered(5);
        auto proposal  = proposal_for(key, std::byte { 0x05 });
        proposal.activation_receipts.push_back(activation_for(cognition, key, 5));
        QIVEN_VERIFY(ingress.try_push(std::move(proposal)));
        drain_until(core, ingress, settled);

        // identical replay after settlement
        QIVEN_VERIFY(ingress.try_push(proposal_for(key, std::byte { 0x05 })));
        auto result = core.drain(ingress);
        QIVEN_VERIFY(result.replays >= 1);
        QIVEN_VERIFY(core.transaction_count() == 1);

        // same key, different action bytes: integrity failure, original intact
        QIVEN_VERIFY(ingress.try_push(proposal_for(key, std::byte { 0xFF })));
        result = core.drain(ingress);
        QIVEN_VERIFY(result.integrity_failures >= 1);
        QIVEN_VERIFY(core.transaction_count() == 1);
        const auto all = core.views();
        QIVEN_VERIFY(all[0].phase == TransactionPhase::CognitiveAllowed); // untouched
    }

    // full-value correlation (§10.3): two keys differing ONLY in the actor
    // field are two distinct transactions — a 64-bit hash is not identity
    {
        TransactionMinter minter;
        Executor pool { 2, 16 };
        BoundedIngressQueue ingress { 32 };
        ControlCore core { minter, pool, registry, succeed_mechanism(), cognition,
                           qiven::runtime::StructuralFacts {} };

        const auto base = key_numbered(6);
        auto other      = base;
        other.actor     = qiven::runtime::ActorInstanceId { 2 };

        auto first = proposal_for(base, std::byte { 0x06 });
        first.activation_receipts.push_back(activation_for(cognition, base, 6));
        auto second = proposal_for(other, std::byte { 0x06 });
        second.activation_receipts.push_back(activation_for(cognition, other, 7));
        QIVEN_VERIFY(ingress.try_push(std::move(first)));
        QIVEN_VERIFY(ingress.try_push(std::move(second)));
        drain_until(core, ingress, settled);

        QIVEN_VERIFY(core.transaction_count() == 2);
    }

    // §61: evidence for an unknown transaction is dropped and counted,
    // never attached anywhere by guessing
    {
        TransactionMinter minter;
        Executor pool { 1, 4 };
        BoundedIngressQueue ingress { 8 };
        ControlCore core { minter, pool, registry, succeed_mechanism(), cognition,
                           qiven::runtime::StructuralFacts {} };

        IngressMessage ghost;
        ghost.kind                = IngressMessage::Kind::EvidenceDelivery;
        ghost.transaction         = qiven::runtime::ControlTransactionId { 999 };
        ghost.requirement.subject = "nowhere";
        QIVEN_VERIFY(ingress.try_push(std::move(ghost)));

        const auto result = core.drain(ingress);
        QIVEN_VERIFY(result.dropped_unknown >= 1);
        QIVEN_VERIFY(core.transaction_count() == 0);
    }

    // duplicate evidence delivery for a settled requirement is an
    // idempotent replay: no double-decrement, no state corruption
    {
        TransactionMinter minter;
        Executor pool { 2, 16 };
        BoundedIngressQueue ingress { 64 };
        ControlCore core { minter, pool, registry, succeed_mechanism(), cognition,
                           qiven::runtime::StructuralFacts {} };

        const auto key = key_numbered(7);
        auto proposal  = proposal_for(key, std::byte { 0x07 });
        proposal.activation_receipts.push_back(activation_for(cognition, key, 8));
        QIVEN_VERIFY(ingress.try_push(std::move(proposal)));
        drain_until(core, ingress, settled);
        const auto id = core.views()[0].id;

        IngressMessage duplicate;
        duplicate.kind                 = IngressMessage::Kind::EvidenceDelivery;
        duplicate.transaction          = id;
        duplicate.requirement.kind     = RequirementKind::RunMechanicalCheck;
        duplicate.requirement.subject  = "gate receipt";
        duplicate.requirement.boundary = RequirementBoundary::BeforeExecution;
        duplicate.requirement.blocking = true;
        duplicate.receipt              = qiven::runtime::resolver::make_evidence_receipt(
            duplicate.requirement, qiven::runtime::resolver::ResolverIdentity { "loopback", 1 },
            qiven::runtime::resolver::ResolverType::MechanicalCheck,
            qiven::runtime::resolver::EvidenceType::MechanicalCheckReceipt, "gate receipt", "loopback-core",
            std::span<const std::byte> {},
            qiven::runtime::resolver::ReceiptContext { qiven::runtime::RuntimeGenerationId { 1 },
                                                       qiven::runtime::ContentDigest { qiven::SHA256Digest {} },
                                                       qiven::context::RevisionId { "sha256:test" }, "loopback", 0 });
        QIVEN_VERIFY(ingress.try_push(std::move(duplicate)));

        const auto result = core.drain(ingress);
        QIVEN_VERIFY(result.replays >= 1);
        QIVEN_VERIFY(core.view(id).phase == TransactionPhase::CognitiveAllowed); // unchanged
    }

    // §30/§62 readiness gating: partial evidence NEVER allows early; the
    // second report releases the decision
    {
        TransactionMinter minter;
        Executor pool { 2, 16 };
        BoundedIngressQueue ingress { 32 };

        std::atomic<bool> release_second { false };

        qiven::runtime::ResolverMechanism resolver =
            [&](const qiven::runtime::resolver::ResolverBinding& binding,
                const qiven::runtime::RequirementIdentity& identity,
                const qiven::runtime::resolver::ReceiptContext& context) {
                if (identity.subject == "gate receipt")
                {
                    while (!release_second.load())
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    }
                }
                return succeed_mechanism()(binding, identity, context);
            };
        ControlCore core { minter, pool, registry, std::move(resolver), cognition,
                           qiven::runtime::StructuralFacts {} };

        const auto key = key_numbered(8);
        auto proposal  = proposal_for(key, std::byte { 0x08 });
        proposal.activation_receipts.push_back(activation_for(cognition, key, 9));
        QIVEN_VERIFY(ingress.try_push(std::move(proposal)));

        // wait until the in-flight BeforeExecution report is still pending
        bool saw_partial = false;
        for (int spin = 0; spin < 600; ++spin)
        {
            core.drain(ingress);
            const auto all = core.views();
            if (!all.empty() && all[0].blocking_pending == 1)
            {
                saw_partial = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        QIVEN_VERIFY(saw_partial);
        QIVEN_VERIFY(core.views()[0].phase != TransactionPhase::CognitiveAllowed); // gated

        release_second.store(true);
        drain_until(core, ingress, settled);
        QIVEN_VERIFY(core.views()[0].phase == TransactionPhase::CognitiveAllowed);
    }

    // determinism: identical arrival order into two fresh cores produces
    // the same transaction ids, phases and dispositions
    {
        auto run = [&cognition, &registry] {
            TransactionMinter minter;
            Executor pool { 2, 16 };
            BoundedIngressQueue ingress { 32 };
            ControlCore core { minter, pool, registry, succeed_mechanism(), cognition,
                               qiven::runtime::StructuralFacts {} };
            auto first = proposal_for(key_numbered(9), std::byte { 0x09 });
            first.activation_receipts.push_back(activation_for(cognition, first.correlation, 10));
            auto second = proposal_for(key_numbered(10), std::byte { 0x0A });
            second.activation_receipts.push_back(activation_for(cognition, second.correlation, 11));
            QIVEN_VERIFY(ingress.try_push(std::move(first)));
            QIVEN_VERIFY(ingress.try_push(std::move(second)));
            drain_until(core, ingress, settled);
            std::vector<std::pair<u64, int>> outcome;
            for (const auto& view : core.views())
            {
                outcome.emplace_back(view.id.value, static_cast<int>(view.phase));
            }
            std::sort(outcome.begin(), outcome.end());
            return outcome;
        };
        const auto first  = run();
        const auto second = run();
        QIVEN_VERIFY(first.size() == 2 && second.size() == 2);
        QIVEN_VERIFY(first == second);
    }

    // concurrent duplicate proposals racing through the queue: exactly one
    // transaction exists, the loser replays
    {
        TransactionMinter minter;
        Executor pool { 2, 32 };
        BoundedIngressQueue ingress { 64 };
        ControlCore core { minter, pool, registry, succeed_mechanism(), cognition,
                           qiven::runtime::StructuralFacts {} };

        const auto key = key_numbered(11);
        std::vector<std::thread> racers;
        for (int i = 0; i < 4; ++i)
        {
            racers.emplace_back([&ingress, &key, &cognition] {
                auto proposal = proposal_for(key, std::byte { 0x0B });
                proposal.activation_receipts.push_back(activation_for(cognition, key, 12));
                while (!ingress.try_push(std::move(proposal)))
                {
                    std::this_thread::sleep_for(std::chrono::microseconds(50));
                }
            });
        }
        for (std::thread& racer : racers)
        {
            racer.join();
        }

        const auto result = drain_until(core, ingress, settled);
        QIVEN_VERIFY(core.transaction_count() == 1);
        QIVEN_VERIFY(result.replays >= 3);
        QIVEN_VERIFY(core.views()[0].phase == TransactionPhase::CognitiveAllowed);
    }

    // stress: 50 distinct proposals through the full pipeline
    {
        TransactionMinter minter;
        Executor pool { 4, 64 };
        BoundedIngressQueue ingress { 128 };
        ControlCore core { minter, pool, registry, succeed_mechanism(), cognition,
                           qiven::runtime::StructuralFacts {} };

        for (u64 n = 100; n < 150; ++n)
        {
            const auto key = key_numbered(n);
            auto proposal  = proposal_for(key, std::byte { static_cast<unsigned char>(n) });
            proposal.activation_receipts.push_back(activation_for(cognition, key, 1000 + n));
            QIVEN_VERIFY(ingress.try_push(std::move(proposal)));
        }
        drain_until(core, ingress, settled);
        QIVEN_VERIFY(core.transaction_count() == 50);
        for (const auto& view : core.views())
        {
            QIVEN_VERIFY(view.phase == TransactionPhase::CognitiveAllowed);
        }
    }

    std::printf("[ OK ] control-core\n");
    return 0;
}
