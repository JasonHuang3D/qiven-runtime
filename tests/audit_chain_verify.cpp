// ============================================================================
// audit_chain_verify.cpp — audit hash chain law and tamper detection
// (MVP-1; ARCH §10.2/§10.4; design docs/design/mvp1-journal.md §6 row 3,
// exit gate 5)
//
// Proves: the chain verifies after ordinary command activity; tampering
// with a payload, deleting a row, or corrupting a stored hash is detected
// as a typed chain failure and places the journal in quarantine — and a
// quarantined journal denies mutation while verification stays readable.
// ============================================================================

#include <qiven/contracts.hpp>
#include <qiven/runtime/identity.hpp>
#include <qiven/runtime/journal/runtime_journal.hpp>

#include <sqlite3.h>

#include <cstdio>
#include <filesystem>
#include <string>

namespace
{
using qiven::runtime::journal::JournalOpenIntent;
using qiven::runtime::journal::RuntimeJournal;

constexpr qiven::u64 t0 = 3'000'000;

std::filesystem::path fresh_dir(const char* name)
{
    const auto root = std::filesystem::path(QIVEN_RUNTIME_TEST_WORKROOT) / "chain";
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

// Grows the chain with a scripted healthy transaction so tamper targets
// exist (several audit events across kinds).
bool seed_activity(const std::filesystem::path& file)
{
    auto opened = RuntimeJournal::open(file, JournalOpenIntent::CreateNew, t0);
    if (!opened.is_ok())
    {
        return false;
    }
    auto journal = std::move(opened.value());
    if (!journal->advance_generation(qiven::runtime::RuntimeGenerationId { 1 }, digest_of(0x01),
                                     digest_of(0x02), "test-build", t0 + 1)
             .is_ok())
    {
        return false;
    }
    for (int i = 0; i < 4; ++i)
    {
        if (!journal
                 ->open_transaction(
                     { {}, "correlation-" + std::to_string(i), digest_of(0x10 + i), std::optional<std::string> { "base-" + std::to_string(i) } },
                     t0 + 10 + static_cast<qiven::u64>(i))
                 .is_ok())
        {
            return false;
        }
    }
    return journal->audit_event_count().value() >= 5;
}

// Tamper channel: raw SQL against the closed file (the attacker/disk-fault
// path — the test links the singleton directly).
bool tamper(const std::filesystem::path& file, const char* statement)
{
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(file.string().c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) !=
        SQLITE_OK)
    {
        if (db != nullptr)
        {
            sqlite3_close_v2(db);
        }
        return false;
    }
    sqlite3_stmt* stmt = nullptr;
    const bool prepared =
        sqlite3_prepare_v2(db, statement, -1, &stmt, nullptr) == SQLITE_OK;
    const bool stepped = prepared && sqlite3_step(stmt) == SQLITE_DONE;
    if (stmt != nullptr)
    {
        sqlite3_finalize(stmt);
    }
    // WAL: force the change out of the -wal into the main file so a fresh
    // open sees it deterministically.
    char* error = nullptr;
    sqlite3_exec(db, "PRAGMA wal_checkpoint(TRUNCATE)", nullptr, nullptr, &error);
    sqlite3_free(error);
    sqlite3_close_v2(db);
    return stepped;
}

bool chain_fails_on_open(const std::filesystem::path& file)
{
    auto opened = RuntimeJournal::open(file, JournalOpenIntent::OpenExisting, t0 + 999);
    if (!opened.is_ok())
    {
        return true; // a typed open failure is also detection
    }
    auto journal = std::move(opened.value());
    // Structurally intact file opens; verification must catch the damage.
    return !journal->verify_audit_chain().is_ok();
}
} // namespace

