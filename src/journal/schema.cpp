// ============================================================================
// journal/schema.cpp — schema v1 DDL, open-time pragmas (MVP-1)
// (cpp-design §7.3 with the batch-design §1 corrections; batch design §3.2)
// ============================================================================

#include <qiven/runtime/journal/schema.hpp>

namespace qiven::runtime::journal
{
std::string_view schema_v1_ddl() noexcept
{
    using namespace std::string_view_literals;
    // schema_version 1 — migrations are append-only and validated, never
    // silent (batch design §3.1: future versions fail typed 52).
    // decisions: state column declared (cpp-design §7.3 defect fix 1) with
    //   the consumption invariant as a storage-layer CHECK.
    // leases: one row per workspace FOREVER (defect fix 2) — expiry is
    //   derived (expires_ms < now), fencing_epoch is the monotonic
    //   high-water mark acquire_lease increments in place.
    return R"SQL(
CREATE TABLE runtime_meta(key TEXT PRIMARY KEY, value TEXT NOT NULL);
CREATE TABLE generations(id INTEGER PRIMARY KEY, bundle_digest BLOB NOT NULL,
  profile_digest BLOB NOT NULL, build_id TEXT NOT NULL, activated_ms INTEGER NOT NULL,
  status TEXT NOT NULL CHECK(status IN ('active','retired')));
CREATE TABLE sessions(id INTEGER PRIMARY KEY, generation INTEGER NOT NULL REFERENCES generations(id),
  actor INTEGER, harness TEXT, opened_ms INTEGER NOT NULL, closed_ms INTEGER);
CREATE TABLE transactions(id INTEGER PRIMARY KEY, boot_epoch INTEGER NOT NULL,
  causal_parent INTEGER, correlation TEXT NOT NULL, request_digest BLOB NOT NULL,
  base_revision TEXT,
  state TEXT NOT NULL CHECK(state IN ('observed','judging','redeliberate','denied',
    'decided','prepared','dispatched','succeeded','failed','indeterminate','quarantined')),
  UNIQUE(boot_epoch, id));
CREATE TABLE decisions(id BLOB PRIMARY KEY,
  token_hash BLOB UNIQUE NOT NULL, "transaction" INTEGER NOT NULL, generation INTEGER NOT NULL,
  binding_digest BLOB NOT NULL, expires_ms INTEGER NOT NULL,
  state TEXT NOT NULL CHECK(state IN ('bound','consumed','stale')),
  consumed_ms INTEGER,
  CHECK((state='consumed') = (consumed_ms IS NOT NULL)));
CREATE TABLE leases(workspace TEXT PRIMARY KEY, holder_install TEXT NOT NULL,
  boot_epoch INTEGER NOT NULL, fencing_epoch INTEGER NOT NULL, acquired_ms INTEGER NOT NULL,
  expires_ms INTEGER NOT NULL);
CREATE TABLE dispatches(id BLOB PRIMARY KEY, "transaction" INTEGER NOT NULL,
  plan_digest BLOB NOT NULL,
  state TEXT NOT NULL CHECK(state IN ('prepared','dispatched','succeeded','failed',
    'indeterminate','quarantined')),
  started_ms INTEGER NOT NULL, done_ms INTEGER);
CREATE TABLE outcomes(dispatch BLOB PRIMARY KEY REFERENCES dispatches(id),
  status TEXT NOT NULL, ref_observed TEXT, validator_digest BLOB, observed_ms INTEGER NOT NULL);
CREATE TABLE barriers(scope TEXT PRIMARY KEY, reason TEXT NOT NULL, opened_ms INTEGER NOT NULL,
  closed_ms INTEGER, evidence TEXT NOT NULL);
CREATE TABLE audit_events(seq INTEGER PRIMARY KEY AUTOINCREMENT, prev_hash BLOB NOT NULL,
  event_hash BLOB NOT NULL, kind TEXT NOT NULL, payload BLOB NOT NULL);
CREATE INDEX idx_transactions_state ON transactions(state);
CREATE INDEX idx_dispatches_state ON dispatches(state);
CREATE INDEX idx_decisions_state ON decisions(state);
)SQL"sv;
}

std::string_view open_time_pragmas() noexcept
{
    using namespace std::string_view_literals;
    // ARCH §10.1 configuration law. busy_timeout is deliberately bounded
    // (1500 ms): single-writer discipline makes contention a defect, not a
    // wait (cpp-design §7.1). cache_size = -2000 → 2 MiB fixed page cache.
    return R"SQL(
PRAGMA journal_mode = WAL;
PRAGMA synchronous = FULL;
PRAGMA foreign_keys = ON;
PRAGMA busy_timeout = 1500;
PRAGMA cache_size = -2000;
)SQL"sv;
}
} // namespace qiven::runtime::journal
