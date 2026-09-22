// ============================================================================
// journal/runtime_journal.cpp — JournalDb + RuntimeJournal commands (MVP-1)
// (production-MVP architecture §10-§13; cpp-design §7.1-§7.4; batch design
// docs/design/mvp1-journal.md §3)
//
// Durability law (ARCH §11.2): every command is exactly one BEGIN
// IMMEDIATE transaction that also appends its audit event with prev_hash
// read inside the same transaction. Crash hooks bracket the durability
// boundary: die_before_commit fires with all mutations staged and COMMIT
// not yet issued (death → rollback); die_after_commit fires after COMMIT
// returned (death → effect durable). "The process restarted" is never an
// outcome — the recovery classification is (§16.3).
//
// Command denials return typed errors and do NOT append audit events: the
// failed transaction rolls back, and ARCH §5's "every denial is audited"
// duty belongs to the governed IPC boundary (MVP-3+), not to in-process
// API misuse — otherwise any caller could grow the chain by failing.
// ============================================================================

#include <qiven/runtime/journal/runtime_journal.hpp>

#include <qiven/hashing_sha256.hpp>
#include <qiven/runtime/auth.hpp>

#include <sqlite3.h>

#include <array>
#include <charconv>
#include <chrono>
#include <cstring>
#include <utility>

