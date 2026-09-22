// ============================================================================
// journal_commands.cpp — command semantics, uniqueness constraints, state
// machine law (MVP-1; ARCH §10.2, §13.1, §11.1; design docs/design/
// mvp1-journal.md §6 row 1)
//
// Proves: only commands transition state and terminal states never
// regress; single-consumption uniqueness at the storage layer; one
// outcome per dispatch; one lease row per workspace with fencing epoch
// +1 on every acquire (expired takeover included); decisions expire.
// ============================================================================

#include <qiven/contracts.hpp>
#include <qiven/runtime/auth.hpp>
#include <qiven/runtime/identity.hpp>
#include <qiven/runtime/journal/runtime_journal.hpp>

#include <cstdio>
#include <filesystem>
#include <string>

namespace
{
using qiven::runtime::journal::JournalOpenIntent;
using qiven::runtime::journal::RuntimeJournal;

constexpr qiven::u64 t0 = 1'000'000; // injected wall clock (ms)

std::filesystem::path journal_file(const char* name)
{
    const auto root = std::filesystem::path(QIVEN_RUNTIME_TEST_WORKROOT) / "commands";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    return root / name;
}

qiven::runtime::ContentDigest digest_of(char fill)
{
    qiven::runtime::ContentDigest digest;
    digest.sha256.fill(static_cast<std::byte>(fill));
    return digest;
}

qiven::runtime::journal::DecisionBind make_decision(
    qiven::runtime::SortableIdMinter& minter,
    qiven::runtime::ControlTransactionId tx,
    qiven::u64 expires)
{
    // A distinct token hash per decision (hash-only ledger identity).
    qiven::runtime::journal::DecisionBind bind;
    bind.id               = minter.next();
    bind.token_hash.value = qiven::SHA256Digest {};
    bind.token_hash.value.fill(static_cast<std::byte>(0xAB));
    bind.token_hash.value[0] = static_cast<std::byte>(tx.value & 0xFFu);
    bind.transaction         = tx;
    bind.generation.value    = 1;
    bind.binding_digest      = digest_of(0x11);
    bind.expires_ms          = expires;
    return bind;
}
} // namespace

