// ============================================================================
// redeliberate_timing.cpp — MVP-0 exit gate: the BeforeJudgment timing
// correction (production-MVP architecture §8.2, ADR-0047)
//
// The four exit-gate properties proved here:
//   1. A BeforeJudgment requirement discovered and resolved INSIDE the
//      current proposal can NEVER authorize that proposal: the
//      transaction ends ReDeliberationRequired with resume context.
//   2. Only the NEXT proposal — a NEW transaction carrying causal_parent
//      and a VALID, unexpired, unspent activation receipt whose type,
//      source, digest and generation match the new judgment — may be
//      authorized, with the requirement pre-satisfied at proposal time.
//   3. Expired, wrong-generation, wrong-digest, wrong-subject and
//      already-spent receipts pre-satisfy NOTHING: the requirement
//      re-enters resolution and the new transaction also ends
//      ReDeliberate (fail-closed, never guessed into place).
//   4. No path treats equality of a 64-bit hash as identity equality:
//      receipts with equal fnv ids but different bindings are unequal,
//      and correlation is keyed by the full five-tuple value.
// ============================================================================

#include <qiven/runtime/port/activation.hpp>
#include <qiven/runtime/resolver.hpp>
#include <qiven/runtime/state.hpp>

#include <qiven/context/persistence.hpp>
#include <qiven/contracts.hpp>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>

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
    const TempFile file("qiven-mvp0-redelib-" + std::to_string(counter.fetch_add(1)) + ".bin");
    file.write(qiven::context::serialize_snapshot(snapshot));
    qiven::runtime::port::DraftSnapshotReader reader;
    auto pinned = reader.pin(qiven::runtime::port::PinRequest { file.path(), "" });
    QIVEN_VERIFY(pinned.is_ok());
    return std::move(pinned).value();
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

    auto built =
        qiven::runtime::resolver::RequirementResolverRegistry::Builder().add(std::move(recall)).build();
    QIVEN_VERIFY(built.is_ok());
    return std::move(built).value();
}

qiven::runtime::ResolverMechanism succeed_mechanism()
{
    return [](const qiven::runtime::resolver::ResolverBinding& binding,
              const qiven::runtime::RequirementIdentity& identity,
              const qiven::runtime::resolver::ReceiptContext& context) {
        return std::make_optional(qiven::runtime::resolver::make_evidence_receipt(
            identity, binding.resolver, binding.key.type, binding.accepted_evidence, identity.subject,
            "redelib-proof", std::span<const std::byte>(), context));
    };
}

qiven::runtime::port::ActivationReceipt valid_activation(const qiven::runtime::port::PinnedCognition& cognition,
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
    activation.expires_at_ms      = 0; // no expiry: the core's clock governs nothing else here
    return activation;
}