namespace qiven::runtime::journal
{
namespace
{
using qiven::Error;
using qiven::error_category;

[[nodiscard]] Error err_open(std::string message)
{
    return Error::make(error_category::unavailable, journal_err_open_corrupt, std::move(message));
}
[[nodiscard]] Error err_migration(std::string message)
{
    return Error::make(error_category::internal, journal_err_migration, std::move(message));
}
[[nodiscard]] Error err_constraint(std::string message)
{
    return Error::make(error_category::invalid_argument, journal_err_constraint,
                       std::move(message));
}
[[nodiscard]] Error err_chain(std::string message)
{
    return Error::make(error_category::internal, journal_err_chain, std::move(message));
}
[[nodiscard]] Error err_clock(std::string message)
{
    return Error::make(error_category::unavailable, journal_err_clock_regression,
                       std::move(message));
}

// Forwards a failed Result of ANY value type into a failed Result<T>
// (callers have already checked !is_ok()). The value channel of the source
// is irrelevant; only its reason survives.
template <typename T, typename SourceT>
[[nodiscard]] qiven::Result<T> failed(const qiven::Result<SourceT>& result)
{
    return qiven::Result<T>::fail(result.reason());
}

[[nodiscard]] qiven::Result<void> ok()
{
    return qiven::Result<void>::ok();
}

[[nodiscard]] u64 wall_now_ms() noexcept
{
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

// Manual digits-only u64 parse (no exceptions, no locale; GR-1).
[[nodiscard]] bool parse_u64(std::string_view text, u64& out) noexcept
{
    if (text.empty())
    {
        return false;
    }
    u64 value  = 0;
    u64 factor = 1;
    for (usize i = text.size(); i-- > 0;)
    {
        const char c = text[i];
        if (c < '0' || c > '9')
        {
            return false;
        }
        const u64 digit = static_cast<u64>(c - '0');
        if (value > (std::numeric_limits<u64>::max)() - digit * factor)
        {
            return false;
        }
        value += digit * factor;
        if (i != 0 && factor > (std::numeric_limits<u64>::max)() / 10)
        {
            return false;
        }
        factor *= 10;
    }
    out = value;
    return true;
}

// --- canonical audit-payload encoding (append-only per kind) --------------
void put_u64(std::vector<std::byte>& out, u64 value)
{
    for (int i = 0; i < 8; ++i)
    {
        out.push_back(static_cast<std::byte>((value >> (8 * i)) & 0xFFu));
    }
}
void put_u32(std::vector<std::byte>& out, u32 value)
{
    for (int i = 0; i < 4; ++i)
    {
        out.push_back(static_cast<std::byte>((value >> (8 * i)) & 0xFFu));
    }
}
void put_bytes(std::vector<std::byte>& out, std::span<const std::byte> value)
{
    put_u32(out, static_cast<u32>(value.size()));
    out.insert(out.end(), value.begin(), value.end());
}
void put_str(std::vector<std::byte>& out, std::string_view value)
{
    put_bytes(out, { reinterpret_cast<const std::byte*>(value.data()), value.size() });
}
void put_digest(std::vector<std::byte>& out, const ContentDigest& digest)
{
    out.insert(out.end(), digest.sha256.begin(), digest.sha256.end());
}
void put_id(std::vector<std::byte>& out, const SortableId128& id)
{
    out.insert(out.end(), id.bytes.begin(), id.bytes.end());
}

[[nodiscard]] std::array<std::byte, 32> zero_hash() noexcept
{
    std::array<std::byte, 32> out {};
    out.fill(std::byte { 0 });
    return out;
}

[[nodiscard]] std::string to_hex(std::span<const std::byte> bytes)
{
    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    out.resize(bytes.size() * 2);
    for (usize i = 0; i < bytes.size(); ++i)
    {
        const auto b   = static_cast<unsigned char>(bytes[i]);
        out[2 * i]     = hex[b >> 4];
        out[2 * i + 1] = hex[b & 0x0Fu];
    }
    return out;
}

// event_hash = SHA256(prev_hash ‖ u32le(len(kind)) ‖ kind ‖ canonical_payload)
[[nodiscard]] SHA256Digest chain_hash(const std::array<std::byte, 32>& prev,
                                      std::string_view kind,
                                      std::span<const std::byte> payload)
{
    SHA256Hasher hasher;
    hasher.update(prev.data(), prev.size());
    std::vector<std::byte> kind_wire;
    put_str(kind_wire, kind);
    hasher.update(kind_wire.data(), kind_wire.size());
    hasher.update(payload.data(), payload.size());
    return hasher.finish();
}

constexpr std::string_view kind_journal_created     = "journal_created";
constexpr std::string_view kind_generation_advanced = "generation_advanced";
constexpr std::string_view kind_session_opened      = "session_opened";
constexpr std::string_view kind_transaction_opened  = "transaction_opened";
constexpr std::string_view kind_evidence_accepted   = "evidence_accepted";
constexpr std::string_view kind_decision_bound      = "decision_bound";
constexpr std::string_view kind_decision_consumed   = "decision_consumed";
constexpr std::string_view kind_lease_acquired      = "lease_acquired";
constexpr std::string_view kind_dispatch_prepared   = "dispatch_prepared";
constexpr std::string_view kind_outcome_observed    = "outcome_observed";
constexpr std::string_view kind_barrier_opened      = "barrier_opened";
constexpr std::string_view kind_barrier_closed      = "barrier_closed";
constexpr std::string_view kind_recovery_completed  = "recovery_completed";
constexpr std::string_view kind_quarantine_entered  = "quarantine_entered";

[[nodiscard]] std::string_view port_kind_name(port::JournalRecordKind kind) noexcept
{
    switch (kind)
    {
    case port::JournalRecordKind::GenerationActivated: return "record_generation_activated";
    case port::JournalRecordKind::SessionOpened: return "record_session_opened";
    case port::JournalRecordKind::TransactionOpened: return "record_transaction_opened";
    case port::JournalRecordKind::RequirementTracked: return "record_requirement_tracked";
    case port::JournalRecordKind::EvidenceAccepted: return "record_evidence_accepted";
    case port::JournalRecordKind::DecisionBound: return "record_decision_bound";
    case port::JournalRecordKind::DecisionConsumed: return "record_decision_consumed";
    case port::JournalRecordKind::LeaseAcquired: return "record_lease_acquired";
    case port::JournalRecordKind::DispatchPrepared: return "record_dispatch_prepared";
    case port::JournalRecordKind::OutcomeObserved: return "record_outcome_observed";
    case port::JournalRecordKind::BarrierOpened: return "record_barrier_opened";
    case port::JournalRecordKind::BarrierClosed: return "record_barrier_closed";
    case port::JournalRecordKind::AuditEvent: return "record_audit";
    }
    return "record_unknown";
}

[[nodiscard]] bool is_valid_outcome_status(std::string_view status) noexcept
{
    return status == tx_state::succeeded || status == tx_state::failed ||
           status == tx_state::indeterminate;
}

[[nodiscard]] std::span<const std::byte> id_bytes(const SortableId128& id) noexcept
{
    return { id.bytes.data(), id.bytes.size() };
}
[[nodiscard]] std::span<const std::byte> digest_bytes(const ContentDigest& digest) noexcept
{
    return { digest.sha256.data(), digest.sha256.size() };
}

// --- shared SQL helpers -----------------------------------------------------

[[nodiscard]] qiven::Result<std::optional<std::string>> read_meta(JournalDb& db,
                                                                  std::string_view key)
{
    auto stmt = db.prepare("SELECT value FROM runtime_meta WHERE key = ?");
    if (!stmt.is_ok())
    {
        return qiven::Result<std::optional<std::string>>::fail(stmt.reason());
    }
    if (auto bind = stmt.value().bind(1, key); !bind.is_ok())
    {
        return qiven::Result<std::optional<std::string>>::fail(bind.reason());
    }
    auto row = stmt.value().step();
    if (!row.is_ok())
    {
        return qiven::Result<std::optional<std::string>>::fail(row.reason());
    }
    if (!row.value())
    {
        return std::optional<std::string> {};
    }
    return std::optional<std::string> { std::string(stmt.value().col_text(0)) };
}

[[nodiscard]] qiven::Result<void> write_meta(JournalDb& db, std::string_view key,
                                             std::string_view value)
{
    auto stmt =
        db.prepare("INSERT INTO runtime_meta(key, value) VALUES(?, ?) "
                   "ON CONFLICT(key) DO UPDATE SET value = excluded.value");
    if (!stmt.is_ok())
    {
        return failed<void>(stmt);
    }
    if (auto bind = stmt.value().bind(1, key); !bind.is_ok())
    {
        return failed<void>(bind);
    }
    if (auto bind = stmt.value().bind(2, value); !bind.is_ok())
    {
        return failed<void>(bind);
    }
    auto row = stmt.value().step();
    if (!row.is_ok())
    {
        return failed<void>(row);
    }
    return ok();
}

// Appends one audit event with prev_hash read inside the caller's open
// transaction; returns the event hash (genesis capture).
[[nodiscard]] qiven::Result<SHA256Digest> append_audit_row(JournalDb& db, std::string_view kind,
                                                           std::span<const std::byte> payload)
{
    auto prev_stmt = db.prepare("SELECT event_hash FROM audit_events ORDER BY seq DESC LIMIT 1");
    if (!prev_stmt.is_ok())
    {
        return qiven::Result<SHA256Digest>::fail(prev_stmt.reason());
    }
    std::array<std::byte, 32> prev = zero_hash();
    auto prev_row                  = prev_stmt.value().step();
    if (!prev_row.is_ok())
    {
        return qiven::Result<SHA256Digest>::fail(prev_row.reason());
    }
    if (prev_row.value())
    {
        const auto prev_blob = prev_stmt.value().col_blob(0);
        if (prev_blob.size() != prev.size())
        {
            return qiven::Result<SHA256Digest>::fail(err_chain("audit-prev-hash-corrupt"));
        }
        std::memcpy(prev.data(), prev_blob.data(), prev.size());
    }

    const SHA256Digest event = chain_hash(prev, kind, payload);
    auto insert              = db.prepare(
        "INSERT INTO audit_events(prev_hash, event_hash, kind, payload) VALUES(?, ?, ?, ?)");
    if (!insert.is_ok())
    {
        return qiven::Result<SHA256Digest>::fail(insert.reason());
    }
    if (auto bind = insert.value().bind(1, std::span<const std::byte> { prev.data(), prev.size() });
        !bind.is_ok())
    {
        return qiven::Result<SHA256Digest>::fail(bind.reason());
    }
    if (auto bind = insert.value().bind(2, std::span<const std::byte> { event.data(), event.size() });
        !bind.is_ok())
    {
        return qiven::Result<SHA256Digest>::fail(bind.reason());
    }
    if (auto bind = insert.value().bind(3, kind); !bind.is_ok())
    {
        return qiven::Result<SHA256Digest>::fail(bind.reason());
    }
    if (auto bind = insert.value().bind(4, payload); !bind.is_ok())
    {
        return qiven::Result<SHA256Digest>::fail(bind.reason());
    }
    auto row = insert.value().step();
    if (!row.is_ok())
    {
        return qiven::Result<SHA256Digest>::fail(row.reason());
    }
    return event;
}

// Reads one TEXT state column for one INTEGER key; 53 when absent.
[[nodiscard]] qiven::Result<std::string> read_state_by_i64(JournalDb& db,
                                                           std::string_view sql,
                                                           i64 key)
{
    auto stmt = db.prepare(sql);
    if (!stmt.is_ok())
    {
        return qiven::Result<std::string>::fail(stmt.reason());
    }
    if (auto bind = stmt.value().bind(1, key); !bind.is_ok())
    {
        return qiven::Result<std::string>::fail(bind.reason());
    }
    auto row = stmt.value().step();
    if (!row.is_ok())
    {
        return qiven::Result<std::string>::fail(row.reason());
    }
    if (!row.value())
    {
        return qiven::Result<std::string>::fail(err_constraint("journal-row-not-found"));
    }
    return std::string(stmt.value().col_text(0));
}

// Reads one TEXT state column for one BLOB key; 53 when absent.
[[nodiscard]] qiven::Result<std::string> read_state_by_blob(JournalDb& db,
                                                            std::string_view sql,
                                                            std::span<const std::byte> key)
{
    auto stmt = db.prepare(sql);
    if (!stmt.is_ok())
    {
        return qiven::Result<std::string>::fail(stmt.reason());
    }
    if (auto bind = stmt.value().bind(1, key); !bind.is_ok())
    {
        return qiven::Result<std::string>::fail(bind.reason());
    }
    auto row = stmt.value().step();
    if (!row.is_ok())
    {
        return qiven::Result<std::string>::fail(row.reason());
    }
    if (!row.value())
    {
        return qiven::Result<std::string>::fail(err_constraint("journal-row-not-found"));
    }
    return std::string(stmt.value().col_text(0));
}
} // namespace

// ===========================================================================
// JournalStmt
// ===========================================================================
JournalStmt::JournalStmt(sqlite3_stmt* stmt) noexcept :
m_stmt(stmt)
{
}

JournalStmt::~JournalStmt()
{
    if (m_stmt != nullptr)
    {
        sqlite3_finalize(m_stmt);
    }
}

JournalStmt::JournalStmt(JournalStmt&& other) noexcept :
m_stmt(std::exchange(other.m_stmt, nullptr))
{
}

JournalStmt& JournalStmt::operator=(JournalStmt&& other) noexcept
{
    if (this != &other)
    {
        if (m_stmt != nullptr)
        {
            sqlite3_finalize(m_stmt);
        }
        m_stmt = std::exchange(other.m_stmt, nullptr);
    }
    return *this;
}

qiven::Result<void> JournalStmt::bind(int index, i64 value)
{
    if (sqlite3_bind_int64(m_stmt, index, static_cast<sqlite3_int64>(value)) != SQLITE_OK)
    {
        return qiven::Result<void>::fail(err_constraint("journal-stmt-bind-int"));
    }
    return ok();
}

qiven::Result<void> JournalStmt::bind(int index, std::string_view value)
{
    // An empty string_view may carry a null data pointer; SQLite would
    // bind SQL NULL instead of empty text. Bind a literal empty string.
    const char* data = value.data() != nullptr ? value.data() : "";
    if (sqlite3_bind_text(m_stmt, index, data, static_cast<int>(value.size()),
                          SQLITE_TRANSIENT) != SQLITE_OK)
    {
        return qiven::Result<void>::fail(err_constraint("journal-stmt-bind-text"));
    }
    return ok();
}

qiven::Result<void> JournalStmt::bind(int index, std::span<const std::byte> value)
{
    if (sqlite3_bind_blob(m_stmt, index, value.data(), static_cast<int>(value.size()),
                          SQLITE_TRANSIENT) != SQLITE_OK)
    {
        return qiven::Result<void>::fail(err_constraint("journal-stmt-bind-blob"));
    }
    return ok();
}

qiven::Result<void> JournalStmt::bind_null(int index)
{
    if (sqlite3_bind_null(m_stmt, index) != SQLITE_OK)
    {
        return qiven::Result<void>::fail(err_constraint("journal-stmt-bind-null"));
    }
    return ok();
}

qiven::Result<bool> JournalStmt::step()
{
    const int rc = sqlite3_step(m_stmt);
    if (rc == SQLITE_ROW)
    {
        return true;
    }
    if (rc == SQLITE_DONE)
    {
        return false;
    }
    return qiven::Result<bool>::fail(
        err_open("journal-stmt-step: " + std::string(sqlite3_errstr(rc))));
}

i64 JournalStmt::col_i64(int column) const noexcept
{
    return static_cast<i64>(sqlite3_column_int64(m_stmt, column));
}

std::string_view JournalStmt::col_text(int column) const noexcept
{
    const auto* text = sqlite3_column_text(m_stmt, column);
    if (text == nullptr)
    {
        return {};
    }
    return { reinterpret_cast<const char*>(text),
             static_cast<usize>(sqlite3_column_bytes(m_stmt, column)) };
}

std::span<const std::byte> JournalStmt::col_blob(int column) const noexcept
{
    const int size = sqlite3_column_bytes(m_stmt, column);
    if (size <= 0)
    {
        return {};
    }
    return { static_cast<const std::byte*>(sqlite3_column_blob(m_stmt, column)),
             static_cast<usize>(size) };
}

bool JournalStmt::col_is_null(int column) const noexcept
{
    return sqlite3_column_type(m_stmt, column) == SQLITE_NULL;
}

void JournalStmt::reset() noexcept
{
    sqlite3_reset(m_stmt);
    sqlite3_clear_bindings(m_stmt);
}

bool JournalStmt::valid() const noexcept
{
    return m_stmt != nullptr;
}

// ===========================================================================
// JournalDb
// ===========================================================================
JournalDb::JournalDb(sqlite3* db) noexcept :
m_db(db)
{
}

JournalDb::~JournalDb()
{
    if (m_db != nullptr)
    {
        sqlite3_close_v2(m_db);
    }
}

JournalDb::JournalDb(JournalDb&& other) noexcept :
m_db(std::exchange(other.m_db, nullptr))
{
}

JournalDb& JournalDb::operator=(JournalDb&& other) noexcept
{
    if (this != &other)
    {
        if (m_db != nullptr)
        {
            sqlite3_close_v2(m_db);
        }
        m_db = std::exchange(other.m_db, nullptr);
    }
    return *this;
}

bool JournalDb::is_open() const noexcept
{
    return m_db != nullptr;
}

i64 JournalDb::last_insert_rowid() const noexcept
{
    return m_db != nullptr ? static_cast<i64>(sqlite3_last_insert_rowid(m_db)) : 0;
}

i64 JournalDb::changes() const noexcept
{
    return m_db != nullptr ? static_cast<i64>(sqlite3_changes64(m_db)) : 0;
}

qiven::Result<void> JournalDb::exec(std::string_view sql)
{
    char* error = nullptr;
    const std::string sql_text(sql);
    const int rc = sqlite3_exec(m_db, sql_text.c_str(), nullptr, nullptr, &error);
    if (rc != SQLITE_OK)
    {
        const std::string message =
            error != nullptr ? std::string(error) : std::string(sqlite3_errstr(rc));
        sqlite3_free(error);
        return qiven::Result<void>::fail(err_open("journal-exec: " + message));
    }
    return ok();
}

qiven::Result<JournalStmt> JournalDb::prepare(std::string_view sql)
{
    sqlite3_stmt* stmt = nullptr;
    const std::string sql_text(sql);
    if (sqlite3_prepare_v2(m_db, sql_text.c_str(), -1, &stmt, nullptr) != SQLITE_OK ||
        stmt == nullptr)
    {
        return qiven::Result<JournalStmt>::fail(
            err_open("journal-prepare: " + std::string(sqlite3_errmsg(m_db))));
    }
    return JournalStmt(stmt);
}

qiven::Result<void> JournalDb::txn(const std::function<qiven::Result<void>()>& body)
{
    if (auto begin = exec("BEGIN IMMEDIATE"); !begin.is_ok())
    {
        // Contention is a defect under single-writer discipline, not a
        // wait (busy_timeout is bounded at 1500 ms, cpp-design §7.1).
        return failed<void>(begin);
    }
    auto result = body();
    if (!result.is_ok())
    {
        exec("ROLLBACK");
        return result;
    }
    if (auto commit = exec("COMMIT"); !commit.is_ok())
    {
        exec("ROLLBACK");
        return failed<void>(commit);
    }
    return ok();
}

qiven::Result<bool> JournalDb::quick_check()
{
    auto stmt = prepare("PRAGMA quick_check");
    if (!stmt.is_ok())
    {
        return qiven::Result<bool>::fail(stmt.reason());
    }
    auto row = stmt.value().step();
    if (!row.is_ok())
    {
        return qiven::Result<bool>::fail(row.reason());
    }
    if (!row.value())
    {
        return qiven::Result<bool>::fail(err_open("journal-quick-check: no result row"));
    }
    return stmt.value().col_text(0) == "ok";
}

qiven::Result<void> JournalDb::checkpoint_wal()
{
    return exec("PRAGMA wal_checkpoint(TRUNCATE)");
}

qiven::Result<JournalDb> JournalDb::open(const std::filesystem::path& file,
                                         JournalOpenIntent intent, u64 now_ms)
{
    std::error_code ec;
    const bool exists = std::filesystem::exists(file, ec);
    if (ec)
    {
        return qiven::Result<JournalDb>::fail(err_open("journal-path-check: " + ec.message()));
    }

    // Fresh-database corruption guard (batch design §3.1): CreateNew never
    // overwrites (a zero-byte file is a truncation artifact, not absence);
    // OpenExisting never creates and never adopts an empty file.
    if (intent == JournalOpenIntent::CreateNew && exists)
    {
        return qiven::Result<JournalDb>::fail(err_open("journal-create-refuses-existing-path"));
    }
    if (intent == JournalOpenIntent::OpenExisting && !exists)
    {
        return qiven::Result<JournalDb>::fail(err_open("journal-file-missing"));
    }
    if (intent == JournalOpenIntent::OpenExisting)
    {
        const auto size = std::filesystem::file_size(file, ec);
        if (ec || size == 0)
        {
            return qiven::Result<JournalDb>::fail(err_open("journal-zero-byte-file"));
        }
    }

    const int flags               = intent == JournalOpenIntent::CreateNew
                                        ? (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE)
                                        : SQLITE_OPEN_READWRITE;
    sqlite3* raw                  = nullptr;
    const std::string path_string = file.string();
    if (sqlite3_open_v2(path_string.c_str(), &raw, flags, nullptr) != SQLITE_OK)
    {
        const std::string message =
            raw != nullptr ? std::string(sqlite3_errmsg(raw)) : "sqlite-open-failed";
        if (raw != nullptr)
        {
            sqlite3_close_v2(raw);
        }
        return qiven::Result<JournalDb>::fail(err_open("journal-open: " + message));
    }
    JournalDb db(raw);

    if (auto pragmas = db.exec(open_time_pragmas()); !pragmas.is_ok())
    {
        return failed<JournalDb>(pragmas);
    }
    if (auto check = db.quick_check(); !check.is_ok())
    {
        return qiven::Result<JournalDb>::fail(check.reason());
    }

    if (intent == JournalOpenIntent::CreateNew)
    {
        auto populate = db.txn([&]() -> qiven::Result<void> {
            if (auto ddl = db.exec(schema_v1_ddl()); !ddl.is_ok())
            {
                return failed<void>(ddl);
            }

            std::array<std::byte, 16> install_bytes {};
            auth::csrandom_fill(install_bytes);
            const std::string install = to_hex({ install_bytes.data(), install_bytes.size() });

            if (auto meta = write_meta(db, meta_schema_version,
                                       std::to_string(current_schema_version));
                !meta.is_ok())
            {
                return failed<void>(meta);
            }
            if (auto meta = write_meta(db, meta_install_id, install); !meta.is_ok())
            {
                return failed<void>(meta);
            }
            if (auto meta = write_meta(db, meta_boot_epoch, "0"); !meta.is_ok())
            {
                return failed<void>(meta);
            }
            if (auto meta = write_meta(db, "quarantine", "0"); !meta.is_ok())
            {
                return failed<void>(meta);
            }
            if (auto meta = write_meta(db, "last_wall_ms", std::to_string(now_ms));
                !meta.is_ok())
            {
                return failed<void>(meta);
            }

            std::vector<std::byte> payload;
            put_str(payload, install);
            put_u64(payload, static_cast<u64>(current_schema_version));
            put_u64(payload, now_ms);
            auto genesis = append_audit_row(db, kind_journal_created, payload);
            if (!genesis.is_ok())
            {
                return failed<void>(genesis);
            }
            return write_meta(db, "genesis_hash",
                              to_hex({ genesis.value().data(), genesis.value().size() }));
        });
        if (!populate.is_ok())
        {
            return failed<JournalDb>(populate);
        }
        return db;
    }

    // OpenExisting: validate that this file IS our journal — never adopt a
    // foreign or future database (ARCH §10.4 / exit gate 4).
    {
        auto table = db.prepare(
            "SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' AND name = "
            "'runtime_meta'");
        if (!table.is_ok())
        {
            return failed<JournalDb>(table);
        }
        auto row = table.value().step();
        if (!row.is_ok() || !row.value())
        {
            return qiven::Result<JournalDb>::fail(
                row.is_ok() ? err_open("journal-catalog-unreadable") : row.reason());
        }
        if (table.value().col_i64(0) == 0)
        {
            return qiven::Result<JournalDb>::fail(
                err_migration("journal-schema-missing (not a qiven journal)"));
        }
    }
    auto version = read_meta(db, meta_schema_version);
    if (!version.is_ok())
    {
        return qiven::Result<JournalDb>::fail(version.reason());
    }
    if (!version.value().has_value())
    {
        return qiven::Result<JournalDb>::fail(
            err_migration("journal-schema-version-missing (not a qiven journal)"));
    }
    if (*version.value() != std::to_string(current_schema_version))
    {
        return qiven::Result<JournalDb>::fail(
            err_migration("journal-schema-version-unknown: " + *version.value()));
    }
    for (const std::string_view key :
         { std::string_view(meta_install_id), std::string_view("boot_epoch"),
           std::string_view("quarantine"), std::string_view("last_wall_ms"),
           std::string_view("genesis_hash") })
    {
        auto value = read_meta(db, key);
        if (!value.is_ok())
        {
            return qiven::Result<JournalDb>::fail(value.reason());
        }
        if (!value.value().has_value())
        {
            return qiven::Result<JournalDb>::fail(err_open("journal-meta-missing: " +
                                                           std::string(key)));
        }
    }
    return db;
}

// ===========================================================================
// RuntimeJournal
// ===========================================================================
RuntimeJournal::RuntimeJournal(JournalDb&& db, std::string install_id_value,
                               u64 boot_epoch_value) :
m_db(std::move(db)),
m_install_id(std::move(install_id_value)),
m_boot_epoch(boot_epoch_value)
{
}

qiven::Result<std::unique_ptr<RuntimeJournal>> RuntimeJournal::open(
    const std::filesystem::path& file, JournalOpenIntent intent, u64 now_ms)
{
    auto db = JournalDb::open(file, intent, now_ms);
    if (!db.is_ok())
    {
        return qiven::Result<std::unique_ptr<RuntimeJournal>>::fail(db.reason());
    }
    auto install = read_meta(db.value(), meta_install_id);
    if (!install.is_ok())
    {
        return qiven::Result<std::unique_ptr<RuntimeJournal>>::fail(install.reason());
    }
    if (!install.value().has_value())
    {
        return qiven::Result<std::unique_ptr<RuntimeJournal>>::fail(
            err_open("journal-install-id-missing"));
    }
    auto epoch = read_meta(db.value(), meta_boot_epoch);
    if (!epoch.is_ok())
    {
        return qiven::Result<std::unique_ptr<RuntimeJournal>>::fail(epoch.reason());
    }
    u64 boot_epoch_value = no_boot_epoch;
    if (!epoch.value().has_value() ||
        !parse_u64(*epoch.value(), boot_epoch_value))
    {
        return qiven::Result<std::unique_ptr<RuntimeJournal>>::fail(
            err_open("journal-boot-epoch-unreadable"));
    }
    return std::unique_ptr<RuntimeJournal>(new RuntimeJournal(
        std::move(db.value()), *install.value(), boot_epoch_value));
}

qiven::Result<void> RuntimeJournal::deny_if_quarantined()
{
    auto flag = read_meta(m_db, "quarantine");
    if (!flag.is_ok())
    {
        return failed<void>(flag);
    }
    if (flag.value().has_value() && *flag.value() == "1")
    {
        m_quarantined = true;
        return qiven::Result<void>::fail(
            err_chain("journal-quarantined (mutation denied; diagnostics remain)"));
    }
    m_quarantined = false;
    return ok();
}

qiven::Result<void> RuntimeJournal::check_clock(u64 now_ms)
{
    auto last = read_meta(m_db, "last_wall_ms");
    if (!last.is_ok())
    {
        return failed<void>(last);
    }
    u64 last_ms = 0;
    if (last.value().has_value() && parse_u64(*last.value(), last_ms))
    {
        if (now_ms + 300000 < last_ms)
        {
            return qiven::Result<void>::fail(
                err_clock("journal-clock-regression (now < last journal event - 300s)"));
        }
    }
    return ok();
}

qiven::Result<void> RuntimeJournal::touch_wall_clock(u64 now_ms)
{
    auto last = read_meta(m_db, "last_wall_ms");
    if (!last.is_ok())
    {
        return failed<void>(last);
    }
    u64 effective = now_ms;
    u64 last_ms   = 0;
    if (last.value().has_value() && parse_u64(*last.value(), last_ms) && last_ms > effective)
    {
        effective = last_ms; // never move the watermark backwards
    }
    return write_meta(m_db, "last_wall_ms", std::to_string(effective));
}

qiven::Result<void> RuntimeJournal::append_audit(std::string_view kind,
                                                 const std::vector<std::byte>& payload)
{
    auto appended = append_audit_row(m_db, kind, payload);
    if (!appended.is_ok())
    {
        return failed<void>(appended);
    }
    return ok();
}

void RuntimeJournal::set_inspector(std::unique_ptr<IReconciliationInspector> inspector)
{
    m_inspector =
        inspector != nullptr ? std::move(inspector) : std::make_unique<FailClosedInspector>();
}

// --- commands ---------------------------------------------------------------

qiven::Result<u64> RuntimeJournal::open_session(const SessionOpen& session, u64 now_ms)
{
    if (auto prelude = deny_if_quarantined(); !prelude.is_ok())
    {
        return failed<u64>(prelude);
    }
    if (auto clock = check_clock(now_ms); !clock.is_ok())
    {
        return failed<u64>(clock);
    }

    u64 row_id = 0;
    auto txn   = m_db.txn([&]() -> qiven::Result<void> {
        auto active =
            m_db.prepare("SELECT id FROM generations WHERE id = ? AND status = 'active'");
        if (!active.is_ok())
        {
            return failed<void>(active);
        }
        if (auto bind = active.value().bind(1, static_cast<i64>(session.generation.value));
            !bind.is_ok())
        {
            return failed<void>(bind);
        }
        auto row = active.value().step();
        if (!row.is_ok())
        {
            return failed<void>(row);
        }
        if (!row.value())
        {
            return qiven::Result<void>::fail(err_constraint("session-generation-not-active"));
        }

        auto insert = m_db.prepare(
            "INSERT INTO sessions(generation, actor, harness, opened_ms) VALUES(?, ?, ?, ?)");
        if (!insert.is_ok())
        {
            return failed<void>(insert);
        }
        if (auto bind = insert.value().bind(1, static_cast<i64>(session.generation.value));
            !bind.is_ok())
        {
            return failed<void>(bind);
        }
        if (session.actor.has_value())
        {
            if (auto bind = insert.value().bind(2, static_cast<i64>(*session.actor));
                !bind.is_ok())
            {
                return failed<void>(bind);
            }
        }
        else if (auto bind = insert.value().bind_null(2); !bind.is_ok())
        {
            return failed<void>(bind);
        }
        if (auto bind = insert.value().bind(3, session.harness); !bind.is_ok())
        {
            return failed<void>(bind);
        }
        if (auto bind = insert.value().bind(4, static_cast<i64>(now_ms)); !bind.is_ok())
        {
            return failed<void>(bind);
        }
        auto inserted = insert.value().step();
        if (!inserted.is_ok())
        {
            return failed<void>(inserted);
        }
        row_id = static_cast<u64>(m_db.last_insert_rowid());

        std::vector<std::byte> payload;
        put_u64(payload, row_id);
        put_u64(payload, session.generation.value);
        put_u64(payload, session.actor.value_or(0));
        put_str(payload, session.harness);
        put_u64(payload, now_ms);
        if (auto audit = append_audit(kind_session_opened, payload); !audit.is_ok())
        {
            return failed<void>(audit);
        }
        return touch_wall_clock(now_ms);
    });
    if (!txn.is_ok())
    {
        return failed<u64>(txn);
    }
    return row_id;
}

qiven::Result<ControlTransactionId> RuntimeJournal::open_transaction(const TransactionOpen& open,
                                                                     u64 now_ms)
{
    if (auto prelude = deny_if_quarantined(); !prelude.is_ok())
    {
        return failed<ControlTransactionId>(prelude);
    }
    if (auto clock = check_clock(now_ms); !clock.is_ok())
    {
        return failed<ControlTransactionId>(clock);
    }

    ControlTransactionId created { 0 };
    auto txn = m_db.txn([&]() -> qiven::Result<void> {
        auto next = m_db.prepare("SELECT COALESCE(MAX(id), 0) + 1 FROM transactions");
        if (!next.is_ok())
        {
            return failed<void>(next);
        }
        auto row = next.value().step();
        if (!row.is_ok())
        {
            return failed<void>(row);
        }
        if (!row.value())
        {
            return qiven::Result<void>::fail(err_open("journal-next-tx-unavailable"));
        }
        created.value = static_cast<u64>(next.value().col_i64(0));

        auto insert = m_db.prepare(
            "INSERT INTO transactions(id, boot_epoch, causal_parent, correlation, "
            "request_digest, base_revision, state) VALUES(?, ?, ?, ?, ?, ?, 'observed')");
        if (!insert.is_ok())
        {
            return failed<void>(insert);
        }
        if (auto bind = insert.value().bind(1, static_cast<i64>(created.value));
            !bind.is_ok())
        {
            return failed<void>(bind);
        }
        if (auto bind = insert.value().bind(2, static_cast<i64>(m_boot_epoch));
            !bind.is_ok())
        {
            return failed<void>(bind);
        }
        if (open.causal_parent.has_value())
        {
            if (auto bind = insert.value().bind(3, static_cast<i64>(open.causal_parent->value));
                !bind.is_ok())
            {
                return failed<void>(bind);
            }
        }
        else if (auto bind = insert.value().bind_null(3); !bind.is_ok())
        {
            return failed<void>(bind);
        }
        if (auto bind = insert.value().bind(4, open.correlation); !bind.is_ok())
        {
            return failed<void>(bind);
        }
        if (auto bind = insert.value().bind(5, digest_bytes(open.request_digest));
            !bind.is_ok())
        {
            return failed<void>(bind);
        }
        if (open.base_revision.has_value())
        {
            if (auto bind = insert.value().bind(6, *open.base_revision); !bind.is_ok())
            {
                return failed<void>(bind);
            }
        }
        else if (auto bind = insert.value().bind_null(6); !bind.is_ok())
        {
            return failed<void>(bind);
        }
        auto inserted = insert.value().step();
        if (!inserted.is_ok())
        {
            return failed<void>(inserted);
        }

        std::vector<std::byte> payload;
        put_u64(payload, created.value);
        put_u64(payload, m_boot_epoch);
        if (open.causal_parent.has_value())
        {
            put_u64(payload, 1);
            put_u64(payload, open.causal_parent->value);
        }
        else
        {
            put_u64(payload, 0);
        }
        put_str(payload, open.correlation);
        put_digest(payload, open.request_digest);
        if (open.base_revision.has_value())
        {
            put_u64(payload, 1);
            put_str(payload, *open.base_revision);
        }
        else
        {
            put_u64(payload, 0);
        }
        if (auto audit = append_audit(kind_transaction_opened, payload); !audit.is_ok())
        {
            return failed<void>(audit);
        }
        if (auto wall = touch_wall_clock(now_ms); !wall.is_ok())
        {
            return failed<void>(wall);
        }
        crash::trigger(crash::CrashPoint::after_create_tx, crash::Phase::before_commit);
        return ok();
    });
    if (!txn.is_ok())
    {
        return failed<ControlTransactionId>(txn);
    }
    crash::trigger(crash::CrashPoint::after_create_tx, crash::Phase::after_commit);
    return created;
}

qiven::Result<void> RuntimeJournal::accept_evidence(ControlTransactionId transaction,
                                                    std::span<const ContentDigest> evidence,
                                                    u64 now_ms)
{
    if (auto prelude = deny_if_quarantined(); !prelude.is_ok())
    {
        return failed<void>(prelude);
    }
    if (auto clock = check_clock(now_ms); !clock.is_ok())
    {
        return failed<void>(clock);
    }

    auto txn = m_db.txn([&]() -> qiven::Result<void> {
        auto state = read_state_by_i64(m_db, "SELECT state FROM transactions WHERE id = ?",
                                       static_cast<i64>(transaction.value));
        if (!state.is_ok())
        {
            return failed<void>(state);
        }
        if (state.value() != tx_state::observed && state.value() != tx_state::judging)
        {
            return qiven::Result<void>::fail(
                err_constraint("evidence-tx-state-invalid: " + state.value()));
        }

        std::vector<std::byte> payload;
        put_u64(payload, transaction.value);
        put_u64(payload, static_cast<u64>(evidence.size()));
        for (const ContentDigest& digest : evidence)
        {
            put_digest(payload, digest);
        }
        put_u64(payload, now_ms);
        if (auto audit = append_audit(kind_evidence_accepted, payload); !audit.is_ok())
        {
            return failed<void>(audit);
        }
        if (auto wall = touch_wall_clock(now_ms); !wall.is_ok())
        {
            return failed<void>(wall);
        }
        crash::trigger(crash::CrashPoint::after_evidence_persist, crash::Phase::before_commit);
        return ok();
    });
    if (!txn.is_ok())
    {
        return txn;
    }
    crash::trigger(crash::CrashPoint::after_evidence_persist, crash::Phase::after_commit);
    return ok();
}

qiven::Result<void> RuntimeJournal::bind_decision(const DecisionBind& decision, u64 now_ms)
{
    if (auto prelude = deny_if_quarantined(); !prelude.is_ok())
    {
        return failed<void>(prelude);
    }
    if (auto clock = check_clock(now_ms); !clock.is_ok())
    {
        return failed<void>(clock);
    }

    auto txn = m_db.txn([&]() -> qiven::Result<void> {
        auto state = read_state_by_i64(m_db, "SELECT state FROM transactions WHERE id = ?",
                                       static_cast<i64>(decision.transaction.value));
        if (!state.is_ok())
        {
            return failed<void>(state);
        }
        if (state.value() != tx_state::observed && state.value() != tx_state::judging)
        {
            return qiven::Result<void>::fail(
                err_constraint("decision-tx-state-invalid: " + state.value()));
        }

        auto insert = m_db.prepare(
            "INSERT INTO decisions(id, token_hash, \"transaction\", generation, "
            "binding_digest, expires_ms, state, consumed_ms) "
            "VALUES(?, ?, ?, ?, ?, ?, 'bound', NULL)");
        if (!insert.is_ok())
        {
            return failed<void>(insert);
        }
        if (auto bind = insert.value().bind(1, id_bytes(decision.id)); !bind.is_ok())
        {
            return failed<void>(bind);
        }
        if (auto bind = insert.value().bind(2, digest_bytes(
                                                   ContentDigest { decision.token_hash.value }));
            !bind.is_ok())
        {
            return failed<void>(bind);
        }
        if (auto bind = insert.value().bind(3, static_cast<i64>(decision.transaction.value));
            !bind.is_ok())
        {
            return failed<void>(bind);
        }
        if (auto bind = insert.value().bind(4, static_cast<i64>(decision.generation.value));
            !bind.is_ok())
        {
            return failed<void>(bind);
        }
        if (auto bind = insert.value().bind(5, digest_bytes(decision.binding_digest));
            !bind.is_ok())
        {
            return failed<void>(bind);
        }
        if (auto bind = insert.value().bind(6, static_cast<i64>(decision.expires_ms));
            !bind.is_ok())
        {
            return failed<void>(bind);
        }
        auto inserted = insert.value().step();
        if (!inserted.is_ok())
        {
            return failed<void>(inserted);
        }

        // Terminal states never regress; the guarded UPDATE is the law.
        auto advance = m_db.prepare(
            "UPDATE transactions SET state = 'decided' WHERE id = ? AND state IN "
            "('observed', 'judging')");
        if (!advance.is_ok())
        {
            return failed<void>(advance);
        }
        if (auto bind = advance.value().bind(1, static_cast<i64>(decision.transaction.value));
            !bind.is_ok())
        {
            return failed<void>(bind);
        }
        auto row = advance.value().step();
        if (!row.is_ok())
        {
            return failed<void>(row);
        }

        std::vector<std::byte> payload;
        put_id(payload, decision.id);
        put_digest(payload, ContentDigest { decision.token_hash.value });
        put_u64(payload, decision.transaction.value);
        put_u64(payload, decision.generation.value);
        put_digest(payload, decision.binding_digest);
        put_u64(payload, decision.expires_ms);
        put_u64(payload, now_ms);
        if (auto audit = append_audit(kind_decision_bound, payload); !audit.is_ok())
        {
            return failed<void>(audit);
        }
        if (auto wall = touch_wall_clock(now_ms); !wall.is_ok())
        {
            return failed<void>(wall);
        }
        crash::trigger(crash::CrashPoint::after_decision_persist, crash::Phase::before_commit);
        return ok();
    });
    if (!txn.is_ok())
    {
        return txn;
    }
    crash::trigger(crash::CrashPoint::after_decision_persist, crash::Phase::after_commit);
    return ok();
}

qiven::Result<void> RuntimeJournal::consume_decision(const DecisionId& id, u64 now_ms)
{
    if (auto prelude = deny_if_quarantined(); !prelude.is_ok())
    {
        return failed<void>(prelude);
    }
    if (auto clock = check_clock(now_ms); !clock.is_ok())
    {
        return failed<void>(clock);
    }

    auto txn = m_db.txn([&]() -> qiven::Result<void> {
        // Pre-read gives a precise denial reason; the guarded UPDATE below
        // is the actual single-consumption enforcement (storage layer).
        auto current = m_db.prepare("SELECT state, expires_ms FROM decisions WHERE id = ?");
        if (!current.is_ok())
        {
            return failed<void>(current);
        }
        if (auto bind = current.value().bind(1, id_bytes(id)); !bind.is_ok())
        {
            return failed<void>(bind);
        }
        auto row = current.value().step();
        if (!row.is_ok())
        {
            return failed<void>(row);
        }
        if (!row.value())
        {
            return qiven::Result<void>::fail(err_constraint("decision-unknown"));
        }
        const std::string_view state = current.value().col_text(0);
        const u64 expires            = static_cast<u64>(current.value().col_i64(1));
        if (state == decision_state::consumed)
        {
            return qiven::Result<void>::fail(err_constraint("decision-already-consumed"));
        }
        if (state == decision_state::stale)
        {
            return qiven::Result<void>::fail(err_constraint("decision-stale"));
        }
        if (now_ms > expires)
        {
            return qiven::Result<void>::fail(err_constraint("decision-expired"));
        }

        auto consume = m_db.prepare(
            "UPDATE decisions SET state = 'consumed', consumed_ms = ? "
            "WHERE id = ? AND state = 'bound'");
        if (!consume.is_ok())
        {
            return failed<void>(consume);
        }
        if (auto bind = consume.value().bind(1, static_cast<i64>(now_ms)); !bind.is_ok())
        {
            return failed<void>(bind);
        }
        if (auto bind = consume.value().bind(2, id_bytes(id)); !bind.is_ok())
        {
            return failed<void>(bind);
        }
        auto updated = consume.value().step();
        if (!updated.is_ok())
        {
            return failed<void>(updated);
        }

        std::vector<std::byte> payload;
        put_id(payload, id);
        put_u64(payload, now_ms);
        if (auto audit = append_audit(kind_decision_consumed, payload); !audit.is_ok())
        {
            return failed<void>(audit);
        }
        if (auto wall = touch_wall_clock(now_ms); !wall.is_ok())
        {
            return failed<void>(wall);
        }
        crash::trigger(crash::CrashPoint::after_token_consume, crash::Phase::before_commit);
        return ok();
    });
    if (!txn.is_ok())
    {
        return txn;
    }
    crash::trigger(crash::CrashPoint::after_token_consume, crash::Phase::after_commit);
    return ok();
}

qiven::Result<LeaseState> RuntimeJournal::acquire_lease(const LeaseRequest& request, u64 now_ms)
{
    if (auto prelude = deny_if_quarantined(); !prelude.is_ok())
    {
        return failed<LeaseState>(prelude);
    }
    if (auto clock = check_clock(now_ms); !clock.is_ok())
    {
        return failed<LeaseState>(clock);
    }

    LeaseState acquired;
    auto txn = m_db.txn([&]() -> qiven::Result<void> {
        auto existing = m_db.prepare(
            "SELECT holder_install, boot_epoch, fencing_epoch, expires_ms FROM leases "
            "WHERE workspace = ?");
        if (!existing.is_ok())
        {
            return failed<void>(existing);
        }
        if (auto bind = existing.value().bind(1, request.workspace); !bind.is_ok())
        {
            return failed<void>(bind);
        }
        auto row = existing.value().step();
        if (!row.is_ok())
        {
            return failed<void>(row);
        }

        u64 new_epoch = 1; // first acquire: 0 -> 1
        if (row.value())
        {
            const std::string_view holder = existing.value().col_text(0);
            const u64 row_expires         = static_cast<u64>(existing.value().col_i64(3));
            const u64 old_epoch           = static_cast<u64>(existing.value().col_i64(2));
            if (row_expires >= now_ms && holder != request.holder_install)
            {
                return qiven::Result<void>::fail(err_constraint("lease-held-by-another-holder"));
            }
            // The row is never deleted: fencing_epoch is the monotonic
            // high-water mark (batch design §1 correction 2).
            new_epoch = old_epoch + 1;
        }

        auto upsert = m_db.prepare(
            "INSERT INTO leases(workspace, holder_install, boot_epoch, fencing_epoch, "
            "acquired_ms, expires_ms) VALUES(?, ?, ?, ?, ?, ?) "
            "ON CONFLICT(workspace) DO UPDATE SET holder_install = "
            "excluded.holder_install, boot_epoch = excluded.boot_epoch, fencing_epoch = "
            "excluded.fencing_epoch, acquired_ms = excluded.acquired_ms, expires_ms = "
            "excluded.expires_ms");
        if (!upsert.is_ok())
        {
            return failed<void>(upsert);
        }
        if (auto bind = upsert.value().bind(1, request.workspace); !bind.is_ok())
        {
            return failed<void>(bind);
        }
        if (auto bind = upsert.value().bind(2, request.holder_install); !bind.is_ok())
        {
            return failed<void>(bind);
        }
        if (auto bind = upsert.value().bind(3, static_cast<i64>(m_boot_epoch)); !bind.is_ok())
        {
            return failed<void>(bind);
        }
        if (auto bind = upsert.value().bind(4, static_cast<i64>(new_epoch)); !bind.is_ok())
        {
            return failed<void>(bind);
        }
        if (auto bind = upsert.value().bind(5, static_cast<i64>(now_ms)); !bind.is_ok())
        {
            return failed<void>(bind);
        }
        if (auto bind = upsert.value().bind(6, static_cast<i64>(now_ms + request.ttl_ms));
            !bind.is_ok())
        {
            return failed<void>(bind);
        }
        auto updated = upsert.value().step();
        if (!updated.is_ok())
        {
            return failed<void>(updated);
        }

        acquired = LeaseState { request.workspace, request.holder_install, m_boot_epoch,
                                new_epoch, now_ms, now_ms + request.ttl_ms };

        std::vector<std::byte> payload;
        put_str(payload, request.workspace);
        put_str(payload, request.holder_install);
        put_u64(payload, m_boot_epoch);
        put_u64(payload, new_epoch);
        put_u64(payload, now_ms + request.ttl_ms);
        if (auto audit = append_audit(kind_lease_acquired, payload); !audit.is_ok())
        {
            return failed<void>(audit);
        }
        if (auto wall = touch_wall_clock(now_ms); !wall.is_ok())
        {
            return failed<void>(wall);
        }
        crash::trigger(crash::CrashPoint::after_lease_fence, crash::Phase::before_commit);
        return ok();
    });
    if (!txn.is_ok())
    {
        return failed<LeaseState>(txn);
    }
    crash::trigger(crash::CrashPoint::after_lease_fence, crash::Phase::after_commit);
    return acquired;
}

qiven::Result<void> RuntimeJournal::record_dispatch_prepared(const DispatchPrepared& dispatch,
                                                             u64 now_ms)
{
    if (auto prelude = deny_if_quarantined(); !prelude.is_ok())
    {
        return failed<void>(prelude);
    }
    if (auto clock = check_clock(now_ms); !clock.is_ok())
    {
        return failed<void>(clock);
    }

    auto txn = m_db.txn([&]() -> qiven::Result<void> {
        auto state = read_state_by_i64(m_db, "SELECT state FROM transactions WHERE id = ?",
                                       static_cast<i64>(dispatch.transaction.value));
        if (!state.is_ok())
        {
            return failed<void>(state);
        }
        if (state.value() != tx_state::decided)
        {
            return qiven::Result<void>::fail(
                err_constraint("dispatch-tx-state-invalid: " + state.value()));
        }

        auto insert = m_db.prepare(
            "INSERT INTO dispatches(id, \"transaction\", plan_digest, state, started_ms) "
            "VALUES(?, ?, ?, 'prepared', ?)");
        if (!insert.is_ok())
        {
            return failed<void>(insert);
        }
        if (auto bind = insert.value().bind(1, id_bytes(dispatch.id)); !bind.is_ok())
        {
            return failed<void>(bind);
        }
        if (auto bind = insert.value().bind(2, static_cast<i64>(dispatch.transaction.value));
            !bind.is_ok())
        {
            return failed<void>(bind);
        }
        if (auto bind = insert.value().bind(3, digest_bytes(dispatch.plan_digest));
            !bind.is_ok())
        {
            return failed<void>(bind);
        }
        if (auto bind = insert.value().bind(4, static_cast<i64>(now_ms)); !bind.is_ok())
        {
            return failed<void>(bind);
        }
        auto inserted = insert.value().step();
        if (!inserted.is_ok())
        {
            return failed<void>(inserted);
        }

        auto advance = m_db.prepare(
            "UPDATE transactions SET state = 'prepared' WHERE id = ? AND state = 'decided'");
        if (!advance.is_ok())
        {
            return failed<void>(advance);
        }
        if (auto bind = advance.value().bind(1, static_cast<i64>(dispatch.transaction.value));
            !bind.is_ok())
        {
            return failed<void>(bind);
        }
        auto row = advance.value().step();
        if (!row.is_ok())
        {
            return failed<void>(row);
        }

        std::vector<std::byte> payload;
        put_id(payload, dispatch.id);
        put_u64(payload, dispatch.transaction.value);
        put_digest(payload, dispatch.plan_digest);
        put_u64(payload, now_ms);
        if (auto audit = append_audit(kind_dispatch_prepared, payload); !audit.is_ok())
        {
            return failed<void>(audit);
        }
        if (auto wall = touch_wall_clock(now_ms); !wall.is_ok())
        {
            return failed<void>(wall);
        }
        crash::trigger(crash::CrashPoint::after_dispatch_commit, crash::Phase::before_commit);
        return ok();
    });
    if (!txn.is_ok())
    {
        return txn;
    }
    crash::trigger(crash::CrashPoint::after_dispatch_commit, crash::Phase::after_commit);
    return ok();
}

qiven::Result<void> RuntimeJournal::record_outcome(const OutcomeRecord& outcome, u64 now_ms)
{
    if (auto prelude = deny_if_quarantined(); !prelude.is_ok())
    {
        return failed<void>(prelude);
    }
    if (auto clock = check_clock(now_ms); !clock.is_ok())
    {
        return failed<void>(clock);
    }
    if (!is_valid_outcome_status(outcome.status))
    {
        return qiven::Result<void>::fail(err_constraint("outcome-status-invalid: " +
                                                        outcome.status));
    }

    auto txn = m_db.txn([&]() -> qiven::Result<void> {
        auto state = read_state_by_blob(
            m_db, "SELECT state FROM dispatches WHERE id = ?", id_bytes(outcome.dispatch));
        if (!state.is_ok())
        {
            return failed<void>(state);
        }
        if (state.value() != dispatch_state::prepared)
        {
            return qiven::Result<void>::fail(
                err_constraint("outcome-dispatch-state-invalid: " + state.value()));
        }

        auto insert = m_db.prepare(
            "INSERT INTO outcomes(dispatch, status, ref_observed, validator_digest, "
            "observed_ms) VALUES(?, ?, ?, ?, ?)");
        if (!insert.is_ok())
        {
            return failed<void>(insert);
        }
        if (auto bind = insert.value().bind(1, id_bytes(outcome.dispatch)); !bind.is_ok())
        {
            return failed<void>(bind);
        }
        if (auto bind = insert.value().bind(2, outcome.status); !bind.is_ok())
        {
            return failed<void>(bind);
        }
        if (outcome.ref_observed.has_value())
        {
            if (auto bind = insert.value().bind(3, *outcome.ref_observed); !bind.is_ok())
            {
                return failed<void>(bind);
            }
        }
        else if (auto bind = insert.value().bind_null(3); !bind.is_ok())
        {
            return failed<void>(bind);
        }
        if (outcome.validator_digest.has_value())
        {
            if (auto bind = insert.value().bind(4, digest_bytes(*outcome.validator_digest));
                !bind.is_ok())
            {
                return failed<void>(bind);
            }
        }
        else if (auto bind = insert.value().bind_null(4); !bind.is_ok())
        {
            return failed<void>(bind);
        }
        if (auto bind = insert.value().bind(5, static_cast<i64>(now_ms)); !bind.is_ok())
        {
            return failed<void>(bind);
        }
        auto inserted = insert.value().step();
        if (!inserted.is_ok())
        {
            return failed<void>(inserted);
        }

        // One final observation per dispatch (outcomes PK) + guarded
        // terminal transitions that can never regress.
        auto close_dispatch = m_db.prepare(
            "UPDATE dispatches SET state = ?, done_ms = ? WHERE id = ? AND state = "
            "'prepared'");
        if (!close_dispatch.is_ok())
        {
            return failed<void>(close_dispatch);
        }
        if (auto bind = close_dispatch.value().bind(1, outcome.status); !bind.is_ok())
        {
            return failed<void>(bind);
        }
        if (auto bind = close_dispatch.value().bind(2, static_cast<i64>(now_ms));
            !bind.is_ok())
        {
            return failed<void>(bind);
        }
        if (auto bind = close_dispatch.value().bind(3, id_bytes(outcome.dispatch));
            !bind.is_ok())
        {
            return failed<void>(bind);
        }
        auto row = close_dispatch.value().step();
        if (!row.is_ok())
        {
            return failed<void>(row);
        }

        auto close_tx = m_db.prepare(
            "UPDATE transactions SET state = ? WHERE id = (SELECT \"transaction\" FROM "
            "dispatches WHERE id = ?) AND state = 'prepared'");
        if (!close_tx.is_ok())
        {
            return failed<void>(close_tx);
        }
        if (auto bind = close_tx.value().bind(1, outcome.status); !bind.is_ok())
        {
            return failed<void>(bind);
        }
        if (auto bind = close_tx.value().bind(2, id_bytes(outcome.dispatch)); !bind.is_ok())
        {
            return failed<void>(bind);
        }
        row = close_tx.value().step();
        if (!row.is_ok())
        {
            return failed<void>(row);
        }

        std::vector<std::byte> payload;
        put_id(payload, outcome.dispatch);
        put_str(payload, outcome.status);
        if (outcome.ref_observed.has_value())
        {
            put_u64(payload, 1);
            put_str(payload, *outcome.ref_observed);
        }
        else
        {
            put_u64(payload, 0);
        }
        put_u64(payload, now_ms);
        if (auto audit = append_audit(kind_outcome_observed, payload); !audit.is_ok())
        {
            return failed<void>(audit);
        }
        if (auto wall = touch_wall_clock(now_ms); !wall.is_ok())
        {
            return failed<void>(wall);
        }
        crash::trigger(crash::CrashPoint::after_outcome_commit, crash::Phase::before_commit);
        return ok();
    });
    if (!txn.is_ok())
    {
        return txn;
    }
    crash::trigger(crash::CrashPoint::after_outcome_commit, crash::Phase::after_commit);
    return ok();
}

qiven::Result<void> RuntimeJournal::open_barrier(const BarrierOpen& barrier, u64 now_ms)
{
    if (auto prelude = deny_if_quarantined(); !prelude.is_ok())
    {
        return failed<void>(prelude);
    }
    if (auto clock = check_clock(now_ms); !clock.is_ok())
    {
        return failed<void>(clock);
    }

    auto txn = m_db.txn([&]() -> qiven::Result<void> {
        auto insert = m_db.prepare(
            "INSERT INTO barriers(scope, reason, opened_ms, closed_ms, evidence) "
            "VALUES(?, ?, ?, NULL, ?)");
        if (!insert.is_ok())
        {
            return failed<void>(insert);
        }
        if (auto bind = insert.value().bind(1, barrier.scope); !bind.is_ok())
        {
            return failed<void>(bind);
        }
        if (auto bind = insert.value().bind(2, barrier.reason); !bind.is_ok())
        {
            return failed<void>(bind);
        }
        if (auto bind = insert.value().bind(3, static_cast<i64>(now_ms)); !bind.is_ok())
        {
            return failed<void>(bind);
        }
        if (auto bind = insert.value().bind(4, barrier.evidence); !bind.is_ok())
        {
            return failed<void>(bind);
        }
        auto inserted = insert.value().step();
        if (!inserted.is_ok())
        {
            return failed<void>(inserted);
        }

        std::vector<std::byte> payload;
        put_str(payload, barrier.scope);
        put_str(payload, barrier.reason);
        put_u64(payload, now_ms);
        if (auto audit = append_audit(kind_barrier_opened, payload); !audit.is_ok())
        {
            return failed<void>(audit);
        }
        return touch_wall_clock(now_ms);
    });
    if (!txn.is_ok())
    {
        return txn;
    }
    return ok();
}

qiven::Result<void> RuntimeJournal::close_barrier(std::string_view scope,
                                                  std::string_view evidence, u64 now_ms)
{
    if (auto prelude = deny_if_quarantined(); !prelude.is_ok())
    {
        return failed<void>(prelude);
    }
    if (auto clock = check_clock(now_ms); !clock.is_ok())
    {
        return failed<void>(clock);
    }

    auto txn = m_db.txn([&]() -> qiven::Result<void> {
        // Pre-check inside the transaction: closing requires an ACTIVE
        // barrier on this scope (a second close is a typed denial, and a
        // zero-row UPDATE must never read as success).
        auto active = m_db.prepare(
            "SELECT 1 FROM barriers WHERE scope = ? AND closed_ms IS NULL");
        if (!active.is_ok())
        {
            return failed<void>(active);
        }
        if (auto bind = active.value().bind(1, scope); !bind.is_ok())
        {
            return failed<void>(bind);
        }
        auto present = active.value().step();
        if (!present.is_ok())
        {
            return failed<void>(present);
        }
        if (!present.value())
        {
            return qiven::Result<void>::fail(err_constraint("barrier-not-active"));
        }

        auto close = m_db.prepare(
            "UPDATE barriers SET closed_ms = ? WHERE scope = ? AND closed_ms IS NULL");
        if (!close.is_ok())
        {
            return failed<void>(close);
        }
        if (auto bind = close.value().bind(1, static_cast<i64>(now_ms)); !bind.is_ok())
        {
            return failed<void>(bind);
        }
        if (auto bind = close.value().bind(2, scope); !bind.is_ok())
        {
            return failed<void>(bind);
        }
        auto row = close.value().step();
        if (!row.is_ok())
        {
            return failed<void>(row);
        }

        std::vector<std::byte> payload;
        put_str(payload, scope);
        put_str(payload, evidence);
        put_u64(payload, now_ms);
        if (auto audit = append_audit(kind_barrier_closed, payload); !audit.is_ok())
        {
            return failed<void>(audit);
        }
        return touch_wall_clock(now_ms);
    });
    if (!txn.is_ok())
    {
        return txn;
    }
    return ok();
}

qiven::Result<void> RuntimeJournal::advance_generation(RuntimeGenerationId id,
                                                       const ContentDigest& bundle_digest,
                                                       const ContentDigest& profile_digest,
                                                       std::string_view build_id, u64 now_ms)
{
    if (auto prelude = deny_if_quarantined(); !prelude.is_ok())
    {
        return failed<void>(prelude);
    }
    if (auto clock = check_clock(now_ms); !clock.is_ok())
    {
        return failed<void>(clock);
    }

    auto txn = m_db.txn([&]() -> qiven::Result<void> {
        auto retire =
            m_db.prepare("UPDATE generations SET status = 'retired' WHERE status = 'active'");
        if (!retire.is_ok())
        {
            return failed<void>(retire);
        }
        auto retired = retire.value().step();
        if (!retired.is_ok())
        {
            return failed<void>(retired);
        }

        auto insert = m_db.prepare(
            "INSERT INTO generations(id, bundle_digest, profile_digest, build_id, "
            "activated_ms, status) VALUES(?, ?, ?, ?, ?, 'active')");
        if (!insert.is_ok())
        {
            return failed<void>(insert);
        }
        if (auto bind = insert.value().bind(1, static_cast<i64>(id.value)); !bind.is_ok())
        {
            return failed<void>(bind);
        }
        if (auto bind = insert.value().bind(2, digest_bytes(bundle_digest)); !bind.is_ok())
        {
            return failed<void>(bind);
        }
        if (auto bind = insert.value().bind(3, digest_bytes(profile_digest)); !bind.is_ok())
        {
            return failed<void>(bind);
        }
        if (auto bind = insert.value().bind(4, build_id); !bind.is_ok())
        {
            return failed<void>(bind);
        }
        if (auto bind = insert.value().bind(5, static_cast<i64>(now_ms)); !bind.is_ok())
        {
            return failed<void>(bind);
        }
        auto inserted = insert.value().step();
        if (!inserted.is_ok())
        {
            return failed<void>(inserted);
        }

        std::vector<std::byte> payload;
        put_u64(payload, id.value);
        put_digest(payload, bundle_digest);
        put_digest(payload, profile_digest);
        put_str(payload, build_id);
        put_u64(payload, now_ms);

        // MVP-2 exit gate 4 (ARCH section 15): creating a new generation
        // invalidates every UNCONSUMED decision bound under an older
        // generation — same transaction as the activation. Consumed
        // decisions are untouched (their effects already happened; an
        // already-dispatched action stays attached to its original
        // generation, ARCH section 7.4). The count rides the audit payload
        // as an append-only field (chain verification rehashes stored
        // bytes, so pre-MVP-2 events verify unchanged).
        auto stale = m_db.prepare(
            "UPDATE decisions SET state = 'stale' WHERE state = 'bound' AND generation != ?");
        if (!stale.is_ok())
        {
            return failed<void>(stale);
        }
        if (auto bind = stale.value().bind(1, static_cast<i64>(id.value)); !bind.is_ok())
        {
            return failed<void>(bind);
        }
        auto staled = stale.value().step();
        if (!staled.is_ok())
        {
            return failed<void>(staled);
        }
        put_u64(payload, static_cast<u64>(m_db.changes()));

        if (auto audit = append_audit(kind_generation_advanced, payload); !audit.is_ok())
        {
            return failed<void>(audit);
        }
        return touch_wall_clock(now_ms);
    });
    if (!txn.is_ok())
    {
        return txn;
    }
    return ok();
}

// --- read surface -----------------------------------------------------------

qiven::Result<std::string> RuntimeJournal::tx_state_of(ControlTransactionId id)
{
    return read_state_by_i64(m_db, "SELECT state FROM transactions WHERE id = ?",
                             static_cast<i64>(id.value));
}

qiven::Result<std::string> RuntimeJournal::decision_state_of(const DecisionId& id)
{
    return read_state_by_blob(m_db, "SELECT state FROM decisions WHERE id = ?", id_bytes(id));
}

qiven::Result<std::string> RuntimeJournal::dispatch_state_of(const SortableId128& id)
{
    return read_state_by_blob(m_db, "SELECT state FROM dispatches WHERE id = ?", id_bytes(id));
}

qiven::Result<DecisionId> RuntimeJournal::first_decision_of_transaction(
    ControlTransactionId id)
{
    auto stmt = m_db.prepare(
        "SELECT id FROM decisions WHERE \"transaction\" = ? ORDER BY id LIMIT 1");
    if (!stmt.is_ok())
    {
        return qiven::Result<DecisionId>::fail(stmt.reason());
    }
    if (auto bind = stmt.value().bind(1, static_cast<i64>(id.value)); !bind.is_ok())
    {
        return qiven::Result<DecisionId>::fail(bind.reason());
    }
    auto row = stmt.value().step();
    if (!row.is_ok())
    {
        return qiven::Result<DecisionId>::fail(row.reason());
    }
    if (!row.value())
    {
        return qiven::Result<DecisionId>::fail(err_constraint("decision-unknown"));
    }
    const auto blob = stmt.value().col_blob(0);
    DecisionId out;
    if (blob.size() != out.bytes.size())
    {
        return qiven::Result<DecisionId>::fail(err_open("decision-id-corrupt"));
    }
    std::memcpy(out.bytes.data(), blob.data(), blob.size());
    return out;
}

qiven::Result<SortableId128> RuntimeJournal::first_dispatch_of_transaction(
    ControlTransactionId id)
{
    auto stmt = m_db.prepare(
        "SELECT id FROM dispatches WHERE \"transaction\" = ? ORDER BY id LIMIT 1");
    if (!stmt.is_ok())
    {
        return qiven::Result<SortableId128>::fail(stmt.reason());
    }
    if (auto bind = stmt.value().bind(1, static_cast<i64>(id.value)); !bind.is_ok())
    {
        return qiven::Result<SortableId128>::fail(bind.reason());
    }
    auto row = stmt.value().step();
    if (!row.is_ok())
    {
        return qiven::Result<SortableId128>::fail(row.reason());
    }
    if (!row.value())
    {
        return qiven::Result<SortableId128>::fail(err_constraint("dispatch-unknown"));
    }
    const auto blob = stmt.value().col_blob(0);
    SortableId128 out;
    if (blob.size() != out.bytes.size())
    {
        return qiven::Result<SortableId128>::fail(err_open("dispatch-id-corrupt"));
    }
    std::memcpy(out.bytes.data(), blob.data(), blob.size());
    return out;
}

qiven::Result<std::optional<LeaseState>> RuntimeJournal::lease_of(std::string_view workspace)
{
    auto stmt = m_db.prepare(
        "SELECT holder_install, boot_epoch, fencing_epoch, acquired_ms, expires_ms "
        "FROM leases WHERE workspace = ?");
    if (!stmt.is_ok())
    {
        return qiven::Result<std::optional<LeaseState>>::fail(stmt.reason());
    }
    if (auto bind = stmt.value().bind(1, workspace); !bind.is_ok())
    {
        return qiven::Result<std::optional<LeaseState>>::fail(bind.reason());
    }
    auto row = stmt.value().step();
    if (!row.is_ok())
    {
        return qiven::Result<std::optional<LeaseState>>::fail(row.reason());
    }
    if (!row.value())
    {
        return std::optional<LeaseState> {};
    }
    LeaseState state;
    state.workspace      = std::string(workspace);
    state.holder_install = std::string(stmt.value().col_text(0));
    state.boot_epoch     = static_cast<u64>(stmt.value().col_i64(1));
    state.fencing_epoch  = static_cast<u64>(stmt.value().col_i64(2));
    state.acquired_ms    = static_cast<u64>(stmt.value().col_i64(3));
    state.expires_ms     = static_cast<u64>(stmt.value().col_i64(4));
    return std::optional<LeaseState> { std::move(state) };
}

qiven::Result<bool> RuntimeJournal::barrier_active(std::string_view scope)
{
    auto stmt = m_db.prepare("SELECT 1 FROM barriers WHERE scope = ? AND closed_ms IS NULL");
    if (!stmt.is_ok())
    {
        return qiven::Result<bool>::fail(stmt.reason());
    }
    if (auto bind = stmt.value().bind(1, scope); !bind.is_ok())
    {
        return qiven::Result<bool>::fail(bind.reason());
    }
    auto row = stmt.value().step();
    if (!row.is_ok())
    {
        return qiven::Result<bool>::fail(row.reason());
    }
    return row.value();
}

qiven::Result<u64> RuntimeJournal::audit_event_count()
{
    auto stmt = m_db.prepare("SELECT COUNT(*) FROM audit_events");
    if (!stmt.is_ok())
    {
        return qiven::Result<u64>::fail(stmt.reason());
    }
    auto row = stmt.value().step();
    if (!row.is_ok() || !row.value())
    {
        return qiven::Result<u64>::fail(
            row.is_ok() ? err_open("journal-audit-count-unavailable") : row.reason());
    }
    return static_cast<u64>(stmt.value().col_i64(0));
}

qiven::Result<u64> RuntimeJournal::boot_epoch()
{
    return m_boot_epoch;
}

qiven::Result<u64> RuntimeJournal::max_generation_id()
{
    auto stmt = m_db.prepare("SELECT COALESCE(MAX(id), 0) FROM generations");
    if (!stmt.is_ok())
    {
        return qiven::Result<u64>::fail(stmt.reason());
    }
    auto row = stmt.value().step();
    if (!row.is_ok() || !row.value())
    {
        return qiven::Result<u64>::fail(
            row.is_ok() ? err_open("journal-generation-max-unavailable") : row.reason());
    }
    return static_cast<u64>(stmt.value().col_i64(0));
}

qiven::Result<std::string> RuntimeJournal::install_id()
{
    return m_install_id;
}

qiven::Result<void> RuntimeJournal::verify_audit_chain()
{
    auto genesis = read_meta(m_db, "genesis_hash");
    if (!genesis.is_ok())
    {
        return failed<void>(genesis);
    }
    if (!genesis.value().has_value())
    {
        return qiven::Result<void>::fail(err_chain("journal-genesis-hash-missing"));
    }

    auto stmt = m_db.prepare(
        "SELECT seq, prev_hash, event_hash, kind, payload FROM audit_events ORDER BY seq");
    if (!stmt.is_ok())
    {
        return failed<void>(stmt);
    }

    std::array<std::byte, 32> prev = zero_hash();
    bool first                     = true;
    usize count                    = 0;
    i64 expected_seq               = 1;
    while (true)
    {
        auto row = stmt.value().step();
        if (!row.is_ok())
        {
            return failed<void>(row);
        }
        if (!row.value())
        {
            break;
        }
        const i64 seq               = stmt.value().col_i64(0);
        const auto prev_blob        = stmt.value().col_blob(1);
        const auto hash_blob        = stmt.value().col_blob(2);
        const std::string_view kind = stmt.value().col_text(3);
        const auto payload          = stmt.value().col_blob(4);

        if (seq != expected_seq)
        {
            return qiven::Result<void>::fail(err_chain(
                "audit-seq-gap at seq " + std::to_string(seq) + " (expected " +
                std::to_string(expected_seq) + ")"));
        }
        ++expected_seq;

        if (prev_blob.size() != prev.size() || hash_blob.size() != 32 || kind.empty())
        {
            return qiven::Result<void>::fail(err_chain("audit-row-malformed at seq " +
                                                       std::to_string(seq)));
        }
        if (std::memcmp(prev.data(), prev_blob.data(), prev.size()) != 0)
        {
            return qiven::Result<void>::fail(
                err_chain("audit-prev-hash-mismatch at seq " + std::to_string(seq)));
        }
        const SHA256Digest recomputed = chain_hash(prev, kind, payload);
        if (std::memcmp(recomputed.data(), hash_blob.data(), recomputed.size()) != 0)
        {
            return qiven::Result<void>::fail(
                err_chain("audit-event-hash-mismatch at seq " + std::to_string(seq)));
        }
        if (first)
        {
            if (to_hex(hash_blob) != *genesis.value())
            {
                return qiven::Result<void>::fail(err_chain("audit-genesis-hash-mismatch"));
            }
            first = false;
        }
        std::memcpy(prev.data(), hash_blob.data(), prev.size());
        ++count;
    }
    if (count == 0)
    {
        return qiven::Result<void>::fail(err_chain("audit-chain-empty"));
    }
    return ok();
}

// --- recovery ----------------------------------------------------------------

qiven::Result<port::JournalRecoveryReport> RuntimeJournal::recover_at(u64 now_ms)
{
    using port::JournalRecoveryReport;
    using port::RecoveryClassification;

    // Step 1: physical integrity (ARCH §13.2 step 2). Failure attempts to
    // persist the quarantine flag (a control flag, not a chain repair) and
    // reports Quarantined.
    const auto mark_quarantined = [&]() -> void {
        auto flag = m_db.txn([&]() -> qiven::Result<void> {
            return write_meta(m_db, "quarantine", "1");
        });
        static_cast<void>(flag);
    };

    if (auto check = m_db.quick_check(); !check.is_ok())
    {
        mark_quarantined();
        return qiven::Result<JournalRecoveryReport>::fail(check.reason());
    }
    // Step 2: audit hash chain verify (fail → quarantine; ARCH §10.4).
    if (auto chain = verify_audit_chain(); !chain.is_ok())
    {
        mark_quarantined();
        return failed<JournalRecoveryReport>(chain);
    }
    // Step 3: clock policy (D-8): a wall clock earlier than the last
    // journal event by > 300 s fails closed until doctor (MVP-3).
    if (auto clock = check_clock(now_ms); !clock.is_ok())
    {
        return failed<JournalRecoveryReport>(clock);
    }

    // Steps 4-8 in one recovery transaction: crash mid-recovery rolls the
    // whole walk back and the next attempt re-runs it (idempotent).
    JournalRecoveryReport report;
    report.audit_chain_verified = true;

    u64 stale_marked       = 0;
    usize unresolved       = 0;
    usize barriers         = 0;
    bool any_conflict      = false;
    bool any_reconstructed = false;
    bool any_not_performed = false;

    auto txn = m_db.txn([&]() -> qiven::Result<void> {
        // Step 4: boot epoch bump (every RuntimeHost start; ARCH §13.2
        // step 3). The in-memory epoch follows the durable one.
        const u64 new_epoch = m_boot_epoch + 1;
        if (auto meta = write_meta(m_db, meta_boot_epoch, std::to_string(new_epoch));
            !meta.is_ok())
        {
            return failed<void>(meta);
        }

        // Step 5: stale-mark unconsumed (bound) decisions from older boot
        // epochs; consumed decisions are untouched — single consumption
        // survives restarts by construction (exit gate 2).
        auto stale = m_db.prepare(
            "UPDATE decisions SET state = 'stale' WHERE state = 'bound' AND \"transaction\" IN "
            "(SELECT id FROM transactions WHERE boot_epoch < ?)");
        if (!stale.is_ok())
        {
            return failed<void>(stale);
        }
        // Compare against the NEW epoch: decisions bound in any earlier
        // boot (including the one that just died) are stale.
        if (auto bind = stale.value().bind(1, static_cast<i64>(new_epoch)); !bind.is_ok())
        {
            return failed<void>(bind);
        }
        auto stale_row = stale.value().step();
        if (!stale_row.is_ok())
        {
            return failed<void>(stale_row);
        }
        {
            auto count = m_db.prepare(
                "SELECT COUNT(*) FROM decisions WHERE state = 'stale'");
            if (!count.is_ok())
            {
                return failed<void>(count);
            }
            auto row = count.value().step();
            if (!row.is_ok())
            {
                return failed<void>(row);
            }
            if (!row.value())
            {
                return qiven::Result<void>::fail(err_open("journal-stale-count-unavailable"));
            }
            stale_marked = static_cast<u64>(count.value().col_i64(0));
        }

        m_boot_epoch = new_epoch;

        // Step 6: leases are never deleted; expiry is derived at
        // acquire_lease time (fencing high-water mark preserved).

        // Step 7: classify unresolved dispatches through the inspector
        // seam (the default is fail-closed; MVP-6 installs real Git
        // inspection — ARCH §13.2 steps 6-8, batch design §3.5).
        auto pending = m_db.prepare(
            "SELECT d.id, d.\"transaction\", d.plan_digest, d.state FROM dispatches d "
            "LEFT JOIN outcomes o ON o.dispatch = d.id "
            "WHERE d.state IN ('prepared', 'dispatched') AND o.dispatch IS NULL");
        if (!pending.is_ok())
        {
            return failed<void>(pending);
        }

        std::vector<PendingDispatch> pendings;
        while (true)
        {
            auto row = pending.value().step();
            if (!row.is_ok())
            {
                return failed<void>(row);
            }
            if (!row.value())
            {
                break;
            }
            PendingDispatch current;
            const auto id_blob = pending.value().col_blob(0);
            if (id_blob.size() != current.id.bytes.size())
            {
                return qiven::Result<void>::fail(err_open("journal-dispatch-id-corrupt"));
            }
            std::memcpy(current.id.bytes.data(), id_blob.data(), id_blob.size());
            current.transaction.value = static_cast<u64>(pending.value().col_i64(1));
            const auto plan_blob      = pending.value().col_blob(2);
            if (plan_blob.size() != current.plan_digest.sha256.size())
            {
                return qiven::Result<void>::fail(err_open("journal-plan-digest-corrupt"));
            }
            std::memcpy(current.plan_digest.sha256.data(), plan_blob.data(),
                        plan_blob.size());
            current.state = std::string(pending.value().col_text(3));
            pendings.push_back(std::move(current));
        }

        unresolved = pendings.size();
        for (const PendingDispatch& dispatch : pendings)
        {
            const DispatchResolution resolution = m_inspector->classify(dispatch);
            const std::string dispatch_hex      = to_hex(id_bytes(dispatch.id));
            switch (resolution.classification)
            {
            case port::RecoveryClassification::Reconstructed:
            {
                const std::string status = resolution.reconstructed_status.value_or(
                    std::string(tx_state::succeeded));
                if (!is_valid_outcome_status(status))
                {
                    return qiven::Result<void>::fail(err_constraint(
                        "inspector-reconstructed-status-invalid"));
                }
                any_reconstructed = true;
                // Runs inside the already-open recovery transaction (same
                // connection; no inner BEGIN).
                if (auto apply = apply_resolution(dispatch, status,
                                                  resolution.ref_observed, now_ms);
                    !apply.is_ok())
                {
                    return failed<void>(apply);
                }
                break;
            }
            case port::RecoveryClassification::NotPerformed:
            {
                any_not_performed = true;
                if (auto apply = apply_resolution(dispatch, std::string(tx_state::failed),
                                                  std::nullopt, now_ms);
                    !apply.is_ok())
                {
                    return failed<void>(apply);
                }
                break;
            }
            case port::RecoveryClassification::IndeterminateConflict:
            {
                any_conflict = true;
                if (auto apply = apply_resolution(dispatch,
                                                  std::string(tx_state::indeterminate),
                                                  std::nullopt, now_ms);
                    !apply.is_ok())
                {
                    return failed<void>(apply);
                }
                {
                    const std::string scope = "journal.reconcile." + dispatch_hex;
                    auto barrier            = m_db.prepare(
                        "INSERT INTO barriers(scope, reason, opened_ms, closed_ms, "
                                   "evidence) VALUES(?, ?, ?, NULL, ?)");
                    if (!barrier.is_ok())
                    {
                        return failed<void>(barrier);
                    }
                    if (auto bind = barrier.value().bind(1, scope); !bind.is_ok())
                    {
                        return failed<void>(bind);
                    }
                    if (auto bind = barrier.value().bind(2, "reconciliation-unknown");
                        !bind.is_ok())
                    {
                        return failed<void>(bind);
                    }
                    if (auto bind = barrier.value().bind(3, static_cast<i64>(now_ms));
                        !bind.is_ok())
                    {
                        return failed<void>(bind);
                    }
                    if (auto bind = barrier.value().bind(4, "inspector=" +
                                                                std::string("fail-closed"));
                        !bind.is_ok())
                    {
                        return failed<void>(bind);
                    }
                    auto inserted = barrier.value().step();
                    if (!inserted.is_ok())
                    {
                        return failed<void>(inserted);
                    }
                    ++barriers;
                }
                break;
            }
            case port::RecoveryClassification::Quarantined:
            {
                if (auto flag = write_meta(m_db, "quarantine", "1"); !flag.is_ok())
                {
                    return failed<void>(flag);
                }
                std::vector<std::byte> quarantine_payload;
                put_str(quarantine_payload, "recovery-inspector-quarantine");
                put_u64(quarantine_payload, now_ms);
                if (auto audit =
                        append_audit(kind_quarantine_entered, quarantine_payload);
                    !audit.is_ok())
                {
                    return failed<void>(audit);
                }
                m_quarantined         = true;
                report.classification = port::RecoveryClassification::Quarantined;
                return touch_wall_clock(now_ms); // stop: quarantine halts everything
            }
            case port::RecoveryClassification::Clean:
            default:
            {
                // An inspector calling a pending dispatch Clean violates
                // its contract — fail closed, never guess.
                return qiven::Result<void>::fail(err_constraint(
                    "inspector-clean-for-pending-dispatch"));
            }
            }
        }

        // Step 8: the recovery itself is audited with its outcome.
        std::vector<std::byte> payload;
        put_u64(payload, m_boot_epoch);
        put_u64(payload, stale_marked);
        put_u64(payload, static_cast<u64>(unresolved));
        put_u64(payload, static_cast<u64>(barriers));
        if (any_conflict || any_reconstructed || any_not_performed)
        {
            put_str(payload, any_conflict        ? "indeterminate"
                             : any_reconstructed ? "reconstructed"
                                                 : "not_performed");
        }
        else
        {
            put_str(payload, "clean");
        }
        if (auto audit = append_audit(kind_recovery_completed, payload); !audit.is_ok())
        {
            return failed<void>(audit);
        }
        return touch_wall_clock(now_ms);
    });
    if (!txn.is_ok())
    {
        return failed<JournalRecoveryReport>(txn);
    }

    if (report.classification != port::RecoveryClassification::Quarantined)
    {
        if (any_conflict)
        {
            report.classification = port::RecoveryClassification::IndeterminateConflict;
        }
        else if (any_reconstructed)
        {
            report.classification = port::RecoveryClassification::Reconstructed;
        }
        else if (any_not_performed)
        {
            report.classification = port::RecoveryClassification::NotPerformed;
        }
        else
        {
            report.classification = port::RecoveryClassification::Clean;
        }
    }
    report.unresolved_transactions = unresolved;
    report.barriers_opened         = barriers;
    return report;
}

// Private helper applying one inspector resolution: one outcome row plus
// guarded terminal transitions for the dispatch and its transaction.
qiven::Result<void> RuntimeJournal::apply_resolution(const PendingDispatch& dispatch,
                                                     const std::string& status,
                                                     const std::optional<std::string>& ref_observed,
                                                     u64 now_ms)
{
    auto insert = m_db.prepare(
        "INSERT INTO outcomes(dispatch, status, ref_observed, validator_digest, "
        "observed_ms) VALUES(?, ?, ?, NULL, ?)");
    if (!insert.is_ok())
    {
        return failed<void>(insert);
    }
    if (auto bind = insert.value().bind(1, id_bytes(dispatch.id)); !bind.is_ok())
    {
        return failed<void>(bind);
    }
    if (auto bind = insert.value().bind(2, status); !bind.is_ok())
    {
        return failed<void>(bind);
    }
    if (ref_observed.has_value())
    {
        if (auto bind = insert.value().bind(3, *ref_observed); !bind.is_ok())
        {
            return failed<void>(bind);
        }
    }
    else if (auto bind = insert.value().bind_null(3); !bind.is_ok())
    {
        return failed<void>(bind);
    }
    if (auto bind = insert.value().bind(4, static_cast<i64>(now_ms)); !bind.is_ok())
    {
        return failed<void>(bind);
    }
    auto inserted = insert.value().step();
    if (!inserted.is_ok())
    {
        return failed<void>(inserted);
    }

    auto close_dispatch = m_db.prepare(
        "UPDATE dispatches SET state = ?, done_ms = ? WHERE id = ? AND state IN "
        "('prepared', 'dispatched')");
    if (!close_dispatch.is_ok())
    {
        return failed<void>(close_dispatch);
    }
    if (auto bind = close_dispatch.value().bind(1, status); !bind.is_ok())
    {
        return failed<void>(bind);
    }
    if (auto bind = close_dispatch.value().bind(2, static_cast<i64>(now_ms)); !bind.is_ok())
    {
        return failed<void>(bind);
    }
    if (auto bind = close_dispatch.value().bind(3, id_bytes(dispatch.id)); !bind.is_ok())
    {
        return failed<void>(bind);
    }
    auto row = close_dispatch.value().step();
    if (!row.is_ok())
    {
        return failed<void>(row);
    }

    auto close_tx = m_db.prepare(
        "UPDATE transactions SET state = ? WHERE id = ? AND state IN ('prepared', "
        "'dispatched')");
    if (!close_tx.is_ok())
    {
        return failed<void>(close_tx);
    }
    if (auto bind = close_tx.value().bind(1, status); !bind.is_ok())
    {
        return failed<void>(bind);
    }
    if (auto bind = close_tx.value().bind(2, static_cast<i64>(dispatch.transaction.value));
        !bind.is_ok())
    {
        return failed<void>(bind);
    }
    row = close_tx.value().step();
    if (!row.is_ok())
    {
        return failed<void>(row);
    }

    std::vector<std::byte> payload;
    put_id(payload, dispatch.id);
    put_str(payload, status);
    put_u64(payload, now_ms);
    return append_audit(kind_outcome_observed, payload);
}

// --- IRuntimeJournalPort ----------------------------------------------------

qiven::Result<void> RuntimeJournal::append(const port::JournalRecord& record)
{
    const u64 now_ms = wall_now_ms();
    if (auto prelude = deny_if_quarantined(); !prelude.is_ok())
    {
        return failed<void>(prelude);
    }
    if (auto clock = check_clock(now_ms); !clock.is_ok())
    {
        return failed<void>(clock);
    }

    auto txn = m_db.txn([&]() -> qiven::Result<void> {
        std::vector<std::byte> payload;
        put_id(payload, record.record_id);
        put_u64(payload, record.transaction.value);
        put_bytes(payload, record.payload);
        if (auto audit = append_audit(port_kind_name(record.kind), payload); !audit.is_ok())
        {
            return failed<void>(audit);
        }
        return touch_wall_clock(now_ms);
    });
    if (!txn.is_ok())
    {
        return txn;
    }
    return ok();
}

port::JournalRecoveryReport RuntimeJournal::recover()
{
    auto outcome = recover_at(wall_now_ms());
    if (outcome.is_ok())
    {
        return outcome.value();
    }
    port::JournalRecoveryReport report;
    report.classification       = port::RecoveryClassification::Quarantined;
    report.audit_chain_verified = false;
    return report;
}
} // namespace qiven::runtime::journal