int main()
{
    using namespace qiven::runtime;
    using namespace qiven::runtime::journal;
    namespace decision_state = qiven::runtime::journal::decision_state;
    namespace dispatch_state = qiven::runtime::journal::dispatch_state;
    namespace tx_state       = qiven::runtime::journal::tx_state;

    qiven::runtime::SortableIdMinter minter;

    auto opened = RuntimeJournal::open(journal_file("commands.sqlite3"),
                                       JournalOpenIntent::CreateNew, t0);
    QIVEN_VERIFY(opened.is_ok());
    auto journal = std::move(opened.value());

    // ---- state machine law: only commands transition ---------------------
    {
        // A transaction is born observed and advances decided -> prepared ->
        // succeeded strictly through bind/prepare/outcome commands.
        auto generation = journal->advance_generation(RuntimeGenerationId { 1 },
                                                      digest_of(0x01), digest_of(0x02),
                                                      "test-build", t0 + 1);
        QIVEN_VERIFY(generation.is_ok());

        auto session = journal->open_session(
            { RuntimeGenerationId { 1 }, std::optional<qiven::u64> { 7 }, "zcode" },
            t0 + 2);
        QIVEN_VERIFY(session.is_ok());

        TransactionOpen open;
        open.correlation    = "gen=1;adapter=1;session=1;actor=7;action=1";
        open.request_digest = digest_of(0x03);
        auto tx             = journal->open_transaction(open, t0 + 3);
        QIVEN_VERIFY(tx.is_ok());
        QIVEN_VERIFY(journal->tx_state_of(tx.value()).value() == tx_state::observed);

        // Evidence acceptance keeps the transaction pre-decision.
        const ContentDigest evidence[] = { digest_of(0x04), digest_of(0x05) };
        QIVEN_VERIFY(journal->accept_evidence(tx.value(), evidence, t0 + 4).is_ok());
        QIVEN_VERIFY(journal->tx_state_of(tx.value()).value() == tx_state::observed);

        // Binding a decision moves it to decided.
        auto bind = make_decision(minter, tx.value(), t0 + 1'000'000);
        QIVEN_VERIFY(journal->bind_decision(bind, t0 + 5).is_ok());
        QIVEN_VERIFY(journal->tx_state_of(tx.value()).value() == tx_state::decided);
        QIVEN_VERIFY(journal->decision_state_of(bind.id).value() == decision_state::bound);

        // A second decision cannot bind onto a decided transaction.
        auto second = make_decision(minter, tx.value(), t0 + 1'000'000);
        QIVEN_VERIFY(!journal->bind_decision(second, t0 + 6).is_ok());

        // Dispatch preparation requires decided.
        journal::DispatchPrepared dispatch;
        dispatch.id          = minter.next();
        dispatch.transaction = tx.value();
        dispatch.plan_digest = digest_of(0x06);
        QIVEN_VERIFY(journal->record_dispatch_prepared(dispatch, t0 + 7).is_ok());
        QIVEN_VERIFY(journal->tx_state_of(tx.value()).value() == tx_state::prepared);
        QIVEN_VERIFY(
            journal->dispatch_state_of(dispatch.id).value() == dispatch_state::prepared);

        // Outcome closes dispatch AND transaction terminally.
        journal::OutcomeRecord outcome;
        outcome.dispatch     = dispatch.id;
        outcome.status       = std::string(tx_state::succeeded);
        outcome.ref_observed = std::optional<std::string> { "abc123" };
        QIVEN_VERIFY(journal->record_outcome(outcome, t0 + 8).is_ok());
        QIVEN_VERIFY(
            journal->dispatch_state_of(dispatch.id).value() == dispatch_state::succeeded);
        QIVEN_VERIFY(journal->tx_state_of(tx.value()).value() == tx_state::succeeded);

        // Terminal states never regress: a second outcome is rejected (one
        // final observation per dispatch, outcomes PK + guarded update).
        QIVEN_VERIFY(!journal->record_outcome(outcome, t0 + 9).is_ok());
        QIVEN_VERIFY(journal->tx_state_of(tx.value()).value() == tx_state::succeeded);

        // A new dispatch cannot attach to a terminal transaction.
        journal::DispatchPrepared late;
        late.id          = minter.next();
        late.transaction = tx.value();
        late.plan_digest = digest_of(0x07);
        QIVEN_VERIFY(!journal->record_dispatch_prepared(late, t0 + 10).is_ok());
    }

    // ---- single consumption at the storage layer --------------------------
    {
        TransactionOpen open;
        open.correlation    = "gen=1;adapter=1;session=1;actor=7;action=2";
        open.request_digest = digest_of(0x08);
        auto tx             = journal->open_transaction(open, t0 + 20);
        QIVEN_VERIFY(tx.is_ok());

        auto bind = make_decision(minter, tx.value(), t0 + 1'000'000);
        QIVEN_VERIFY(journal->bind_decision(bind, t0 + 21).is_ok());

        // Consume once: bound -> consumed with consumed_ms set.
        QIVEN_VERIFY(journal->consume_decision(bind.id, t0 + 22).is_ok());
        QIVEN_VERIFY(journal->decision_state_of(bind.id).value() ==
                     decision_state::consumed);

        // Consume again: typed rejection; state unchanged.
        auto again = journal->consume_decision(bind.id, t0 + 23);
        QIVEN_VERIFY(!again.is_ok());
        QIVEN_VERIFY(again.reason().message.find("consumed") != std::string::npos);
        QIVEN_VERIFY(journal->decision_state_of(bind.id).value() ==
                     decision_state::consumed);
    }

    // ---- decision expiry is a denial, not a consumption -------------------
    {
        TransactionOpen open;
        open.correlation    = "gen=1;adapter=1;session=1;actor=7;action=3";
        open.request_digest = digest_of(0x09);
        auto tx             = journal->open_transaction(open, t0 + 30);
        QIVEN_VERIFY(tx.is_ok());

        auto bind = make_decision(minter, tx.value(), t0 + 50); // expires soon
        QIVEN_VERIFY(journal->bind_decision(bind, t0 + 31).is_ok());

        auto late = journal->consume_decision(bind.id, t0 + 60); // past expiry
        QIVEN_VERIFY(!late.is_ok());
        QIVEN_VERIFY(late.reason().message.find("expired") != std::string::npos);
        QIVEN_VERIFY(journal->decision_state_of(bind.id).value() == decision_state::bound);
    }

    // ---- lease: one row per workspace, epoch +1 every acquire -------------
    {
        journal::LeaseRequest request;
        request.workspace      = "D:/work/qiven-context";
        request.holder_install = "install-A";
        request.ttl_ms         = 1'000;

        auto first = journal->acquire_lease(request, t0 + 40);
        QIVEN_VERIFY(first.is_ok());
        QIVEN_VERIFY(first.value().fencing_epoch == 1);

        // Renewal by the same holder bumps the epoch.
        auto renewed = journal->acquire_lease(request, t0 + 41);
        QIVEN_VERIFY(renewed.is_ok());
        QIVEN_VERIFY(renewed.value().fencing_epoch == 2);

        // A competing holder while the lease is live: typed rejection, the
        // row is untouched (epoch stays).
        journal::LeaseRequest competitor;
        competitor.workspace      = request.workspace;
        competitor.holder_install = "install-B";
        competitor.ttl_ms         = 1'000;
        auto denied               = journal->acquire_lease(competitor, t0 + 42);
        QIVEN_VERIFY(!denied.is_ok());
        auto state = journal->lease_of(request.workspace);
        QIVEN_VERIFY(state.is_ok() && state.value().has_value());
        QIVEN_VERIFY(state.value()->fencing_epoch == 2);

        // After expiry the competitor takes over — epoch INCREASES (never
        // decreases; the row is the high-water mark).
        auto takeover = journal->acquire_lease(competitor, t0 + 42 + 1'000);
        QIVEN_VERIFY(takeover.is_ok());
        QIVEN_VERIFY(takeover.value().fencing_epoch == 3);
        QIVEN_VERIFY(takeover.value().holder_install == "install-B");

        // Exactly one lease row exists for the workspace.
        auto rows = journal->lease_of(request.workspace);
        QIVEN_VERIFY(rows.is_ok() && rows.value().has_value());
        QIVEN_VERIFY(rows.value()->fencing_epoch == 3);
    }

    // ---- barriers: one active per scope, close is verified ----------------
    {
        journal::BarrierOpen barrier;
        barrier.scope    = "workspace-projection";
        barrier.reason   = "dirty-projection";
        barrier.evidence = "diff-nonempty";
        QIVEN_VERIFY(journal->open_barrier(barrier, t0 + 50).is_ok());
        QIVEN_VERIFY(journal->barrier_active("workspace-projection").value());

        // A second open on the same scope while active: rejected (PK).
        QIVEN_VERIFY(!journal->open_barrier(barrier, t0 + 51).is_ok());

        QIVEN_VERIFY(journal->close_barrier("workspace-projection", "repaired", t0 + 52).is_ok());
        QIVEN_VERIFY(!journal->barrier_active("workspace-projection").value());

        // Closing again: no active barrier, typed rejection.
        QIVEN_VERIFY(!journal->close_barrier("workspace-projection", "again", t0 + 53).is_ok());
    }

    // ---- port append: durable record trail through the MVP-0 port ---------
    {
        port::JournalRecord record;
        record.kind        = port::JournalRecordKind::AuditEvent;
        record.record_id   = minter.next();
        record.transaction = ControlTransactionId { 1 };
        record.payload     = { std::byte { 0x01 }, std::byte { 0x02 } };
        QIVEN_VERIFY(journal->append(record).is_ok());

        auto via_port = journal->recover(); // port surface, wall clock
        QIVEN_VERIFY(via_port.audit_chain_verified);
    }

    // ---- reopen: durable identity and audit continuity --------------------
    {
        const auto file                = journal_file("commands.sqlite3");
        const qiven::u64 events_before = journal->audit_event_count().value();
        QIVEN_VERIFY(journal->verify_audit_chain().is_ok());
        journal.reset(); // close the connection cleanly

        auto reopened = RuntimeJournal::open(file, JournalOpenIntent::OpenExisting, t0 + 60);
        QIVEN_VERIFY(reopened.is_ok());
        QIVEN_VERIFY(reopened.value()->audit_event_count().value() == events_before);
        QIVEN_VERIFY(reopened.value()->verify_audit_chain().is_ok());
        // Durable facts survive the reopen.
        QIVEN_VERIFY(reopened.value()->tx_state_of(ControlTransactionId { 1 }).value() ==
                     tx_state::succeeded);
    }

    std::printf("[ OK ] journal-commands\n");
    return 0;
}
