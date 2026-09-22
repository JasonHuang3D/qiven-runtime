#pragma once

// ============================================================================
// journal/runtime_journal.hpp — the SQLite control journal (MVP-1)
// (production-MVP architecture §10-§11, §13; cpp-design §7.1-§7.4; batch
// design docs/design/mvp1-journal.md §3.1-§3.4)
//
// Two types:
//   JournalDb     — move-only RAII owner of the sqlite3 connection with
//                   the ARCH §10.1 configuration applied and open-time
//                   validation (quick_check, schema, fresh-database
//                   guard). The wrapper IS the seam (cpp-design §7.1:
//                   no interface abstraction over sqlite3*).
//   RuntimeJournal — the command API (§7.4: state transitions are journal
//                   COMMANDS only, callers never write rows) and the
//                   IRuntimeJournalPort implementation landed by MVP-0.
//
// Failure channel: qiven::Result with journal error codes 51-55
// (schema.hpp). Single control thread (GR-4): this class is NOT
// thread-safe and does not need to be — everything crossing a thread
// boundary is a value message.
// ============================================================================

#include <qiven/result.hpp>
#include <qiven/runtime/identity.hpp>
#include <qiven/runtime/journal/recovery.hpp>
#include <qiven/runtime/journal/schema.hpp>
#include <qiven/runtime/port/runtime_journal.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace qiven::runtime::journal
{
// Why the journal file gets created vs opened — never both (batch design
// §3.1 fresh-database guard: corruption must not silently produce a fresh
// database followed by mutation).
enum class JournalOpenIntent : u8
{
    CreateNew,   // path must NOT exist (a zero-byte file is a truncation artifact)
    OpenExisting // path must exist and validate as OUR journal
};

// ---------------------------------------------------------------------------
// Statement handle (RAII finalize; move-only). Text/blob views point into
// SQLite-owned memory and stay valid until the next step/reset/finalize —
// callers copy when retaining.
// ---------------------------------------------------------------------------
class JournalStmt
{
public:
    JournalStmt() = default;
    explicit JournalStmt(sqlite3_stmt* stmt) noexcept;
    ~JournalStmt();
    JournalStmt(JournalStmt&& other) noexcept;
    JournalStmt& operator=(JournalStmt&& other) noexcept;
    JournalStmt(const JournalStmt&)            = delete;
    JournalStmt& operator=(const JournalStmt&) = delete;

    // 1-based parameter binding
    [[nodiscard]] qiven::Result<void> bind(int index, i64 value);
    [[nodiscard]] qiven::Result<void> bind(int index, std::string_view value);
    [[nodiscard]] qiven::Result<void> bind(int index, std::span<const std::byte> value);
    [[nodiscard]] qiven::Result<void> bind_null(int index);

    // true = SQLITE_ROW (a row is available); false = SQLITE_DONE.
    [[nodiscard]] qiven::Result<bool> step();

    // 0-based column readers (valid after step() returned true).
    [[nodiscard]] i64 col_i64(int column) const noexcept;
    [[nodiscard]] std::string_view col_text(int column) const noexcept;
    [[nodiscard]] std::span<const std::byte> col_blob(int column) const noexcept;
    [[nodiscard]] bool col_is_null(int column) const noexcept;

    void reset() noexcept; // back to pre-step state for re-execution

    [[nodiscard]] bool valid() const noexcept;

private:
    sqlite3_stmt* m_stmt = nullptr;
};

// ---------------------------------------------------------------------------
// JournalDb — connection owner with open-time validation
// ---------------------------------------------------------------------------
class JournalDb
{
public:
    JournalDb() = default;
    ~JournalDb();
    JournalDb(JournalDb&& other) noexcept;
    JournalDb& operator=(JournalDb&& other) noexcept;
    JournalDb(const JournalDb&)            = delete;
    JournalDb& operator=(const JournalDb&) = delete;

    // Open (or deliberately create) the journal file. CreateNew refuses an
    // existing path; OpenExisting refuses a missing/corrupt/foreign file.
    // Errors: 51 open/corrupt, 52 schema/migration.
    [[nodiscard]] static qiven::Result<JournalDb> open(const std::filesystem::path& file,
                                                       JournalOpenIntent intent,
                                                       u64 now_ms);

    // BEGIN IMMEDIATE … body … COMMIT; any failure or thrown error path is
    // impossible (no exceptions) — a failed body returns ROLLBACK.
    [[nodiscard]] qiven::Result<void> txn(const std::function<qiven::Result<void>()>& body);

    [[nodiscard]] qiven::Result<void> exec(std::string_view sql);
    [[nodiscard]] qiven::Result<JournalStmt> prepare(std::string_view sql);

    // PRAGMA quick_check — false + reason on corruption (51).
    [[nodiscard]] qiven::Result<bool> quick_check();

    // WAL checkpoint (TRUNCATE) for graceful-shutdown callers.
    [[nodiscard]] qiven::Result<void> checkpoint_wal();

    // Last INSERT row id on this connection (sessions table).
    [[nodiscard]] i64 last_insert_rowid() const noexcept;

    [[nodiscard]] bool is_open() const noexcept;

private:
    explicit JournalDb(sqlite3* db) noexcept;
    sqlite3* m_db = nullptr; // closed by dtor (sqlite3_close_v2), never leaked
};

// ---------------------------------------------------------------------------
// Command payloads
// ---------------------------------------------------------------------------
struct SessionOpen
{
    RuntimeGenerationId generation {};
    std::optional<u64> actor; // loopback sessions are actor-less on day one
    std::string harness;
};

struct TransactionOpen
{
    std::optional<ControlTransactionId> causal_parent; // §8.2 causal chain
    std::string correlation;                           // full-value CorrelationKey render
    ContentDigest request_digest {};
    std::optional<std::string> base_revision;
};

struct DecisionBind
{
    DecisionId id {};        // SortableId128 — caller-minted, PK fail-closed
    TokenHash token_hash {}; // hash only; the plaintext never reaches the journal
    ControlTransactionId transaction {};
    RuntimeGenerationId generation {};
    ContentDigest binding_digest {};
    u64 expires_ms = 0;
};

struct LeaseRequest
{
    std::string workspace;      // canonical repository-root identity
    std::string holder_install; // installation id of the acquiring host
    u64 ttl_ms = 0;
};

struct LeaseState
{
    std::string workspace;
    std::string holder_install;
    u64 boot_epoch    = 0;
    u64 fencing_epoch = 0; // monotonic high-water mark — never decreases
    u64 acquired_ms   = 0;
    u64 expires_ms    = 0;
};

struct DispatchPrepared
{
    SortableId128 id {}; // DispatchId — SortableId128, caller-minted
    ControlTransactionId transaction {};
    ContentDigest plan_digest {};
};

struct OutcomeRecord
{
    SortableId128 dispatch {};
    std::string status; // succeeded | failed | indeterminate (dispatch terminal map)
    std::optional<std::string> ref_observed;
    std::optional<ContentDigest> validator_digest;
};

struct BarrierOpen
{
    std::string scope; // active barrier denies mutation in this scope
    std::string reason;
    std::string evidence;
};

// ---------------------------------------------------------------------------
// RuntimeJournal — the command API + IRuntimeJournalPort
// ---------------------------------------------------------------------------
class RuntimeJournal final : public port::IRuntimeJournalPort
{
public:
    // All mutating commands deny while quarantined (54); every command is
    // exactly one BEGIN IMMEDIATE transaction that also appends its audit
    // event; every now_ms is compared against last_wall_ms (D-8, 55).
    [[nodiscard]] static qiven::Result<std::unique_ptr<RuntimeJournal>> open(
        const std::filesystem::path& file, JournalOpenIntent intent, u64 now_ms);

    // --- commands (cpp-design §7.4) --------------------------------------
    [[nodiscard]] qiven::Result<u64> open_session(const SessionOpen& session, u64 now_ms);
    [[nodiscard]] qiven::Result<ControlTransactionId> open_transaction(const TransactionOpen& open,
                                                                       u64 now_ms);
    [[nodiscard]] qiven::Result<void> accept_evidence(ControlTransactionId transaction,
                                                      std::span<const ContentDigest> evidence,
                                                      u64 now_ms);
    [[nodiscard]] qiven::Result<void> bind_decision(const DecisionBind& decision, u64 now_ms);
    [[nodiscard]] qiven::Result<void> consume_decision(const DecisionId& id, u64 now_ms);
    [[nodiscard]] qiven::Result<LeaseState> acquire_lease(const LeaseRequest& request,
                                                          u64 now_ms);
    [[nodiscard]] qiven::Result<void> record_dispatch_prepared(const DispatchPrepared& dispatch,
                                                               u64 now_ms);
    [[nodiscard]] qiven::Result<void> record_outcome(const OutcomeRecord& outcome, u64 now_ms);
    [[nodiscard]] qiven::Result<void> open_barrier(const BarrierOpen& barrier, u64 now_ms);
    [[nodiscard]] qiven::Result<void> close_barrier(std::string_view scope,
                                                    std::string_view evidence,
                                                    u64 now_ms);
    [[nodiscard]] qiven::Result<void> advance_generation(RuntimeGenerationId id,
                                                         const ContentDigest& bundle_digest,
                                                         const ContentDigest& profile_digest,
                                                         std::string_view build_id,
                                                         u64 now_ms);

    // --- recovery ---------------------------------------------------------
    // The inspector decides unresolved-dispatch classification; the default
    // is fail-closed (IndeterminateConflict + barrier). MVP-6 installs the
    // real Git ref/object inspector (batch design §3.5, deferral J-3).
    void set_inspector(std::unique_ptr<IReconciliationInspector> inspector);

    [[nodiscard]] qiven::Result<port::JournalRecoveryReport> recover_at(u64 now_ms);

    // --- read surface (control facts; no audit side effects) --------------
    [[nodiscard]] qiven::Result<std::string> tx_state_of(ControlTransactionId id);
    [[nodiscard]] qiven::Result<std::string> decision_state_of(const DecisionId& id);
    [[nodiscard]] qiven::Result<std::string> dispatch_state_of(const SortableId128& id);
    [[nodiscard]] qiven::Result<DecisionId> first_decision_of_transaction(
        ControlTransactionId id);
    [[nodiscard]] qiven::Result<SortableId128> first_dispatch_of_transaction(
        ControlTransactionId id);
    [[nodiscard]] qiven::Result<std::optional<LeaseState>> lease_of(std::string_view workspace);
    [[nodiscard]] qiven::Result<bool> barrier_active(std::string_view scope);
    [[nodiscard]] qiven::Result<u64> audit_event_count();
    [[nodiscard]] qiven::Result<u64> boot_epoch();
    [[nodiscard]] qiven::Result<std::string> install_id();
    [[nodiscard]] qiven::Result<void> verify_audit_chain();

    // --- IRuntimeJournalPort ----------------------------------------------
    // append(): durable append of one control fact as a chain-protected
    // audit event carrying the record's typed payload bytes (the rich
    // mutations go through the commands above; the port carries the record
    // trail, port/runtime_journal.hpp).
    [[nodiscard]] qiven::Result<void> append(const port::JournalRecord& record) override;
    // recover(): recovery at the live wall clock; errors map to a
    // Quarantined report (the port contract has no error channel).
    [[nodiscard]] port::JournalRecoveryReport recover() override;

    ~RuntimeJournal() override                       = default;
    RuntimeJournal(const RuntimeJournal&)            = delete;
    RuntimeJournal& operator=(const RuntimeJournal&) = delete;

private:
    RuntimeJournal(JournalDb&& db, std::string install_id_value, u64 boot_epoch_value);

    // Applies one inspector resolution inside the open recovery
    // transaction: outcome row + guarded terminal transitions + audit.
    [[nodiscard]] qiven::Result<void> apply_resolution(const PendingDispatch& dispatch,
                                                       const std::string& status,
                                                       const std::optional<std::string>& ref_observed,
                                                       u64 now_ms);

    [[nodiscard]] qiven::Result<void> deny_if_quarantined();
    [[nodiscard]] qiven::Result<void> check_clock(u64 now_ms);
    [[nodiscard]] qiven::Result<void> touch_wall_clock(u64 now_ms); // inside the txn
    [[nodiscard]] qiven::Result<void> append_audit(std::string_view kind,
                                                   const std::vector<std::byte>& payload);

    JournalDb m_db;
    std::string m_install_id;
    u64 m_boot_epoch   = no_boot_epoch;
    bool m_quarantined = false;
    std::unique_ptr<IReconciliationInspector> m_inspector =
        std::make_unique<FailClosedInspector>();
};
} // namespace qiven::runtime::journal
