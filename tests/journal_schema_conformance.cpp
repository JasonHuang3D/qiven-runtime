// ============================================================================
// journal_schema_conformance.cpp — ARCH §10.2 no-collapse invariant,
// migration law, and the fresh-database corruption guard (MVP-1; design
// docs/design/mvp1-journal.md §6 row 2, exit gate 4)
//
// Proves: every logical table family of ARCH §10.2 is physically present
// with its essential fields (never the thin consumed-int/barrier-scope
// image); an unknown future schema version is a typed failure; a
// zero-byte / truncated / foreign database is NEVER silently recreated as
// a fresh mutating journal; quarantine denies mutation while reads stay
// available.
// ============================================================================

#include <qiven/contracts.hpp>
#include <qiven/runtime/identity.hpp>
#include <qiven/runtime/journal/runtime_journal.hpp>

#include <sqlite3.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace
{
using qiven::runtime::journal::JournalOpenIntent;
using qiven::runtime::journal::RuntimeJournal;

constexpr qiven::u64 t0 = 2'000'000;

std::filesystem::path workdir(const char* name)
{
    const auto root = std::filesystem::path(QIVEN_RUNTIME_TEST_WORKROOT) / "schema";
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

// Direct SQL over the file (the test links the singleton directly — the
// tampering channel a real attacker or disk fault would use).
bool sql_scalar(const std::filesystem::path& file, const char* query, std::string& out)
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
    bool ok            = sqlite3_prepare_v2(db, query, -1, &stmt, nullptr) == SQLITE_OK &&
              sqlite3_step(stmt) == SQLITE_ROW;
    if (ok)
    {
        const auto* text = sqlite3_column_text(stmt, 0);
        out              = text != nullptr ? reinterpret_cast<const char*>(text) : "";
    }
    if (stmt != nullptr)
    {
        sqlite3_finalize(stmt);
    }
    sqlite3_close_v2(db);
    return ok;
}

bool sql_exec(const std::filesystem::path& file, const char* query)
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
    char* error   = nullptr;
    const bool ok = sqlite3_exec(db, query, nullptr, nullptr, &error) == SQLITE_OK;
    sqlite3_free(error);
    sqlite3_close_v2(db);
    return ok;
}
} // namespace

