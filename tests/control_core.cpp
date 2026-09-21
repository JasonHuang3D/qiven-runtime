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

qiven::runtime::CorrelationKey key_numbered(u64 n)
{
    return qiven::runtime::CorrelationKey { qiven::runtime::RuntimeGenerationId { 1 },
                                            qiven::runtime::AdapterInstanceId { 1 },
                                            qiven::runtime::HarnessSessionId { 1 },
                                            qiven::runtime::ActorInstanceId { 1 },
                                            qiven::runtime::HarnessActionId { n } };
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

qiven::runtime::ResolverFn succeed_resolver()
{
    return [](const qiven::runtime::RequirementIdentity& identity) {
        return std::make_optional(qiven::runtime::resolver::make_evidence_receipt(
            identity, qiven::runtime::resolver::ResolverIdentity { "loopback", 1 }, identity.subject, "loopback-core",
            std::span<const std::byte>()));
    };
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
        if (view.phase != TransactionPhase::CognitiveAllowed && view.phase != TransactionPhase::Denied)
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
    const auto cognition = pin_of(ruled_snapshot(), "qiven-rca8-core-");

    // end to end: both blocking requirements resolve concurrently and the
    // transaction reaches CognitiveAllowed with recorded evidence
    {
        TransactionMinter minter;
        Executor pool { 2, 16 };
        BoundedIngressQueue ingress { 32 };
        ControlCore core { minter, pool, succeed_resolver(), cognition, qiven::runtime::StructuralFacts {} };

        QIVEN_VERIFY(ingress.try_push(proposal_for(key_numbered(1), std::byte { 0x01 })));
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

    // §29 fail-closed: a resolver that cannot produce trusted evidence
    // denies the transaction (Blocked -> Failed, never Satisfied)
    {
        TransactionMinter minter;
        Executor pool { 2, 16 };
        BoundedIngressQueue ingress { 32 };
        ControlCore core { minter, pool,
                           [](const qiven::runtime::RequirementIdentity&) { return std::optional<qiven::runtime::resolver::EvidenceReceipt> {}; },
                           cognition, qiven::runtime::StructuralFacts {} };

        QIVEN_VERIFY(ingress.try_push(proposal_for(key_numbered(2), std::byte { 0x02 })));
        drain_until(core, ingress, settled);
        const auto all = core.views();
        QIVEN_VERIFY(all.size() == 1);
        QIVEN_VERIFY(all[0].phase == TransactionPhase::Denied);
    }

    // §60 idempotent replay: the same correlation + identical content never
    // creates a second transaction; conflicting content never overwrites
    {
        TransactionMinter minter;
        Executor pool { 2, 16 };
        BoundedIngressQueue ingress { 32 };
        ControlCore core { minter, pool, succeed_resolver(), cognition, qiven::runtime::StructuralFacts {} };

        const auto key = key_numbered(3);
        QIVEN_VERIFY(ingress.try_push(proposal_for(key, std::byte { 0x03 })));
        drain_until(core, ingress, settled);

        // identical replay after settlement
        QIVEN_VERIFY(ingress.try_push(proposal_for(key, std::byte { 0x03 })));
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

    // §61: evidence for an unknown transaction is dropped and counted,
    // never attached anywhere by guessing
    {
        TransactionMinter minter;
        Executor pool { 1, 4 };
        BoundedIngressQueue ingress { 8 };
        ControlCore core { minter, pool, succeed_resolver(), cognition, qiven::runtime::StructuralFacts {} };

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
        ControlCore core { minter, pool, succeed_resolver(), cognition, qiven::runtime::StructuralFacts {} };

        QIVEN_VERIFY(ingress.try_push(proposal_for(key_numbered(4), std::byte { 0x04 })));
        drain_until(core, ingress, settled);
        const auto id = core.views()[0].id;

        IngressMessage duplicate;
        duplicate.kind                 = IngressMessage::Kind::EvidenceDelivery;
        duplicate.transaction          = id;
        duplicate.requirement.kind     = RequirementKind::MandatoryRecall;
        duplicate.requirement.subject  = "policy context";
        duplicate.requirement.boundary = RequirementBoundary::BeforeJudgment;
        duplicate.requirement.blocking = true;
        duplicate.receipt              = qiven::runtime::resolver::make_evidence_receipt(
            duplicate.requirement, qiven::runtime::resolver::ResolverIdentity { "loopback", 1 }, "policy context",
            "loopback-core", std::span<const std::byte> {});
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

        qiven::runtime::ResolverFn resolver = [&](const qiven::runtime::RequirementIdentity& identity) {
            if (identity.subject == "gate receipt")
            {
                while (!release_second.load())
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
            }
            return succeed_resolver()(identity);
        };
        ControlCore core { minter, pool, resolver, cognition, qiven::runtime::StructuralFacts {} };

        QIVEN_VERIFY(ingress.try_push(proposal_for(key_numbered(5), std::byte { 0x05 })));

        // wait until one report landed and the other is still in flight
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
        auto run = [&cognition] {
            TransactionMinter minter;
            Executor pool { 2, 16 };
            BoundedIngressQueue ingress { 32 };
            ControlCore core { minter, pool, succeed_resolver(), cognition, qiven::runtime::StructuralFacts {} };
            QIVEN_VERIFY(ingress.try_push(proposal_for(key_numbered(6), std::byte { 0x06 })));
            QIVEN_VERIFY(ingress.try_push(proposal_for(key_numbered(7), std::byte { 0x07 })));
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
        ControlCore core { minter, pool, succeed_resolver(), cognition, qiven::runtime::StructuralFacts {} };

        const auto key = key_numbered(8);
        std::vector<std::thread> racers;
        for (int i = 0; i < 4; ++i)
        {
            racers.emplace_back([&ingress, &key] {
                while (!ingress.try_push(proposal_for(key, std::byte { 0x08 })))
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
        ControlCore core { minter, pool, succeed_resolver(), cognition, qiven::runtime::StructuralFacts {} };

        for (u64 n = 100; n < 150; ++n)
        {
            QIVEN_VERIFY(ingress.try_push(proposal_for(key_numbered(n), std::byte { static_cast<unsigned char>(n) })));
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
