// ============================================================================
// journal_crash_recovery.cpp — self-spawning crash-injection at every
// journal commit point (MVP-1 exit gate; ARCH §15 MVP-1 / §16.3; design
// docs/design/mvp1-journal.md §6 row 4, §3.6)
//
// Parent spawns ITSELF as a child with a scripted journal transaction; the
// child arms one crash point and one death mode and dies at it
// (TerminateProcess — power-loss semantics, no CRT death notifications).
// The parent then reopens the journal, runs recovery, and asserts the
// DETERMINISTIC classification for that point. "The process restarted" is
// never an assertion (ARCH §16.3).
//
// Exit-gate rows proven:
//   1. forced termination at every journal commit point → deterministic
//      restart result (per-point table below);
//   2. consumed decisions never reappear (re-consume denied after restart;
//      stale-marking touches only bound decisions);
//   3. fencing epoch never decreases (crash → reopen → re-acquire ≥ N).
// ============================================================================

#include <qiven/contracts.hpp>
#include <qiven/runtime/identity.hpp>
#include <qiven/runtime/journal/recovery.hpp>
#include <qiven/runtime/journal/runtime_journal.hpp>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace
{
using qiven::runtime::journal::JournalOpenIntent;
using qiven::runtime::journal::RuntimeJournal;
using qiven::runtime::journal::crash::CrashMode;
using qiven::runtime::journal::crash::CrashPoint;

constexpr qiven::u64 t0 = 4'000'000;

qiven::runtime::ContentDigest digest_of(char fill)
{
    qiven::runtime::ContentDigest digest;
    digest.sha256.fill(static_cast<std::byte>(fill));
    return digest;
}

struct ScriptedPoint
{
    const char* script; // child transaction script name
    CrashPoint point;   // where the child dies
    CrashMode mode;     // before or after the durability boundary
};

// The seven live journal points (§16.3 journal subset; batch design §6).
constexpr ScriptedPoint k_points[] = {
    { "create_tx", CrashPoint::after_create_tx, CrashMode::die_before_commit },
    { "create_tx", CrashPoint::after_create_tx, CrashMode::die_after_commit },
    { "evidence", CrashPoint::after_evidence_persist, CrashMode::die_before_commit },
    { "evidence", CrashPoint::after_evidence_persist, CrashMode::die_after_commit },
    { "decision", CrashPoint::after_decision_persist, CrashMode::die_before_commit },
    { "decision", CrashPoint::after_decision_persist, CrashMode::die_after_commit },
    { "consume", CrashPoint::after_token_consume, CrashMode::die_before_commit },
    { "consume", CrashPoint::after_token_consume, CrashMode::die_after_commit },
    { "lease", CrashPoint::after_lease_fence, CrashMode::die_before_commit },
    { "lease", CrashPoint::after_lease_fence, CrashMode::die_after_commit },
    { "dispatch", CrashPoint::after_dispatch_commit, CrashMode::die_before_commit },
    { "dispatch", CrashPoint::after_dispatch_commit, CrashMode::die_after_commit },
    { "outcome", CrashPoint::after_outcome_commit, CrashMode::die_before_commit },
    { "outcome", CrashPoint::after_outcome_commit, CrashMode::die_after_commit },
};

// ---------------------------------------------------------------------------
// Child side: run one scripted transaction with one armed crash point.
// Exit 0 = completed without dying (the parent treats this as a failure:
// an armed point that never fired, or an unarmed point).
// ---------------------------------------------------------------------------
int run_child(const std::string& script, CrashPoint point, CrashMode mode,
              const std::filesystem::path& file)
{
    qiven::runtime::journal::crash::arm(point, mode);

    auto opened = RuntimeJournal::open(file, JournalOpenIntent::CreateNew, t0);
    if (!opened.is_ok())
    {
        std::printf("child: open failed: %s\n", opened.reason().message.c_str());
        return 2;
    }
    auto journal = std::move(opened.value());

    qiven::runtime::SortableIdMinter minter;

    // Every script needs an active generation (pre-state, never armed).
    if (!journal
             ->advance_generation(qiven::runtime::RuntimeGenerationId { 1 }, digest_of(0x01),
                                  digest_of(0x02), "crash-build", t0 + 1)
             .is_ok())
    {
        return 3;
    }

    qiven::runtime::journal::TransactionOpen open;
    open.correlation    = "gen=1;adapter=1;session=1;actor=1;action=1";
    open.request_digest = digest_of(0x03);

    qiven::runtime::journal::DecisionBind bind;
    bind.id = minter.next();
    bind.token_hash.value.fill(static_cast<std::byte>(0xCD));
    bind.transaction      = qiven::runtime::ControlTransactionId { 1 };
    bind.generation.value = 1;
    bind.binding_digest   = digest_of(0x04);
    bind.expires_ms       = t0 + 10'000'000;

    qiven::runtime::journal::DispatchPrepared dispatch;
    dispatch.id          = minter.next();
    dispatch.transaction = qiven::runtime::ControlTransactionId { 1 };
    dispatch.plan_digest = digest_of(0x05);

    if (script == "create_tx")
    {
        return journal->open_transaction(open, t0 + 2).is_ok() ? 0 : 4;
    }
    if (script == "evidence")
    {
        if (!journal->open_transaction(open, t0 + 2).is_ok())
        {
            return 4;
        }
        const qiven::runtime::ContentDigest evidence[] = { digest_of(0x06) };
        return journal->accept_evidence(qiven::runtime::ControlTransactionId { 1 },
                                        evidence, t0 + 3)
                       .is_ok()
                   ? 0
                   : 4;
    }
    if (script == "decision")
    {
        if (!journal->open_transaction(open, t0 + 2).is_ok())
        {
            return 4;
        }
        return journal->bind_decision(bind, t0 + 3).is_ok() ? 0 : 4;
    }
    if (script == "consume")
    {
        if (!journal->open_transaction(open, t0 + 2).is_ok() ||
            !journal->bind_decision(bind, t0 + 3).is_ok())
        {
            return 4;
        }
        return journal->consume_decision(bind.id, t0 + 4).is_ok() ? 0 : 4;
    }
    if (script == "lease")
    {
        qiven::runtime::journal::LeaseRequest request;
        request.workspace      = "D:/work/qiven-context";
        request.holder_install = "install-crash";
        request.ttl_ms         = 1'000;
        return journal->acquire_lease(request, t0 + 2).is_ok() ? 0 : 4;
    }
    if (script == "dispatch")
    {
        if (!journal->open_transaction(open, t0 + 2).is_ok() ||
            !journal->bind_decision(bind, t0 + 3).is_ok())
        {
            return 4;
        }
        return journal->record_dispatch_prepared(dispatch, t0 + 4).is_ok() ? 0 : 4;
    }
    if (script == "outcome")
    {
        if (!journal->open_transaction(open, t0 + 2).is_ok() ||
            !journal->bind_decision(bind, t0 + 3).is_ok() ||
            !journal->record_dispatch_prepared(dispatch, t0 + 4).is_ok())
        {
            return 4;
        }
        qiven::runtime::journal::OutcomeRecord outcome;
        outcome.dispatch = dispatch.id;
        outcome.status   = std::string(
            qiven::runtime::journal::tx_state::succeeded);
        outcome.ref_observed = std::optional<std::string> { "deadbeef" };
        return journal->record_outcome(outcome, t0 + 5).is_ok() ? 0 : 4;
    }
    return 5;
}

// ---------------------------------------------------------------------------
// Parent side: spawn the child, verify it died AT the armed point, then
// assert the deterministic restart classification.
// ---------------------------------------------------------------------------
bool spawn_child_dies_at(const std::string& script, CrashPoint point, CrashMode mode,
                         const std::filesystem::path& file, DWORD& exit_code)
{
    char own_path[MAX_PATH] {};
    if (GetModuleFileNameA(nullptr, own_path, MAX_PATH) == 0)
    {
        return false;
    }
    std::string command =
        std::string("\"") + own_path + "\" --crash-child " + script + " " +
        std::to_string(static_cast<int>(point)) + " " +
        std::to_string(static_cast<int>(mode)) + " \"" + file.string() + "\"";

    STARTUPINFOA startup {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process {};
    if (CreateProcessA(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                       nullptr, &startup, &process) == FALSE)
    {
        return false;
    }
    CloseHandle(process.hThread);

    // Bounded wait (hang-contract: never unbounded; a hung child is a
    // classification event).
    const DWORD waited = WaitForSingleObject(process.hProcess, 30'000);
    bool died_at_point = false;
    if (waited == WAIT_OBJECT_0 && GetExitCodeProcess(process.hProcess, &exit_code))
    {
        died_at_point = exit_code ==
                        qiven::runtime::journal::crash::crash_exit_base +
                            static_cast<DWORD>(point);
    }
    CloseHandle(process.hProcess);
    return died_at_point;
}

std::string point_name(CrashPoint point)
{
    switch (point)
    {
    case CrashPoint::after_create_tx: return "create_tx";
    case CrashPoint::after_evidence_persist: return "evidence";
    case CrashPoint::after_decision_persist: return "decision";
    case CrashPoint::after_token_consume: return "consume";
    case CrashPoint::after_lease_fence: return "lease";
    case CrashPoint::after_dispatch_commit: return "dispatch";
    case CrashPoint::after_outcome_commit: return "outcome";
    default: return "unarmed";
    }
}

std::string hex_id(const qiven::runtime::SortableId128& id)
{
    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    out.resize(id.bytes.size() * 2);
    for (qiven::usize i = 0; i < id.bytes.size(); ++i)
    {
        const auto b   = static_cast<unsigned char>(id.bytes[i]);
        out[2 * i]     = hex[b >> 4];
        out[2 * i + 1] = hex[b & 0x0Fu];
    }
    return out;
}

using RuntimeJournalPtr = std::unique_ptr<RuntimeJournal>;

qiven::runtime::DecisionId first_decision(const RuntimeJournalPtr& journal)
{
    auto id = journal->first_decision_of_transaction(qiven::runtime::ControlTransactionId { 1 });
    QIVEN_VERIFY(id.is_ok());
    return id.value();
}

qiven::runtime::SortableId128 first_dispatch(const RuntimeJournalPtr& journal)
{
    auto id =
        journal->first_dispatch_of_transaction(qiven::runtime::ControlTransactionId { 1 });
    QIVEN_VERIFY(id.is_ok());
    return id.value();
}
} // namespace

int main(int argc, char** argv)
{
    using namespace qiven::runtime;
    using portcls            = qiven::runtime::port::RecoveryClassification;
    namespace decision_state = qiven::runtime::journal::decision_state;
    namespace dispatch_state = qiven::runtime::journal::dispatch_state;
    namespace tx_state       = qiven::runtime::journal::tx_state;

    // ---- child mode -------------------------------------------------------
    if (argc == 6 && std::string(argv[1]) == "--crash-child")
    {
        return run_child(argv[2], static_cast<CrashPoint>(std::atoi(argv[3])),
                         static_cast<CrashMode>(std::atoi(argv[4])),
                         std::filesystem::path(argv[5]));
    }
    if (argc != 1)
    {
        std::printf("unexpected arguments\n");
        return 1;
    }

    const auto root = std::filesystem::path(QIVEN_RUNTIME_TEST_WORKROOT) / "crash";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);

    int cases = 0;
    for (const ScriptedPoint& scripted : k_points)
    {
        const std::string case_name =
            std::string(scripted.script) + "-" +
            (scripted.mode == CrashMode::die_before_commit ? "before" : "after");
        const auto file = root / (case_name + ".sqlite3");

        DWORD exit_code = 0;
        if (!spawn_child_dies_at(scripted.script, scripted.point, scripted.mode, file,
                                 exit_code))
        {
            std::printf("FAIL [%s]: child did not die at the armed point (exit %lu)\n",
                        case_name.c_str(),
                        static_cast<unsigned long>(exit_code));
            return 1;
        }
        ++cases;

        // Reopen the orphaned journal and recover with a deterministic
        // classification per point (batch design §6 table).
        auto reopened = RuntimeJournal::open(file, JournalOpenIntent::OpenExisting,
                                             t0 + 1'000'000);
        if (!reopened.is_ok())
        {
            std::printf("FAIL [%s]: reopen failed: %s\n", case_name.c_str(),
                        reopened.reason().message.c_str());
            return 1;
        }
        auto journal = std::move(reopened.value());
        auto report  = journal->recover_at(t0 + 1'000'001);
        if (!report.is_ok())
        {
            std::printf("FAIL [%s]: recovery failed: %s\n", case_name.c_str(),
                        report.reason().message.c_str());
            return 1;
        }

        const std::string point = scripted.script;
        const bool before       = scripted.mode == CrashMode::die_before_commit;

        if (point == "create_tx")
        {
            if (before)
            {
                if (journal->tx_state_of(ControlTransactionId { 1 }).is_ok())
                {
                    std::printf("FAIL [%s]: rolled-back tx visible\n", case_name.c_str());
                    return 1;
                }
            }
            else
            {
                if (journal->tx_state_of(ControlTransactionId { 1 }).value() !=
                    tx_state::observed)
                {
                    std::printf("FAIL [%s]: committed tx missing\n", case_name.c_str());
                    return 1;
                }
            }
            if (report.value().classification != portcls::Clean)
            {
                std::printf("FAIL [%s]: classification not Clean\n", case_name.c_str());
                return 1;
            }
        }
        else if (point == "evidence")
        {
            if (journal->tx_state_of(ControlTransactionId { 1 }).value() !=
                tx_state::observed)
            {
                std::printf("FAIL [%s]: tx state wrong\n", case_name.c_str());
                return 1;
            }
            if (report.value().classification != portcls::Clean)
            {
                std::printf("FAIL [%s]: classification not Clean\n", case_name.c_str());
                return 1;
            }
        }
        else if (point == "decision")
        {
            if (before)
            {
                // The bind rolled back: no decision row at all, tx undecided.
                if (journal
                        ->first_decision_of_transaction(ControlTransactionId { 1 })
                        .is_ok())
                {
                    std::printf("FAIL [%s]: rolled-back decision visible\n",
                                case_name.c_str());
                    return 1;
                }
                if (journal->tx_state_of(ControlTransactionId { 1 }).value() !=
                    tx_state::observed)
                {
                    std::printf("FAIL [%s]: tx should still be observed\n",
                                case_name.c_str());
                    return 1;
                }
            }
            else
            {
                // Committed bound decision from an older boot epoch: recovery
                // stale-marks it (exit gate 2 applies to bound decisions).
                const DecisionId id = first_decision(journal);
                if (journal->decision_state_of(id).value() != decision_state::stale)
                {
                    std::printf("FAIL [%s]: bound decision not stale-marked\n",
                                case_name.c_str());
                    return 1;
                }
            }
            if (report.value().classification != portcls::Clean)
            {
                std::printf("FAIL [%s]: classification not Clean\n", case_name.c_str());
                return 1;
            }
        }
        else if (point == "consume")
        {
            const DecisionId id = first_decision(journal);
            if (before)
            {
                // Rollback left it bound; recovery stale-marks it and a
                // post-restart consume is denied — the token never returns.
                if (journal->decision_state_of(id).value() != decision_state::stale)
                {
                    std::printf("FAIL [%s]: rolled-back decision not stale\n",
                                case_name.c_str());
                    return 1;
                }
            }
            else
            {
                // Exit gate 2: consumed decisions never reappear. The state
                // survives the crash AND the restart; re-consumption denied.
                if (journal->decision_state_of(id).value() != decision_state::consumed)
                {
                    std::printf("FAIL [%s]: consumed state lost\n", case_name.c_str());
                    return 1;
                }
            }
            auto reconsume = journal->consume_decision(id, t0 + 2'000'000);
            if (reconsume.is_ok())
            {
                std::printf("FAIL [%s]: decision reconsumed after restart\n",
                            case_name.c_str());
                return 1;
            }
            if (report.value().classification != portcls::Clean)
            {
                std::printf("FAIL [%s]: classification not Clean\n", case_name.c_str());
                return 1;
            }
        }
        else if (point == "lease")
        {
            auto lease = journal->lease_of("D:/work/qiven-context");
            QIVEN_VERIFY(lease.is_ok());
            if (before)
            {
                if (lease.value().has_value())
                {
                    std::printf("FAIL [%s]: rolled-back lease visible\n",
                                case_name.c_str());
                    return 1;
                }
            }
            else
            {
                if (!lease.value().has_value() || lease.value()->fencing_epoch != 1)
                {
                    std::printf("FAIL [%s]: committed lease missing\n",
                                case_name.c_str());
                    return 1;
                }
            }
            // Exit gate 3: the epoch never decreases across crash/restart.
            // Before-commit: nothing was ever committed, so the first real
            // acquire is epoch 1. After-commit: epoch 1 is durable, so the
            // re-acquire must be >= 2 (the high-water mark survived).
            qiven::runtime::journal::LeaseRequest reacquire;
            reacquire.workspace      = "D:/work/qiven-context";
            reacquire.holder_install = "install-crash";
            reacquire.ttl_ms         = 1'000;
            auto next                = journal->acquire_lease(reacquire, t0 + 2'000'000);
            QIVEN_VERIFY(next.is_ok());
            const qiven::u64 epoch_floor = before ? 1 : 2;
            if (next.value().fencing_epoch < epoch_floor)
            {
                std::printf("FAIL [%s]: fencing epoch regressed\n", case_name.c_str());
                return 1;
            }
            if (report.value().classification != portcls::Clean)
            {
                std::printf("FAIL [%s]: classification not Clean\n", case_name.c_str());
                return 1;
            }
        }
        else if (point == "dispatch")
        {
            if (before)
            {
                // Rollback: no dispatch, tx still decided, nothing unresolved.
                if (journal
                        ->first_dispatch_of_transaction(ControlTransactionId { 1 })
                        .is_ok())
                {
                    std::printf("FAIL [%s]: rolled-back dispatch visible\n",
                                case_name.c_str());
                    return 1;
                }
                if (journal->tx_state_of(ControlTransactionId { 1 }).value() !=
                    tx_state::decided)
                {
                    std::printf("FAIL [%s]: tx should still be decided\n",
                                case_name.c_str());
                    return 1;
                }
                if (report.value().classification != portcls::Clean)
                {
                    std::printf("FAIL [%s]: classification not Clean\n",
                                case_name.c_str());
                    return 1;
                }
            }
            else
            {
                // Committed prepared dispatch, no outcome: fail-closed
                // reconciliation — indeterminate + barrier, never a guess.
                const SortableId128 id = first_dispatch(journal);
                if (journal->dispatch_state_of(id).value() !=
                    dispatch_state::indeterminate)
                {
                    std::printf("FAIL [%s]: dispatch not indeterminate\n",
                                case_name.c_str());
                    return 1;
                }
                if (!journal->barrier_active("journal.reconcile." + hex_id(id)).value())
                {
                    std::printf("FAIL [%s]: reconcile barrier missing\n",
                                case_name.c_str());
                    return 1;
                }
                if (report.value().classification != portcls::IndeterminateConflict ||
                    report.value().barriers_opened != 1)
                {
                    std::printf("FAIL [%s]: classification/barrier wrong\n",
                                case_name.c_str());
                    return 1;
                }
            }
        }
        else if (point == "outcome")
        {
            const SortableId128 id = first_dispatch(journal);
            if (before)
            {
                // The outcome rolled back: the dispatch is still prepared
                // with no observation — indeterminate + barrier.
                if (journal->dispatch_state_of(id).value() !=
                    dispatch_state::indeterminate)
                {
                    std::printf("FAIL [%s]: dispatch not indeterminate\n",
                                case_name.c_str());
                    return 1;
                }
                if (report.value().classification != portcls::IndeterminateConflict)
                {
                    std::printf("FAIL [%s]: classification wrong\n", case_name.c_str());
                    return 1;
                }
            }
            else
            {
                // Committed outcome: dispatch and transaction terminal.
                if (journal->dispatch_state_of(id).value() != dispatch_state::succeeded ||
                    journal->tx_state_of(ControlTransactionId { 1 }).value() !=
                        tx_state::succeeded)
                {
                    std::printf("FAIL [%s]: terminal states missing\n",
                                case_name.c_str());
                    return 1;
                }
                if (report.value().classification != portcls::Clean)
                {
                    std::printf("FAIL [%s]: classification not Clean\n",
                                case_name.c_str());
                    return 1;
                }
            }
        }

        std::printf("       [%4d] %-18s %-6s deterministic restart OK\n", cases,
                    point_name(scripted.point).c_str(), before ? "before" : "after");
    }

    std::printf("[ OK ] journal-crash-recovery (%d point-mode cases)\n", cases);
    return 0;
}
