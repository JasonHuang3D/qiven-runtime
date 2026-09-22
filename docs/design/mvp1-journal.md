# MVP-1 — SQLite RuntimeJournal and Recovery Skeleton (batch design)

Status: **batch design for MVP-1** (design-first standard; extends
`docs/architecture/runtime-production-mvp-cpp-design.md` §7 — cited below
as **DESIGN** — implementing `docs/architecture/runtime-production-mvp-architecture.md`
— cited below as **ARCH** — §10, §11.1–11.2, §13.1–13.2, §15 MVP-1,
§16.3 journal points).

Batch: MVP-1 of OBL-20260922T155800Z-9A7B41. Exit gate (ARCH §15):

1. forced termination at every journal commit point yields a deterministic
   restart result;
2. consumed decisions never reappear;
3. fencing epoch never decreases;
4. database corruption cannot silently produce a fresh database followed
   by mutation;
5. the audit hash chain verifies.

---

## 1. Basis

- ARCH §10 (journal storage decision, logical tables, identifiers,
  audit/data-minimization, quarantine), §11.1–11.2 (lease/fence, durability
  order), §13.1–13.2 (transaction state machine, startup recovery),
  §15 MVP-1 work list + exit gate, §16.3 fault-injection points.
- DESIGN §2 ground rules (binding), §5 error taxonomy (journal 50–59),
  §6 identity completion plan (MVP-1: `DecisionId`/`DispatchId` are
  `SortableId128`; journal persists minter state per boot epoch via
  PK collision fail-closed), §7 (journal subsystem: binding, location,
  physical schema, commands, crash framework), §16 test spine, §17 TP-1.
- MVP-0 landed contracts this builds on: `port/runtime_journal.hpp`
  (`IRuntimeJournalPort`, `JournalRecoveryReport`, `RecoveryClassification`),
  `identity.hpp` (`SortableId128`, `ContentDigest`, `TokenHash`),
  `auth::csrandom_fill`.
- **Defect corrections to DESIGN §7.3 (found by this design's review;
  DESIGN stays authoritative, this delta records the two fixes)**:
  1. the `decisions` DDL CHECK clause references a `state` column the
     column list never declared — fixed by declaring
     `state TEXT NOT NULL CHECK(state IN ('bound','consumed','stale'))`
     and tightening the consumption invariant to
     `CHECK((state='consumed') = (consumed_ms IS NOT NULL))`;
  2. lease expiry cannot DELETE the lease row — `fencing_epoch`
     monotonicity requires the row (the high-water mark) to persist.
     Expiry is DERIVED (`expires_ms < now`), the single row per
     workspace is updated in place, and `acquire_lease` always computes
     `fencing_epoch = old_row.fencing_epoch + 1` (0→1 on first acquire).
     This makes "epoch never decreases" true by construction.

## 2. Module map

New files (migration law, DESIGN §3 — target subdirectories from the start):

```text
include/qiven/runtime/journal/
  schema.hpp          schema version, DDL, meta keys, error codes
  runtime_journal.hpp JournalDb (RAII wrapper) + RuntimeJournal commands
                      + IRuntimeJournalPort implementation
  recovery.hpp        recovery walk, IReconciliationInspector seam,
                      crash-injection framework (test-guarded)
src/journal/
  schema.cpp          migration/validation application
  runtime_journal.cpp commands, audit chain append, quarantine gate
  recovery.cpp        startup recovery + inspector integration
tests/
  journal_commands.cpp        command semantics + uniqueness + state law
  journal_schema_conformance.cpp  ARCH §10.2 no-collapse + migration law +
                               corruption/fresh-database guard + quarantine
  audit_chain_verify.cpp      chain law + tamper classes + verify points
  journal_crash_recovery.cpp  self-spawning §16.3 journal points (exit gate)
  headers/journal_*.cpp       header self-containment entries
```

Out of scope (later batches by ARCH §15): Git plumbing and ref inspection
(MVP-5/6 — recovery consumes the inspector seam only), evidence-receipt
TABLE normalization (lands with resolver persistence; recorded in
schema.hpp evolution notes), IPC/host lifecycle (MVP-3), projection repair
(MVP-6), WAL metrics events (MVP-7).

## 3. Contracts

### 3.1 `JournalDb` (DESIGN §7.1, no interface abstraction over `sqlite3*`)

