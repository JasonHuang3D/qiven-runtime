// ============================================================================
// third_party_sqlite.cpp — TP-1 discovery/compile/link smoke (Devkit
// third-party-dependencies.md; design §17). Proves the vendored
// amalgamation compiles under the standard's scoped flag adaptation and
// behaves as a working SQLite at runtime: version identity, in-memory
// open, exec, prepared read.
// ============================================================================

#include <qiven/contracts.hpp>

#include <sqlite3.h>

#include <cstdio>
#include <string>

int main()
{
    // version identity matches the provenance record
    QIVEN_VERIFY(sqlite3_libversion() == std::string("3.53.4"));
    QIVEN_VERIFY(sqlite3_threadsafe() >= 1); // SQLITE_THREADSAFE=1 compiled in
    QIVEN_VERIFY(sqlite3_compileoption_used("OMIT_LOAD_EXTENSION") != 0);
    QIVEN_VERIFY(sqlite3_compileoption_used("OMIT_DEPRECATED") != 0);

    // in-memory open + exec + prepared read round trip
    sqlite3* db = nullptr;
    QIVEN_VERIFY(sqlite3_open(":memory:", &db) == SQLITE_OK);
    char* error = nullptr;
    QIVEN_VERIFY(sqlite3_exec(db,
                              "CREATE TABLE probe(id INTEGER PRIMARY KEY, value TEXT NOT NULL);"
                              "INSERT INTO probe(value) VALUES ('qiven');",
                              nullptr, nullptr, &error) == SQLITE_OK);
    if (error != nullptr)
    {
        std::printf("sqlite error: %s\n", error);
        sqlite3_free(error);
    }

    sqlite3_stmt* stmt = nullptr;
    QIVEN_VERIFY(sqlite3_prepare_v2(db, "SELECT id, value FROM probe", -1, &stmt, nullptr) == SQLITE_OK);
    int rows = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        QIVEN_VERIFY(sqlite3_column_int(stmt, 0) == 1);
        const unsigned char* value = sqlite3_column_text(stmt, 1);
        QIVEN_VERIFY(value != nullptr && std::string(reinterpret_cast<const char*>(value)) == "qiven");
        ++rows;
    }
    QIVEN_VERIFY(rows == 1);
    sqlite3_finalize(stmt);

    // compileoption probe of the default WAL sync level (design §7.1)
    QIVEN_VERIFY(sqlite3_compileoption_used("DEFAULT_WAL_SYNCHRONOUS=1") != 0);

    sqlite3_close(db);
    std::printf("[ OK ] third-party-sqlite\n");
    return 0;
}
