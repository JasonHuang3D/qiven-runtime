#pragma once

// ============================================================================
// port/runtime_journal.hpp — the durable control-journal port
// (production-MVP architecture §10, MVP-0 definition; implementation
// lands with MVP-1 as the pinned SQLite control journal)
//
// The journal is the authoritative record of RUNTIME CONTROL FACTS ONLY —
// sessions, transactions, decisions, leases, dispatches, outcomes,
// barriers and the append-only audit chain. It MUST NOT store the
// authoritative qiven-context domain records (§10.1: no second domain
// truth). Every state transition is a command through this port; callers
// never assign state directly (§13.1), terminal states never regress,
// and single-consumption uniqueness is enforced at the storage layer
// (§10.2 decisions table).
//
// MVP-0 DEFINES the port; the thin test image (state_store.hpp) is
// explicitly NOT a production implementation of it.
// ============================================================================

#include <qiven/result.hpp>
#include <qiven/runtime/decision.hpp>
#include <qiven/runtime/identity.hpp>
#include <qiven/runtime/reconciliation.hpp>

#include <cstddef>
#include <vector>

namespace qiven::runtime::port
{
// Journal record kinds (§10.2 logical tables; the physical schema may
// normalize or partition but must not collapse them).
enum class JournalRecordKind : u8
{
    GenerationActivated, // immutable after activation
    SessionOpened,
    TransactionOpened, // carries causal_parent (§8.2)
    RequirementTracked,
    EvidenceAccepted, // registry-validated receipt fields
    DecisionBound,    // token HASH only; never the plaintext token
    DecisionConsumed, // single-consumption uniqueness constraint
    LeaseAcquired,    // fencing epoch monotonic per workspace
    DispatchPrepared, // persisted BEFORE external execution (§11.2)
    OutcomeObserved,  // one final observation per dispatch
    BarrierOpened,
    BarrierClosed,
    AuditEvent, // append-only hash chain (§10.2)
};

struct JournalRecord
{
    JournalRecordKind kind = JournalRecordKind::AuditEvent;
    SortableId128 record_id {};
    ControlTransactionId transaction {}; // correlation when applicable
    std::vector<std::byte> payload;      // kind-typed canonical preimage bytes
};

// Recovery classification (§11.2): the journal re-inspects authoritative
// state (Git refs and objects) rather than retrying blindly; the outcome
// of a dispatch whose acknowledgement was lost is reconstructed, never
// guessed.
enum class RecoveryClassification : u8
{
    Clean,                 // nothing unresolved
    Reconstructed,         // authoritative state proved the effect
    NotPerformed,          // authoritative state proved no mutation
    IndeterminateConflict, // third-value ref: barrier required, never a guess
    Quarantined,           // journal integrity failure: governed mutation stops
};

struct JournalRecoveryReport
{
    RecoveryClassification classification = RecoveryClassification::Clean;
    usize unresolved_transactions         = 0;
    usize barriers_opened                 = 0;
    bool audit_chain_verified             = false;
};

class IRuntimeJournalPort
{
public:
    virtual ~IRuntimeJournalPort() = default;

    // Append one record durably. A false/failed return means the record
    // is NOT persisted — the caller must fail closed, never proceed on an
    // unrecorded control fact (§11.2 durability order).
    [[nodiscard]] virtual qiven::Result<void> append(const JournalRecord& record) = 0;

    // Startup recovery (§13.2): verify schema, integrity and the audit
    // hash chain; mark unconsumed decisions from older boot epochs stale;
    // classify unresolved dispatches against authoritative state.
    // Corruption places the runtime in quarantine — mutation stops,
    // read-only diagnostics remain.
    [[nodiscard]] virtual JournalRecoveryReport recover() = 0;
};
} // namespace qiven::runtime::port