```cpp
struct sqlite3;                       // forward-declared: public headers stay
                                      // free of third-party includes (GR-2:
                                      // products link qiven::runtime + OS only)
enum class JournalOpenIntent : u8 { CreateNew, OpenExisting };

class JournalDb {                     // move-only RAII owner
public:
    static qiven::Result<JournalDb> open(const std::filesystem::path& file,
                                         JournalOpenIntent intent);   // 51/52 on corrupt/unknown
    ~JournalDb();                     // sqlite3_close_v2, never throws
    // txn(): BEGIN IMMEDIATE … COMMIT/ROLLBACK; every command is exactly one txn
    qiven::Result<void> txn(const std::function<qiven::Result<void>()>& body);
    // … statement helpers (prepare/step/bind/column), quick_check, close
};
```

Open-time configuration (ARCH §10.1): `journal_mode=WAL`, `synchronous=FULL`,
`foreign_keys=ON`, `busy_timeout=1500`, `cache_size=-2000` (fixed 2 MiB).
`PRAGMA quick_check` runs on every open (fail → typed 51). Full
`integrity_check` is exposed for `doctor`-class callers (bounded policy is
the host's, MVP-3).

**Fresh-database corruption guard (exit gate 4).** `CreateNew` fails if the
path exists at all (a zero-byte file is a truncation artifact, not absence).
`OpenExisting` fails typed (51/52) if the file is not a readable SQLite
database, has no `runtime_meta`, or carries an unknown future
`schema_version`. No code path deletes-and-recreates a journal file; the
quarantine row (below) is the only in-database recovery state and it never
resets on reopen.

### 3.2 Physical schema (schema_version 1; migrations append-only)

DESIGN §7.3 DDL with the two §1 corrections applied. Tables:
`runtime_meta`, `generations`, `sessions`, `transactions`, `decisions`,
`leases`, `dispatches`, `outcomes`, `barriers`, `audit_events` (+ recovery
indexes on `transactions(state)`, `dispatches(state)`, `decisions(state)`).
All ARCH §10.2 logical tables are physically present — the no-collapse
invariant is proven by `journal_schema_conformance` (every logical table
name resolves, with its essential fields, not a thin image).

- `runtime_meta` keys: `schema_version`, `install_id`, `boot_epoch`,
  `quarantine`, `last_wall_ms`, `genesis_hash`.
- `transactions.state` ∈ {observed, judging, redeliberate, denied, decided,
  prepared, dispatched, succeeded, failed, indeterminate, quarantined}
  (ARCH §13.1; MVP-1 commands reach the bold subset; the full set is legal
  in the CHECK from day one so later batches add commands, not migrations).
- `decisions.state` ∈ {bound, consumed, stale} with
  `CHECK((state='consumed') = (consumed_ms IS NOT NULL))`; `token_hash`
  UNIQUE. Single consumption is enforced by
  `UPDATE … WHERE id=? AND state='bound'` inside the consume transaction —
  zero changed rows = typed 53 `decision-not-consumable`.
- Boot epoch lives in `runtime_meta`; a decision's owning boot epoch is its
  transaction's (`transactions.boot_epoch`), joined for stale-marking.
- `genesis_hash`: the `event_hash` of the `journal_created` audit event —
  the chain root a verifier compares before walking.

### 3.3 Commands (DESIGN §7.4 — callers never write rows)

`open_session`, `open_transaction`, `accept_evidence`, `bind_decision`,
`consume_decision`, `acquire_lease`, `record_dispatch_prepared`,
`record_outcome`, `open_barrier`, `close_barrier`, `advance_generation`.
Each is one `BEGIN IMMEDIATE` transaction that also appends its audit
event; each validates preconditions and returns typed errors (below) on
violation; none is permitted while `quarantine=1` (typed 54
`journal-quarantined`; reads remain available).

State machine wiring owned by MVP-1 commands:

| Command | Preconditions (else 53) | Effects |
| --- | --- | --- |
| `open_session` | generation active | row; audit `session_opened` |
| `open_transaction` | — | id = MAX(id)+1; state `observed`; audit |
| `accept_evidence` | tx `observed`/`judging` | audit `evidence_accepted` (typed payload) |
| `bind_decision` | tx `observed`/`judging`, token_hash unseen | tx→`decided`; decision row `bound` |
| `consume_decision` | decision `bound`, not expired | decision→`consumed` (consumed_ms) |
| `acquire_lease` | row absent, or expired, or holder=self | fencing_epoch = old+1 (first = 1) |
| `record_dispatch_prepared` | tx `decided` | tx→`prepared`; dispatch `prepared` |
| `record_outcome` | dispatch `prepared`, no outcome row | dispatch+tx → terminal by status |
| `open_barrier` | no ACTIVE barrier on scope | barrier row (closed_ms NULL) |
| `close_barrier` | barrier active | closed_ms set |
| `advance_generation` | — | prior active retired; new active |

Terminal states never regress (guarded UPDATEs + CHECK constraints); the
states MVP-1 cannot reach (`redeliberate`, `denied`, `dispatched`,
`quarantined`) stay legal-but-unwritten until their owning batch lands the
commands that produce them (recorded deferral, §8).

`RuntimeJournal` implements `IRuntimeJournalPort`: `append(JournalRecord)`
maps each `JournalRecordKind` onto the typed command above (audit-only
kinds append typed audit events); `recover()` runs §3.5.

### 3.4 Audit chain (ARCH §10.2, DESIGN §7.4)

`event_hash = SHA256(prev_hash ‖ kind ‖ canonical_payload)` with
`prev_hash` read inside the same transaction; genesis `prev_hash` = 32
zero bytes and the first event is `journal_created` (install_id,
schema_version, wall time), whose hash is persisted as
`runtime_meta.genesis_hash`. Canonical payloads are length-prefixed
field encodings in a fixed per-kind order (u64/u256-as-blob/UTF-8 string
with u32 byte length) — identical facts produce identical bytes (the
renderer is the only encoder and is append-only per kind). Verification
rehashes the full chain from `genesis_hash`; any mismatch, gap, or
re-sequencing → typed 54 and quarantine. Every command appends exactly one
event; recovery appends `recovery_completed` (its own classification is
part of the payload — recovery is audited, not ambient).

### 3.5 Recovery skeleton (ARCH §13.2 journal scope)

`RuntimeJournal::recover(now_ms)`:

1. quick_check (51 → quarantine, stop);
2. audit-chain verify (54 → quarantine, stop);
3. clock policy (D-8): `now_ms < last_wall_ms − 300 000` → typed 55
   `clock-regression`, fail closed until an operator-adjudicated repair
   path exists (MVP-3 `doctor`);
4. boot_epoch += 1 (audited);
5. stale-mark: `decisions.state='bound'` whose transaction's boot epoch is
   older → `stale` (consumed decisions are untouched — exit gate 2);
6. leases: expired rows stay (epoch is the high-water mark); acquirability
   is derived at `acquire_lease` time;
7. unresolved dispatches (`prepared`/`dispatched` with no outcome) go to
   `IReconciliationInspector::classify` — **the seam MVP-6 fills with real
   Git ref/object inspection** (ARCH §11.2). The default inspector is
   fail-closed: unknown external state → `IndeterminateConflict` +
   barrier `journal.reconcile.<dispatch>`; test inspectors return the
   scripted §16.3 classification. No blind retry exists in MVP-1 — there
   is no retry API at all;
8. `recovery_completed` audit event; report (`JournalRecoveryReport`)
   names classification, unresolved count, barriers, chain-verified flag.

Quarantine mode: `runtime_meta.quarantine=1` (plus the
`quarantine_entered` audit event when the db is still writable). While
set: every mutating command fails typed 54; reads and verification remain
available. Nothing in MVP-1 clears quarantine (no self-healing — an
operator decision, MVP-3 runbook).

### 3.6 Crash-injection framework (DESIGN §7.5)

`recovery.hpp` declares the full §16.3 point set
(`after_create_tx`, `after_evidence_persist`, `after_decision_persist`,
`after_token_consume`, `after_lease_fence`, `after_dispatch_commit`,
`after_candidate_tree`, `after_validator`, `after_commit_object`,
`after_ref_cas`, `after_outcome_commit`, `after_ack_persist`,
`after_projection_update`) with modes `die_before_commit` /
`die_after_commit` / `die_hard`. Trigger sites live in the journal
command path for the six journal-transaction points; the seven
external-effect points are declared but not yet armed (their operations do
not exist until MVP-5/6; arming an unarmed point is a typed test error,
not a silent no-op). Death is `TerminateProcess(GetCurrentProcess())` —
strictly harder than `_exit` (no CRT death notifications), the closest
in-process approximation of power loss on Windows.

Activation: the macro `QIVEN_RUNTIME_TEST_CRASH_POINTS` compiles the hook
body; without it every trigger is an empty inline function (zero
production behavior). The production library is always built WITHOUT the
macro; `qiven-runtime-journal-crash-recovery` compiles `src/journal/*.cpp`
directly with the macro defined (the self-spawning pattern needs hooks
live inside the same process that dies — DESIGN §7.5).

### 3.7 Error codes (DESIGN §5, journal 50–59)

| Code | Meaning |
| --- | --- |
| 51 | open/corrupt (quick_check fail, unreadable file, not-a-journal) |
| 52 | migration (unknown/missing schema version, future version) |
| 53 | constraint (precondition, state regression, double consume, unknown row) |
| 54 | chain (hash mismatch, gap, quarantine-denied mutation) |
| 55 | clock regression (D-8 fail-closed window) |

103 (host quarantine lifecycle) stays reserved for MVP-3 host code.

### 3.8 Time and identity

Time is injected per call (`now_ms`, matching `decision.cpp`/`resolver.cpp`
precedent — no ambient clock). `DecisionId`/`DispatchId` are caller-minted
`SortableId128`; duplicate ids fail closed on the PRIMARY KEY (53) — the
DESIGN §6 "never repeat within an installation" invariant. `install_id`
is 128-bit CSPRNG at `CreateNew` (`auth::csrandom_fill`), hex-stored.

## 4. Concurrency and lifecycle

Single control thread (GR-4): `RuntimeJournal` is not thread-safe and says
so; the SQLite build is serialized (`SQLITE_THREADSAFE=1`) so accidental
cross-thread use cannot corrupt, only contend. `JournalDb` is move-only;
destruction closes via `sqlite3_close_v2`. Startup/shutdown sequences
belong to RuntimeHost (MVP-3); MVP-1 provides the ordered recovery walk
above and a `checkpoint_wal()` (TRUNCATE) for graceful shutdown callers.
Journal files live at `<repo>/.qiven/runtime/journal.sqlite3` (+ `-wal`,
`-shm`, DESIGN §7.2/GR-5); `.gitignore` gains `.qiven/runtime/`. Tests
instantiate journals under `<repo>/.generated-temp/runtime/tests/<name>/`
(regenerable class, convention-compliant) via a compile-time
`QIVEN_RUNTIME_TEST_WORKROOT` definition.

## 5. Failure modes (fail-closed behavior)

| Failure class | Behavior |
| --- | --- |
| Corrupt file / truncation / foreign DB | typed 51; no recreation; quarantine |
| Future schema version | typed 52; no migration guess |
| Audit tamper (flip/delete/reinsert/resequence) | typed 54; quarantine |
| Double consume / regression / unknown parent | typed 53; txn rolls back |
| Crash mid-command (any point) | txn atomicity: the command either fully exists (audit + rows) or not at all |
| Crash after COMMIT before return | effect durable; restart classifies deterministically |
| Clock regression > 300 s | typed 55; recovery stops; no mutation |
| Quarantined journal | mutation denied (54); reads/verify allowed |
| Disk-full / IO error | SQLite propagates as typed 51-class failure; command fails closed |

## 6. Test spine (each row names its exit-gate proof)

| Test | Proves (ARCH) |
| --- | --- |
| `journal_commands` | §13.1 only-commands-transition, terminal-no-regress; §10.2 uniqueness (token UNIQUE, one-outcome-per-dispatch, one-lease-row); §11.1 epoch +1-on-acquire incl. expired takeover |
| `journal_schema_conformance` | §10.2 no-collapse (all logical tables + essential fields); §15 exit gate 4 (zero-byte/truncated/foreign/future-version files never become a fresh mutating DB; CreateNew refuses existing path) |
| `audit_chain_verify` | §15 exit gate 5 (chain verifies; payload flip, row delete, row reinsert, hash corruption → 54 + quarantine; quarantine denies mutation, keeps reads) |
| `journal_crash_recovery` | §15 exit gate 1 (self-spawning, both die modes at all six journal points, deterministic classification per §16.3 table); exit gate 2 (consumed decisions never reappear — re-consume after restart typed 53, stale-marking only touches bound); exit gate 3 (fencing epoch never decreases across crash/reopen/re-acquire cycles) |

Deterministic restart classifications (MVP-1 journal points; the
authoritative inspection source is the journal itself — nothing external
exists yet):

| Crash point × mode | Durable state | Classification |
| --- | --- | --- |
| create_tx before | no tx | Clean |
| create_tx after | tx observed | Clean |
| evidence before/after | tx + evidence audit | Clean |
| decision_persist before | no decision row | Clean |
| decision_persist after | decision bound (old epoch) | Clean (stale-marked at recovery) |
| token_consume before | still bound (old epoch) | Clean (stale-marked; NOT consumable) |
| token_consume after | consumed | Clean; re-consume typed 53 |
| lease_fence before | lease unchanged | Clean; epoch unchanged |
| lease_fence after | epoch N | Clean; next acquire ≥ N+1 |
| dispatch_commit before | no dispatch | Clean |
| dispatch_commit after | dispatch prepared, no outcome | IndeterminateConflict + barrier (default inspector) |
| outcome before | dispatch unresolved | IndeterminateConflict + barrier |
| outcome after | dispatch+tx terminal, outcome row | Clean (resolved) |

"The process restarted" is never an assertion (ARCH §16.3).

## 7. Dependencies and deployment

TP-1 SQLite 3.53.4 via the workspace singleton, exact-SHA pin
(`QIVEN_THIRD_PARTY_PIN 3184538…`), already consumed by this repository
(PR #40); `qiven-runtime` links `qiven::tp::sqlite3` PRIVATE (public
headers forward-declare `struct sqlite3;` only — GR-2 consumer surface
unchanged). No new third-party slots. Deployment: no bundle change
(journal is runtime state, not shipped bits); `.gitignore` gains the
journal location.

## 8. Deferrals (falsifiable, constitution §15)

| # | Deferral | Failure signal / revisit trigger |
| --- | --- | --- |
| J-1 | evidence_receipts TABLE normalization (audit-carried until resolver persistence needs queryable rows) | a consumer needs evidence queries beyond audit replay (MVP-2 resolver persistence expected) |
| J-2 | `redeliberate`/`denied`/`dispatched`/`quarantined` transition commands | the owning batch lands them (ReDeliberate wiring MVP-4; dispatched marking MVP-3/6; tx quarantine MVP-6) |
| J-3 | real Git reconciliation inspector (MVP-6) | any dispatch whose classification must consult refs/objects |
| J-4 | operator `doctor` surface for quarantine/chain repair (MVP-3) | first real quarantine incident |
| J-5 | WAL auto-checkpoint tuning | measured WAL growth in MVP-7 dogfood |

## 9. Review record

Self-review 2026-09-23 (v18 session, designation
`jason-extended-cognition`), performed against the design ALONE, before
any implementation code existed in the branch:

1. **Found and fixed DESIGN §7.3 defect #1** (decisions CHECK references
   an undeclared `state` column) — the exact class of error design-first
   exists to catch; recorded in §1 with the corrected invariant.