int main()
{
    // ---- the chain verifies across ordinary command activity -------------
    {
        const auto file = fresh_dir("healthy.sqlite3");
        QIVEN_VERIFY(seed_activity(file));
        auto opened = RuntimeJournal::open(file, JournalOpenIntent::OpenExisting, t0 + 100);
        QIVEN_VERIFY(opened.is_ok());
        QIVEN_VERIFY(opened.value()->verify_audit_chain().is_ok());

        // Recovery on a healthy journal reports Clean and a verified chain.
        auto report = opened.value()->recover_at(t0 + 101);
        QIVEN_VERIFY(report.is_ok());
        QIVEN_VERIFY(report.value().classification ==
                     qiven::runtime::port::RecoveryClassification::Clean);
        QIVEN_VERIFY(report.value().audit_chain_verified);
    }

    // ---- tamper class 1: payload flip -------------------------------------
    {
        const auto file = fresh_dir("payload-flip.sqlite3");
        QIVEN_VERIFY(seed_activity(file));
        QIVEN_VERIFY(tamper(file,
                            "UPDATE audit_events SET payload = X'00000000' WHERE seq = 3"));
        QIVEN_VERIFY(chain_fails_on_open(file));
    }

    // ---- tamper class 2: stored event-hash corruption ---------------------
    {
        const auto file = fresh_dir("hash-corrupt.sqlite3");
        QIVEN_VERIFY(seed_activity(file));
        QIVEN_VERIFY(tamper(file,
                            "UPDATE audit_events SET event_hash = X'00' WHERE seq = 2"));
        QIVEN_VERIFY(chain_fails_on_open(file));
    }

    // ---- tamper class 3: deleting a middle row breaks the linkage ---------
    {
        const auto file = fresh_dir("row-delete.sqlite3");
        QIVEN_VERIFY(seed_activity(file));
        QIVEN_VERIFY(tamper(file, "DELETE FROM audit_events WHERE seq = 3"));
        QIVEN_VERIFY(chain_fails_on_open(file));
    }

    // ---- tamper class 4: resequencing (gap) -------------------------------
    {
        const auto file = fresh_dir("resequence.sqlite3");
        QIVEN_VERIFY(seed_activity(file));
        QIVEN_VERIFY(
            tamper(file, "UPDATE audit_events SET seq = seq + 10 WHERE seq > 3"));
        QIVEN_VERIFY(chain_fails_on_open(file));
    }

    // ---- quarantine consequence: mutation stops, diagnostics stay ---------
    {
        const auto file = fresh_dir("quarantine.sqlite3");
        QIVEN_VERIFY(seed_activity(file));
        QIVEN_VERIFY(tamper(file,
                            "UPDATE audit_events SET payload = X'00000000' WHERE seq = 3"));

        auto opened = RuntimeJournal::open(file, JournalOpenIntent::OpenExisting, t0 + 100);
        QIVEN_VERIFY(opened.is_ok());
        auto journal = std::move(opened.value());

        // Recovery detects the damage and places the journal in quarantine.
        auto recovery = journal->recover_at(t0 + 101);
        QIVEN_VERIFY(!recovery.is_ok());
        QIVEN_VERIFY(recovery.reason().code ==
                     qiven::runtime::journal::journal_err_chain);

        // The quarantine flag is durable (row in runtime_meta).
        std::string flag;
        {
            sqlite3* db = nullptr;
            QIVEN_VERIFY(sqlite3_open_v2(file.string().c_str(), &db,
                                         SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
            sqlite3_stmt* stmt = nullptr;
            QIVEN_VERIFY(sqlite3_prepare_v2(db,
                                            "SELECT value FROM runtime_meta WHERE key = "
                                            "'quarantine'",
                                            -1, &stmt, nullptr) == SQLITE_OK);
            QIVEN_VERIFY(sqlite3_step(stmt) == SQLITE_ROW);
            const auto* text = sqlite3_column_text(stmt, 0);
            flag             = text != nullptr ? reinterpret_cast<const char*>(text) : "";
            sqlite3_finalize(stmt);
            sqlite3_close_v2(db);
        }
        QIVEN_VERIFY(flag == "1");

        // Mutating commands are denied; reads keep working.
        auto denied = journal->open_transaction(
            { {}, "post-damage", digest_of(0x09), std::nullopt }, t0 + 102);
        QIVEN_VERIFY(!denied.is_ok());
        QIVEN_VERIFY(journal->audit_event_count().is_ok());
    }

    std::printf("[ OK ] audit-chain-verify\n");
    return 0;
}
