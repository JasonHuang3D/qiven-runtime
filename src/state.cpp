#include <qiven/runtime/state.hpp>

#include <qiven/contracts.hpp>

#include <chrono>
#include <thread>

namespace qiven::runtime
{
namespace
{
u64 clock_ms() noexcept
{
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}
} // namespace

ControlCore::ControlCore(TransactionMinter& minter,
                         Executor& executor,
                         const resolver::RequirementResolverRegistry& registry,
                         ResolverMechanism mechanism,
                         port::PinnedCognition cognition,
                         StructuralFacts facts) :
m_minter(minter),
m_executor(executor),
m_registry(registry),
m_mechanism(std::move(mechanism)),
m_cognition(std::move(cognition)),
m_facts(facts)
{
}

const ControlTransaction* ControlCore::transaction(const ControlTransactionId& id) const
{
    const auto found = m_transactions.find(id.value);
    return found == m_transactions.end() ? nullptr : &found->second;
}

std::vector<TransactionView> ControlCore::views() const
{
    std::vector<TransactionView> out;
    out.reserve(m_transactions.size());
    for (const auto& [value, transaction] : m_transactions)
    {
        static_cast<void>(value);
        TransactionView v = view(transaction.id);
        out.push_back(v);
    }
    return out;
}

TransactionView ControlCore::view(const ControlTransactionId& id) const
{
    TransactionView out;
    const ControlTransaction* transaction = this->transaction(id);
    if (transaction == nullptr)
    {
        return out;
    }
    out.exists        = true;
    out.id            = transaction->id;
    out.causal_parent = transaction->causal_parent;
    out.phase         = transaction->phase;
    out.disposition   = disposition_for(*transaction);
    for (const qiven::context::PreparedRequirement& prepared : transaction->packet.requirements)
    {
        if (prepared.requirement.blocking && prepared.status == qiven::context::RequirementStatus::Pending)
        {
            ++out.blocking_pending;
        }
    }
    return out;
}

const std::vector<resolver::EvidenceReceipt>& ControlCore::resume_receipts(const ControlTransactionId& id) const
{
    static const std::vector<resolver::EvidenceReceipt> empty;
    const auto found = m_resume.find(id.value);
    return found == m_resume.end() ? empty : found->second;
}

DrainResult ControlCore::drain(BoundedIngressQueue& ingress)
{
    DrainResult result;
    while (true)
    {
        std::optional<IngressMessage> message = ingress.pop_wait(std::chrono::milliseconds(20));
        if (!message.has_value())
        {
            break; // momentary quiesce; concurrent resolvers may still post
        }
        switch (message->kind)
        {
        case IngressMessage::Kind::Proposal:
            handle_proposal(ingress, *message, result);
            break;
        case IngressMessage::Kind::EvidenceDelivery:
            handle_evidence(*message, result);
            break;
        case IngressMessage::Kind::ResolutionFailure:
            handle_failure(*message, result);
            break;
        }
        ++result.processed;
    }
    return result;
}

const resolver::ResolverBinding* ControlCore::binding_for(const RequirementIdentity& identity) const
{
    // Selection by the full four-tuple's (kind, view) resolution: exactly
    // one accepted binding may exist for a dispatchable requirement in
    // this generation; zero or many are both fail-closed (§8.3).
    const auto found = m_registry.find_for_kind_and_view(identity.kind, "default");
    if (found.size() != 1)
    {
        return nullptr;
    }
    return found.front();
}

void ControlCore::handle_proposal(BoundedIngressQueue& ingress, const IngressMessage& message, DrainResult& result)
{
    const CorrelationKey key = message.correlation;

    const auto existing = m_by_correlation.find(key);
    if (existing != m_by_correlation.end())
    {
        const ControlTransaction* original = transaction(existing->second);
        if (original == nullptr)
        {
            // index/bookkeeping divergence is a defect; fail loudly rather
            // than guess (§61)
            ++result.integrity_failures;
            return;
        }
        if (action_digest_of(original->action) == action_digest_of(message.action))
        {
            ++result.replays; // §60 idempotent replay, no second transaction
        }
        else
        {
            ++result.integrity_failures; // §60 same key, different content
            ++m_integrity_failures[key];
        }
        return;
    }

    QIVEN_ASSERT(m_cognition.snapshot != nullptr); // a core without a pinned
    // snapshot is a construction defect, not a runtime condition

    // classify (day-one structural facts from construction; the loopback
    // adapter supplies real facts with RCA-14) and derive with the frozen
    // semantics against the pinned snapshot
    IntentSet intents = classify(message.action, m_facts);
    RequirementSet derived =
        derive_requirement_set(*m_cognition.snapshot, intents);

    qiven::context::PreparationPacket packet;
    const std::span<const RequirementInstance> instances = derived.instances();
    for (const RequirementInstance& instance : instances)
    {
        qiven::context::PreparedRequirement prepared;
        prepared.requirement.kind     = instance.identity.kind;
        prepared.requirement.subject  = instance.identity.subject;
        prepared.requirement.blocking = instance.identity.blocking;
        prepared.boundary             = instance.identity.boundary;
        prepared.status               = qiven::context::RequirementStatus::Pending;
        packet.requirements.push_back(prepared);
    }

    // §8.2 step 7 — pre-satisfaction happens HERE, at proposal time,
    // before the judgment opens: an unexpired activation receipt whose
    // type, source, digest and generation match the new judgment may
    // pre-satisfy its BeforeJudgment requirements. Single-use: a receipt
    // pre-satisfies exactly one requirement of one judgment.
    const u64 now_ms = clock_ms();
    for (qiven::context::PreparedRequirement& prepared : packet.requirements)
    {
        if (!prepared.requirement.blocking ||
            prepared.boundary != qiven::context::RequirementBoundary::BeforeJudgment ||
            prepared.status != qiven::context::RequirementStatus::Pending)
        {
            continue;
        }
        RequirementIdentity identity;
        identity.kind     = prepared.requirement.kind;
        identity.subject  = prepared.requirement.subject;
        identity.boundary = prepared.boundary;
        identity.blocking = prepared.requirement.blocking;
        for (const port::ActivationReceipt& activation : message.activation_receipts)
        {
            const u64 use = port::activation_use_identity(activation);
            if (m_spent_activations.contains(use))
            {
                continue; // single-use: already consumed by an earlier judgment
            }
            if (!activation.valid_for(identity, m_cognition.revision, m_cognition.snapshot_digest_sha256,
                                      message.correlation.generation, m_cognition.policy_digest, now_ms))
            {
                continue;
            }
            prepared.status = qiven::context::RequirementStatus::Satisfied;
            prepared.evidence.push_back("activation:" + std::to_string(activation.injection_event));
            m_spent_activations.insert(use);
            break;
        }
    }

    ControlTransaction transaction =
        begin_control_transaction(m_minter.next(), message.causal_parent, message.correlation, message.action,
                                  std::move(intents), m_cognition, std::move(packet));

    const u64 id_value = transaction.id.value;
    m_transactions.emplace(id_value, std::move(transaction));
    m_by_correlation.emplace(key, ControlTransactionId { id_value });

    ControlTransaction& stored = m_transactions.at(id_value);
    dispatch_resolvers(ingress, stored);
    if (m_outstanding[id_value] == 0)
    {
        reevaluate_after_reports(stored);
    }
}

void ControlCore::dispatch_resolvers(BoundedIngressQueue& ingress, ControlTransaction& transaction)
{
    u64 outstanding = 0;
    for (qiven::context::PreparedRequirement& prepared : transaction.packet.requirements)
    {
        if (!prepared.requirement.blocking || prepared.status != qiven::context::RequirementStatus::Pending)
        {
            continue;
        }
        RequirementIdentity identity;
        identity.kind     = prepared.requirement.kind;
        identity.subject  = prepared.requirement.subject;
        identity.boundary = prepared.boundary;
        identity.blocking = prepared.requirement.blocking;

        // §8.3 selection: only the registry's accepted binding may
        // dispatch; no binding (or an ambiguous set) fails the
        // requirement closed NOW — it must not linger Pending forever
        const resolver::ResolverBinding* binding = binding_for(identity);
        if (binding == nullptr)
        {
            prepared.status = qiven::context::RequirementStatus::Failed;
            continue;
        }

        const resolver::ResolverBinding bound            = *binding;
        const ControlTransactionId id                    = transaction.id;
        const RuntimeGenerationId generation             = transaction.correlation.generation;
        const ContentDigest policy_digest                = m_cognition.policy_digest;
        const qiven::context::RevisionId source_revision = m_cognition.revision;
        ResolverMechanism mechanism                      = m_mechanism;
        BoundedIngressQueue* ingress_ptr                 = &ingress;

        Executor::Task task = [id, identity, bound, generation, policy_digest, source_revision, mechanism,
                               ingress_ptr] {
            // resolver thread: touches ONLY the injected mechanism and the
            // thread-safe ingress queue — never control state (design §9)
            const resolver::ReceiptContext context { generation,
                                                     policy_digest,
                                                     source_revision,
                                                     "loopback",
                                                     clock_ms() };
            std::optional<qiven::runtime::resolver::EvidenceReceipt> receipt;
            try
            {
                receipt = mechanism ? mechanism(bound, identity, context) : std::nullopt;
            }
            catch (...)
            {
                receipt = std::nullopt; // §29: an exploding resolver is an
                                        // unresolved requirement, never Satisfied
            }
            IngressMessage report;
            report.transaction = id;
            report.requirement = identity;
            if (receipt.has_value())
            {
                report.kind    = IngressMessage::Kind::EvidenceDelivery;
                report.receipt = std::move(*receipt);
            }
            else
            {
                report.kind = IngressMessage::Kind::ResolutionFailure;
            }
            // bounded queue full at report time: retry-free drop would lose
            // a requirement report; spin-retry keeps the invariant that
            // every dispatched resolver reports exactly once, while the
            // queue's capacity still bounds memory
            while (!ingress_ptr->try_push(std::move(report)))
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        };
        if (m_executor.submit(std::move(task)))
        {
            ++outstanding;
        }
        else
        {
            // executor saturated: the requirement fails closed NOW (§29) —
            // it must not linger Pending forever
            prepared.status = qiven::context::RequirementStatus::Failed;
        }
    }
    m_outstanding[transaction.id.value] = outstanding;
}

void ControlCore::handle_evidence(const IngressMessage& message, DrainResult& result)
{
    const auto found = m_transactions.find(message.transaction.value);
    if (found == m_transactions.end())
    {
        ++result.dropped_unknown; // §61: never guessed into place
        return;
    }

    ControlTransaction& transaction = found->second;
    bool matched                    = false;
    for (qiven::context::PreparedRequirement& prepared : transaction.packet.requirements)
    {
        RequirementIdentity identity;
        identity.kind     = prepared.requirement.kind;
        identity.subject  = prepared.requirement.subject;
        identity.boundary = prepared.boundary;
        identity.blocking = prepared.requirement.blocking;
        if (!(identity == message.requirement))
        {
            continue;
        }
        matched = true;
        if (prepared.status != qiven::context::RequirementStatus::Pending)
        {
            // duplicate delivery for an already-reported requirement: an
            // idempotent replay, never a second outstanding decrement
            ++result.replays;
            return;
        }

        // §8.3 acceptance: a receipt satisfies a requirement ONLY when the
        // registry validates it — requirement identity, resolver type and
        // version, evidence type, source, digest, generation and policy
        // under the live facts. Any mismatch fails the requirement closed
        // and is counted; it is never guessed into place.
        const resolver::ResolverBinding* binding = binding_for(identity);
        const resolver::ReceiptValidation validation =
            binding != nullptr
                ? resolver::validate_receipt(message.receipt, *binding, identity, transaction.correlation.generation,
                                             m_cognition.policy_digest, clock_ms())
                : resolver::ReceiptValidation { resolver::ReceiptRejection::WrongRequirement };
        if (validation.rejection == resolver::ReceiptRejection::None)
        {
            prepared.status = qiven::context::RequirementStatus::Satisfied;
            prepared.evidence.push_back("receipt:" + std::to_string(message.receipt.id.fnv));

            // §8.2 timing: in-flight satisfaction of a blocking
            // BeforeJudgment requirement marks this transaction for
            // ReDeliberate — it can never authorize this proposal
            if (prepared.requirement.blocking &&
                prepared.boundary == qiven::context::RequirementBoundary::BeforeJudgment)
            {
                m_redeliberation_pending.insert(transaction.id.value);
                m_resume[transaction.id.value].push_back(message.receipt);
            }
        }
        else
        {
            prepared.status = qiven::context::RequirementStatus::Failed;
            ++result.receipt_rejections;
            ++result.integrity_failures;
        }
        break;
    }
    if (!matched)
    {
        ++result.dropped_unknown; // §61: evidence for a requirement this
                                  // transaction never derived
        return;
    }

    const auto outstanding = m_outstanding.find(transaction.id.value);
    if (outstanding != m_outstanding.end() && outstanding->second > 0)
    {
        if (--outstanding->second == 0)
        {
            reevaluate_after_reports(transaction);
        }
    }
}

void ControlCore::handle_failure(const IngressMessage& message, DrainResult& result)
{
    const auto found = m_transactions.find(message.transaction.value);
    if (found == m_transactions.end())
    {
        ++result.dropped_unknown;
        return;
    }

    ControlTransaction& transaction = found->second;
    for (qiven::context::PreparedRequirement& prepared : transaction.packet.requirements)
    {
        RequirementIdentity identity;
        identity.kind     = prepared.requirement.kind;
        identity.subject  = prepared.requirement.subject;
        identity.boundary = prepared.boundary;
        identity.blocking = prepared.requirement.blocking;
        if (identity == message.requirement)
        {
            if (prepared.status != qiven::context::RequirementStatus::Pending)
            {
                ++result.replays; // duplicate failure report: idempotent
                return;
            }
            prepared.status = qiven::context::RequirementStatus::Failed; // §29 fail-closed
            break;
        }
    }

    const auto outstanding = m_outstanding.find(transaction.id.value);
    if (outstanding != m_outstanding.end() && outstanding->second > 0)
    {
        if (--outstanding->second == 0)
        {
            reevaluate_after_reports(transaction);
        }
    }
}

void ControlCore::reevaluate_after_reports(ControlTransaction& transaction)
{
    if (transaction.terminal())
    {
        return; // no post-hoc repair of a settled path (§18)
    }

    // §8.2: an in-flight-resolved BeforeJudgment requirement ends THIS
    // transaction as ReDeliberate even when every requirement reports
    // satisfied — the evidence authorizes the NEXT judgment, never the
    // one that discovered it. A typed preparation failure still denies
    // first (S0-02).
    if (m_redeliberation_pending.contains(transaction.id.value) &&
        transaction.packet.failure == qiven::context::PreparationFailure::None)
    {
        transaction.phase = TransactionPhase::ReDeliberationRequired;
        return;
    }

    evaluate_before_judgment(transaction);
    if (transaction.phase == TransactionPhase::PreparedForJudgment)
    {
        evaluate_before_execution(transaction);
    }
}
} // namespace qiven::runtime