2. **Found and fixed DESIGN §7.3 defect #2** — DELETE-on-expiry would
   destroy the fencing high-water mark; corrected to derived expiry +
   in-place epoch increment (monotonic by construction, §3.3).
3. Checked every ARCH §10.2 logical table against the physical schema —
   all present; `actions`/`requirements`/`evidence_receipts` ride the
   transaction row + typed audit payloads (J-1 records the trigger for
   the receipts table; the no-collapse test still proves presence of all
   table FAMILIES ARCH names, which is the invariant's actual claim).
4. Verified exit gates 1–5 each have a named test row (§6) and that
   "deterministic restart" is asserted as journal-visible facts, never
   as "the process restarted".
5. Considered PUBLIC vs PRIVATE sqlite3 linkage: PRIVATE with a
   forward-declared handle keeps third-party out of every consumer's
   compile surface (GR-2); the wrapper remains the only seam (DESIGN
   §7.1 review note 1 affirmed — no interface layer added).
6. Crash-hook activation strategy (library never compiled with hooks;
   crash test compiles `src/journal/*.cpp` with the macro) checked
   against the "no test scaffolding in production binaries" hygiene and
   the self-spawning requirement — the dying process must contain the
   armed hooks, and the production library must not.
7. Time injection follows the `now_ms` parameter precedent instead of a
   clock abstraction (no new seam without a second implementation —
   mirrors the D-2 wrapper reasoning).
8. Confirmed no second domain truth: the journal stores control facts
   only; no qiven-context domain record is written by any MVP-1 command.