int main()
{
    // ---- §10.2 no-collapse: all logical table families physically present
    {
        const auto file = workdir("conformance.sqlite3");
        auto opened     = RuntimeJournal::open(file, JournalOpenIntent::CreateNew, t0);
        QIVEN_VERIFY(opened.is_ok());
        auto journal = std::move(opened.value());

        const char* tables[] = { "runtime_meta", "generations", "sessions", "transactions",
                                 "decisions", "leases", "dispatches", "outcomes",
                                 "barriers", "audit_events" };
        for (const char* table : tables)
        {
            const std::string query =
                std::string("SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' AND "
                            "name = '") +
                table + "'";
            std::string count;
            QIVEN_VERIFY(sql_scalar(file, query.c_str(), count));
            QIVEN_VERIFY(count == "1");
        }

        // Essential fields the thin image never carried (ARCH §10.2): the
        // decisions row is (id, token_hash, transaction, generation,
        // binding_digest, expires, state, consumed) — not a consumed int.
        std::string columns;
        QIVEN_VERIFY(sql_scalar(file,
                                "SELECT group_concat(name) FROM pragma_table_info('decisions')",
                                columns));
        QIVEN_VERIFY(columns.find("token_hash") != std::string::npos);
        QIVEN_VERIFY(columns.find("binding_digest") != std::string::npos);
        QIVEN_VERIFY(columns.find("expires_ms") != std::string::npos);
        QIVEN_VERIFY(columns.find("state") != std::string::npos);

        // The audit chain carries a hash chain, not a bare log.
        std::string audit_columns;
        QIVEN_VERIFY(sql_scalar(file,
                                "SELECT group_concat(name) FROM "
                                "pragma_table_info('audit_events')",
                                audit_columns));
        QIVEN_VERIFY(audit_columns.find("prev_hash") != std::string::npos);
        QIVEN_VERIFY(audit_columns.find("event_hash") != std::string::npos);

        // WAL configuration is the storage decision of ARCH §10.1.
        std::string mode;
        QIVEN_VERIFY(sql_scalar(file, "PRAGMA journal_mode", mode));
        QIVEN_VERIFY(mode == "wal");

        journal.reset();
    }

    // ---- migration law: future schema version fails typed, never adopts --
    {
        const auto file = workdir("future.sqlite3");
        auto opened     = RuntimeJournal::open(file, JournalOpenIntent::CreateNew, t0);
        QIVEN_VERIFY(opened.is_ok());
        opened.value().reset();

        QIVEN_VERIFY(sql_exec(file, "UPDATE runtime_meta SET value = '99' WHERE key = "
                                    "'schema_version'"));

        auto future = RuntimeJournal::open(file, JournalOpenIntent::OpenExisting, t0 + 1);
        QIVEN_VERIFY(!future.is_ok());
        QIVEN_VERIFY(future.reason().code ==
                     qiven::runtime::journal::journal_err_migration);
    }

    // ---- exit gate 4: corruption never silently becomes a fresh journal ---
    {
        // (a) CreateNew refuses an existing path — even a zero-byte file.
        const auto zero = workdir("zero-byte.sqlite3");
        {
            std::ofstream touch(zero, std::ios::binary);
        }
        auto refuse = RuntimeJournal::open(zero, JournalOpenIntent::CreateNew, t0);
        QIVEN_VERIFY(!refuse.is_ok());
        QIVEN_VERIFY(refuse.reason().code ==
                     qiven::runtime::journal::journal_err_open_corrupt);
        // And the zero-byte file is still zero bytes (no recreation).
        QIVEN_VERIFY(std::filesystem::file_size(zero) == 0);

        // (b) OpenExisting of a zero-byte file: typed corrupt, no recreation.
        auto empty = RuntimeJournal::open(zero, JournalOpenIntent::OpenExisting, t0);
        QIVEN_VERIFY(!empty.is_ok());
        QIVEN_VERIFY(std::filesystem::file_size(zero) == 0);

        // (c) A valid journal truncated mid-file: typed corrupt on reopen,
        // never a fresh database.
        const auto truncated = workdir("truncated.sqlite3");
        {
            auto opened =
                RuntimeJournal::open(truncated, JournalOpenIntent::CreateNew, t0);
            QIVEN_VERIFY(opened.is_ok());
            opened.value().reset();
        }
        const auto full_size = std::filesystem::file_size(truncated);
        QIVEN_VERIFY(full_size > 1024);
        std::error_code ec;
        std::filesystem::resize_file(truncated, full_size / 3, ec);
        QIVEN_VERIFY(!ec);
        auto broken =
            RuntimeJournal::open(truncated, JournalOpenIntent::OpenExisting, t0);
        QIVEN_VERIFY(!broken.is_ok());
        QIVEN_VERIFY(broken.reason().code ==
                     qiven::runtime::journal::journal_err_open_corrupt);
        // The truncated file was NOT silently rebuilt into a fresh journal.
        QIVEN_VERIFY(std::filesystem::file_size(truncated) <= full_size / 3 + 64);

        // (d) A foreign SQLite database (valid SQLite, not our journal):
        // typed migration failure, never adoption.
        const auto foreign = workdir("foreign.sqlite3");
        {
            sqlite3* db = nullptr;
            QIVEN_VERIFY(sqlite3_open(foreign.string().c_str(), &db) == SQLITE_OK);
            char* error = nullptr;
            QIVEN_VERIFY(sqlite3_exec(db, "CREATE TABLE stuff(x);", nullptr, nullptr,
                                      &error) == SQLITE_OK);
            sqlite3_free(error);
            sqlite3_close_v2(db);
        }
        auto adopted = RuntimeJournal::open(foreign, JournalOpenIntent::OpenExisting, t0);
        QIVEN_VERIFY(!adopted.is_ok());
        QIVEN_VERIFY(adopted.reason().code ==
                     qiven::runtime::journal::journal_err_migration);
    }

    // ---- quarantine: mutation denied, reads stay available -----------------
    {
        const auto file = workdir("quarantine.sqlite3");
        auto opened     = RuntimeJournal::open(file, JournalOpenIntent::CreateNew, t0);
        QIVEN_VERIFY(opened.is_ok());
        auto journal = std::move(opened.value());

        QIVEN_VERIFY(journal->advance_generation(qiven::runtime::RuntimeGenerationId { 1 },
                                                 digest_of(0x01), digest_of(0x02),
                                                 "test-build", t0 + 1)
                         .is_ok());

        // Enter quarantine the way real corruption does (flag + reopen).
        journal.reset();
        QIVEN_VERIFY(sql_exec(file, "UPDATE runtime_meta SET value = '1' WHERE key = "
                                    "'quarantine'"));
        auto quarantined = RuntimeJournal::open(file, JournalOpenIntent::OpenExisting, t0 + 2);
        QIVEN_VERIFY(quarantined.is_ok());

        // Mutation denied with the chain/quarantine class.
        auto denied = quarantined.value()->open_transaction(
            { {}, "correlation", digest_of(0x03), std::nullopt }, t0 + 3);
        QIVEN_VERIFY(!denied.is_ok());
        QIVEN_VERIFY(denied.reason().code == qiven::runtime::journal::journal_err_chain);

        // Reads (diagnostics) remain available in quarantine.
        QIVEN_VERIFY(quarantined.value()->audit_event_count().is_ok());
        QIVEN_VERIFY(quarantined.value()->verify_audit_chain().is_ok());
    }

    std::printf("[ OK ] journal-schema-conformance\n");
    return 0;
}