// Run one proposal to a terminal phase; returns the view of the NEWEST
// transaction (re-deliberation chains keep prior terminal transactions in
// the core, so "the first view" is not "this proposal's view").
qiven::runtime::TransactionView run_one(ControlCore& core, BoundedIngressQueue& ingress, IngressMessage proposal,
                                        qiven::usize expected_transactions)
{
    QIVEN_VERIFY(ingress.try_push(std::move(proposal)));
    for (int spin = 0; spin < 600; ++spin)
    {
        core.drain(ingress);
        const auto all = core.views();
        if (all.size() == expected_transactions)
        {
            bool all_terminal                             = true;
            const qiven::runtime::TransactionView* newest = &all[0];
            for (const auto& view : all)
            {
                const bool terminal = view.phase == TransactionPhase::CognitiveAllowed ||
                                      view.phase == TransactionPhase::Denied ||
                                      view.phase == TransactionPhase::ReDeliberationRequired;
                all_terminal = all_terminal && terminal;
                if (view.id.value > newest->id.value)
                {
                    newest = &view;
                }
            }
            if (all_terminal)
            {
                return *newest;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    QIVEN_VERIFY(false && "pipeline did not settle");
    return {};
}

IngressMessage proposal_numbered(const qiven::runtime::port::PinnedCognition& cognition, u64 action, std::byte payload)
{
    IngressMessage message;
    message.kind        = IngressMessage::Kind::Proposal;
    message.correlation = qiven::runtime::CorrelationKey { qiven::runtime::RuntimeGenerationId { 1 },
                                                           qiven::runtime::AdapterInstanceId { 1 },
                                                           qiven::runtime::HarnessSessionId { 1 },
                                                           qiven::runtime::ActorInstanceId { 1 },
                                                           qiven::runtime::HarnessActionId { action } };
    const std::byte blob[] { payload };
    message.action = qiven::runtime::observe_action(message.correlation.adapter, message.correlation.session,
                                                    message.correlation.actor, qiven::runtime::CapabilityId { 1 },
                                                    "op", "target", std::span<const std::byte>(blob, 1));
    static_cast<void>(cognition);
    return message;
}
} // namespace

int main()
{
    const auto cognition = pin_ruled();
    const auto registry  = make_registry();

    // EXIT GATE 1: a late-discovered, in-flight-resolved BeforeJudgment
    // requirement NEVER authorizes the current proposal
    {
        TransactionMinter minter;
        Executor pool { 2, 8 };
        BoundedIngressQueue ingress { 16 };
        ControlCore core { minter, pool, registry, succeed_mechanism(), cognition,
                           qiven::runtime::StructuralFacts {} };

        const auto view = run_one(core, ingress, proposal_numbered(cognition, 1, std::byte { 0x01 }), 1);
        QIVEN_VERIFY(view.phase == TransactionPhase::ReDeliberationRequired);
        QIVEN_VERIFY(view.disposition == qiven::runtime::Disposition::ReDeliberate);
        QIVEN_VERIFY(view.phase != TransactionPhase::CognitiveAllowed);

        // §8.2 step 4: the resolution is retained as resume context for
        // the NEXT judgment
        const auto& resume = core.resume_receipts(view.id);
        QIVEN_VERIFY(resume.size() == 1);
        QIVEN_VERIFY(resume[0].requirement.subject == "policy context");
        QIVEN_VERIFY(!resume[0].source_revision.value.empty());
    }

    // EXIT GATE 2 (positive): the NEW transaction with causal_parent and a
    // valid pre-injected activation receipt IS authorized — the receipt
    // pre-satisfies the BeforeJudgment requirement at proposal time
    {
        TransactionMinter minter;
        Executor pool { 2, 8 };
        BoundedIngressQueue ingress { 16 };
        ControlCore core { minter, pool, registry, succeed_mechanism(), cognition,
                           qiven::runtime::StructuralFacts {} };

        const auto first = run_one(core, ingress, proposal_numbered(cognition, 2, std::byte { 0x02 }), 1);
        QIVEN_VERIFY(first.phase == TransactionPhase::ReDeliberationRequired);

        auto second          = proposal_numbered(cognition, 3, std::byte { 0x02 });
        second.causal_parent = first.id;
        second.activation_receipts.push_back(valid_activation(cognition, second.correlation, 1));
        const auto retried = run_one(core, ingress, std::move(second), 2);

        QIVEN_VERIFY(retried.phase == TransactionPhase::CognitiveAllowed);
        QIVEN_VERIFY(retried.causal_parent.has_value() && retried.causal_parent->value == first.id.value);
        QIVEN_VERIFY(core.transaction_count() == 2); // a CHAIN of exact transactions, never one mutated one
    }

    // EXIT GATE 3 (negative): expired / wrong-generation / wrong-digest /
    // wrong-subject / spent receipts pre-satisfy NOTHING
    {
        // expired: the receipt's window closed before the new judgment
        {
            TransactionMinter minter;
            Executor pool { 2, 8 };
            BoundedIngressQueue ingress { 16 };
            ControlCore core { minter, pool, registry, succeed_mechanism(), cognition,
                               qiven::runtime::StructuralFacts {} };

            auto expired          = proposal_numbered(cognition, 4, std::byte { 0x03 });
            auto receipt          = valid_activation(cognition, expired.correlation, 2);
            receipt.expires_at_ms = 1; // long past (epoch ms)
            expired.activation_receipts.push_back(receipt);
            const auto view = run_one(core, ingress, std::move(expired), 1);
            QIVEN_VERIFY(view.phase == TransactionPhase::ReDeliberationRequired);
        }

        // wrong generation: the receipt belongs to another generation
        {
            TransactionMinter minter;
            Executor pool { 2, 8 };
            BoundedIngressQueue ingress { 16 };
            ControlCore core { minter, pool, registry, succeed_mechanism(), cognition,
                               qiven::runtime::StructuralFacts {} };

            auto cross         = proposal_numbered(cognition, 5, std::byte { 0x03 });
            auto receipt       = valid_activation(cognition, cross.correlation, 3);
            receipt.generation = qiven::runtime::RuntimeGenerationId { 99 };
            cross.activation_receipts.push_back(receipt);
            const auto view = run_one(core, ingress, std::move(cross), 1);
            QIVEN_VERIFY(view.phase == TransactionPhase::ReDeliberationRequired);
        }

        // wrong digest: the injected content is not the pinned cognition
        {
            TransactionMinter minter;
            Executor pool { 2, 8 };
            BoundedIngressQueue ingress { 16 };
            ControlCore core { minter, pool, registry, succeed_mechanism(), cognition,
                               qiven::runtime::StructuralFacts {} };

            auto poisoned           = proposal_numbered(cognition, 6, std::byte { 0x03 });
            auto receipt            = valid_activation(cognition, poisoned.correlation, 4);
            receipt.injected_digest = qiven::runtime::ContentDigest { qiven::SHA256Digest { std::byte { 0xFF } } };
            poisoned.activation_receipts.push_back(receipt);
            const auto view = run_one(core, ingress, std::move(poisoned), 1);
            QIVEN_VERIFY(view.phase == TransactionPhase::ReDeliberationRequired);
        }

        // wrong subject: the receipt proves OTHER cognition, not this
        // requirement's
        {
            TransactionMinter minter;
            Executor pool { 2, 8 };
            BoundedIngressQueue ingress { 16 };
            ControlCore core { minter, pool, registry, succeed_mechanism(), cognition,
                               qiven::runtime::StructuralFacts {} };

            auto alien      = proposal_numbered(cognition, 7, std::byte { 0x03 });
            auto receipt    = valid_activation(cognition, alien.correlation, 5);
            receipt.subject = "some other subject";
            alien.activation_receipts.push_back(receipt);
            const auto view = run_one(core, ingress, std::move(alien), 1);
            QIVEN_VERIFY(view.phase == TransactionPhase::ReDeliberationRequired);
        }

        // spent: single-use — the same authorization opportunity never
        // pre-satisfies a second judgment
        {
            TransactionMinter minter;
            Executor pool { 2, 8 };
            BoundedIngressQueue ingress { 16 };
            ControlCore core { minter, pool, registry, succeed_mechanism(), cognition,
                               qiven::runtime::StructuralFacts {} };

            auto first = proposal_numbered(cognition, 8, std::byte { 0x04 });
            first.activation_receipts.push_back(valid_activation(cognition, first.correlation, 6));
            const auto allowed = run_one(core, ingress, std::move(first), 1);
            QIVEN_VERIFY(allowed.phase == TransactionPhase::CognitiveAllowed);

            auto second = proposal_numbered(cognition, 9, std::byte { 0x04 });
            // SAME receipt (same generation, injection event and subject):
            // the authorization opportunity was spent by the first judgment
            second.activation_receipts.push_back(valid_activation(cognition, second.correlation, 6));
            const auto refused = run_one(core, ingress, std::move(second), 2);
            QIVEN_VERIFY(refused.phase == TransactionPhase::ReDeliberationRequired);
        }
    }

    // EXIT GATE 4: no path treats 64-bit-hash equality as identity equality
    {
        // receipts: equal fnv id, different binding => NOT equal receipts
        const qiven::runtime::resolver::ReceiptContext context {
            qiven::runtime::RuntimeGenerationId { 1 },
            qiven::runtime::ContentDigest { qiven::SHA256Digest {} },
            qiven::context::RevisionId { "sha256:test" },
            "test-build",
            1'000'000
        };
        const qiven::runtime::RequirementIdentity identity { RequirementKind::MandatoryRecall, "policy context",
                                                             RequirementBoundary::BeforeJudgment, true };
        const auto left = qiven::runtime::resolver::make_evidence_receipt(
            identity, qiven::runtime::resolver::ResolverIdentity { "loopback", 1 },
            qiven::runtime::resolver::ResolverType::CanonicalRecall,
            qiven::runtime::resolver::EvidenceType::CanonicalRecord, "policy context", "redelib-proof",
            std::span<const std::byte>(), context);

        auto forged    = left;            // same id.fnv ...
        forged.subject = "other subject"; // ... but a different binding
        QIVEN_VERIFY(forged.id.fnv == left.id.fnv);
        QIVEN_VERIFY(!(forged == left)); // identity is the FULL binding, not the hash

        // correlation: keys differing in one field are distinct values even
        // though the fnv hash is only 64 bits (full-value keying)
        qiven::runtime::CorrelationKey a { qiven::runtime::RuntimeGenerationId { 1 },
                                           qiven::runtime::AdapterInstanceId { 1 },
                                           qiven::runtime::HarnessSessionId { 1 },
                                           qiven::runtime::ActorInstanceId { 1 },
                                           qiven::runtime::HarnessActionId { 1 } };
        qiven::runtime::CorrelationKey b = a;
        b.actor                          = qiven::runtime::ActorInstanceId { 987654321 };
        QIVEN_VERIFY(a != b);

        // activation ledger: single-use bookkeeping keys the (generation,
        // event, subject) tuple, not a bare hash equality
        qiven::runtime::port::ActivationLedger ledger;
        const u64 event = ledger.notify_activation(qiven::runtime::port::ActivationEvent {});
        auto minted     = ledger.mint_receipt(valid_activation(cognition, a, event));
        QIVEN_VERIFY(minted.has_value());
        QIVEN_VERIFY(ledger.try_spend(*minted));
        QIVEN_VERIFY(!ledger.try_spend(*minted)); // second spend refused
        QIVEN_VERIFY(ledger.is_spent(*minted));
        auto unknown = ledger.mint_receipt(valid_activation(cognition, a, 424242));
        QIVEN_VERIFY(!unknown.has_value()); // unrecorded events fail closed
    }

    std::printf("[ OK ] redeliberate-timing\n");
    return 0;
}
