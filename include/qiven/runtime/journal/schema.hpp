#pragma once

// ============================================================================
// journal/schema.hpp — SQLite control-journal schema, version and errors
// (production-MVP architecture §10; cpp-design §7.3; batch design
// docs/design/mvp1-journal.md §3.2)
//
// The journal stores RUNTIME CONTROL FACTS ONLY — never qiven-context
// domain records (no second domain truth, ARCH §10.1). Migrations are
// append-only and validated, never silent (batch design §3.1: a future
// schema version is a typed failure, not a guess).
//
// Schema evolution log (append-only; normalization choices recorded here
// per cpp-design §7.3):
//   v1 (MVP-1) — all ARCH §10.2 logical table families present; the
//     evidence_receipts normalization is deliberately deferred (J-1:
//     receipts ride typed audit payloads until resolver persistence
//     needs queryable rows).
// ============================================================================

#include <qiven/types.hpp>

#include <string_view>

namespace qiven::runtime::journal
{
// Journal module error codes (cpp-design §5, range 50-59; batch design
// §3.7). 103 (host quarantine lifecycle) stays reserved for MVP-3.
inline constexpr u32 journal_err_open_corrupt     = 51; // quick_check fail, unreadable, not-a-journal
inline constexpr u32 journal_err_migration        = 52; // unknown/missing/future schema version
inline constexpr u32 journal_err_constraint       = 53; // precondition, regression, double consume
inline constexpr u32 journal_err_chain            = 54; // hash mismatch/gap; quarantine-denied mutation
inline constexpr u32 journal_err_clock_regression = 55; // D-8 fail-closed window

// runtime_meta keys (cpp-design §7.3; batch design §3.2).
inline constexpr std::string_view meta_schema_version = "schema_version";
inline constexpr std::string_view meta_install_id     = "install_id";
inline constexpr std::string_view meta_boot_epoch     = "boot_epoch";
inline constexpr u64 no_boot_epoch                    = 0;

// Physical schema v1 (cpp-design §7.3 with the two batch-design §1
// corrections: decisions.state declared with its consumption invariant;
// leases keep one row per workspace forever with derived expiry so the
// fencing epoch high-water mark can never be destroyed).
inline constexpr i64 current_schema_version = 1;

[[nodiscard]] std::string_view schema_v1_ddl() noexcept;

// PRAGMA configuration applied on every open (ARCH §10.1): WAL, FULL
// synchronous, foreign keys, bounded busy timeout, fixed page cache.
[[nodiscard]] std::string_view open_time_pragmas() noexcept;

// Transactions / dispatches state values (ARCH §13.1, §11.2). MVP-1
// commands reach observed/judging/decided/prepared and the terminal
// succeeded/failed/indeterminate subset; the remaining values are legal
// in the CHECK constraints from day one so later batches add commands,
// not migrations (batch design §3.2, deferral J-2).
namespace tx_state
{
inline constexpr std::string_view observed      = "observed";
inline constexpr std::string_view judging       = "judging";
inline constexpr std::string_view redeliberate  = "redeliberate";
inline constexpr std::string_view denied        = "denied";
inline constexpr std::string_view decided       = "decided";
inline constexpr std::string_view prepared      = "prepared";
inline constexpr std::string_view dispatched    = "dispatched";
inline constexpr std::string_view succeeded     = "succeeded";
inline constexpr std::string_view failed        = "failed";
inline constexpr std::string_view indeterminate = "indeterminate";
inline constexpr std::string_view quarantined   = "quarantined";
} // namespace tx_state

namespace decision_state
{
inline constexpr std::string_view bound    = "bound";
inline constexpr std::string_view consumed = "consumed";
inline constexpr std::string_view stale    = "stale";
} // namespace decision_state

namespace dispatch_state
{
inline constexpr std::string_view prepared      = "prepared";
inline constexpr std::string_view dispatched    = "dispatched";
inline constexpr std::string_view succeeded     = "succeeded";
inline constexpr std::string_view failed        = "failed";
inline constexpr std::string_view indeterminate = "indeterminate";
inline constexpr std::string_view quarantined   = "quarantined";
} // namespace dispatch_state
} // namespace qiven::runtime::journal
