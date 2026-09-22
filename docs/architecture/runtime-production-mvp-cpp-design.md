# Runtime Production MVP — Detailed C++ Design

Status: **accepted design baseline** (owner direction 2026-09-23; ADR-0047
accepted). This document is the detailed C++ design implementing
`runtime-production-mvp-architecture.md` (the normative architecture,
cited below as **ARCH**). It extends, and where they conflict supersedes
the RCA-era `runtime-cpp-design.md` per the MVP corrections (ARCH §14.2);
the RCA-era document remains authoritative for the pure component layer
that MVP-0 left intact.

Design-first compliance: this document precedes all MVP-1..MVP-7
implementation batches (Devkit engineering standard
`design-first-workflow.md`).

---

## 1. Design basis and reading order

1. **ARCH** — `runtime-production-mvp-architecture.md`: vertical slice,
   invariants (§4), topology (§5), components (§6), control path (§8),
   journal (§10), execution authority (§11), IPC (§12), state machines
   and recovery (§13), source layout (§14), batch ladder + exit gates
   (§15), verification matrix (§16), observability (§17), DoD (§18).
2. Component ADL (`runtime-component-adl.md`) — component semantics
   (phases, causality, receipts) unchanged by the MVP.
3. RCA-era detailed design (`runtime-cpp-design.md`) — the control-plane
   value types and ports MVP-0 built on; superseded only where ARCH §14.2
   mandates corrections (already landed in MVP-0) or where this document
   says so explicitly.
4. Frozen v4 draft semantics (`qiven-context-draft`, pinned) — never
   redefined here.
5. Devkit engineering standards (`qiven-devkit/docs/engineering/`) —
   implementation/testing/execution/third-party/deployment/design-first.

Mandatory language follows ARCH: MUST/SHOULD/MAY. Every MUST here maps
to a test in §16 or an exit gate in ARCH §15.

## 2. Ground rules (binding for every MVP batch)

**GR-1 Language & toolchain.** C++20, MSVC 19.3x (VS2022, pinned by
`qiven-toolchain-win`), `/W4 /permissive- /utf-8` for all first-party
targets; clang-format (pinned) is the formatting law. No RTTI reliance,
no exceptions (all failure channels are `qiven::Result`/typed enums —
existing law), no new global mutable state.

**GR-2 Layering.** `qiven-runtime` links `qiven::foundation`,
`qiven::context` (draft, pinned at an exact SHA), and
`qiven::tp::sqlite3` (vendored, MVP-1; see §17). Nothing else. Products
(`qiven-runtime-host`, `qiven-zcode-hook`, `qiven-record`,
`qiven-runtimectl`) link `qiven::runtime` plus the OS only.

**GR-3 Fail-closed default.** Every subsystem returns a typed denial on
missing/ambiguous/mismatched/unverifiable input. Unknown enum values,
unknown fields, oversized inputs, expired anything → deny/drop. A
subsystem that cannot prove its precondition does not proceed.

**GR-4 Single-writer discipline.** One control thread owns journal
writes and all control state. Every other thread communicates only
through bounded queues or owned handles (existing ingress model,
ARCH §5).

**GR-5 Workspace-bounded execution.** All durable state the host writes
lives under the governed repository checkout:
`<repo>/.qiven/runtime/` (journal, secrets, bundles — gitignored;
see §7.2 and the Devkit deployment standard). No writes outside the
workspace, ever. Temporary candidates use
`<repo>/.generated-temp/runtime/<tx-id>/` (regenerable class).

**GR-6 No premature third-party.** A dependency requires a slot in §17
with provenance under the Devkit third-party standard. The default
answer is "write the bounded first-party implementation"; the exception
must argue maturity (SQLite: reimplementing a transactional database is
not viable; JSON/YAML: bounded codecs ARE viable first-party, see D-1).

**GR-7 Design-first.** No production-code batch without a design section
in this document (or a `docs/design/<batch>-*.md` extending it) merged
before implementation (Devkit `design-first-workflow.md`).

## 3. Target module topology and migration law

ARCH §14.1 defines the target tree. Current repository state is the
RCA-era flat layout plus `port/`, `adapter/`. **Migration law**: new
MVP modules land in their target subdirectories from the start; existing
flat files move only when their batch touches them semantically (no
big-bang churn moves; each move is its own reviewable delta with test
updates). Compat adapters keep existing tests proving frozen semantics
green (ARCH §14.3).

```text
include/qiven/runtime/
  host/        runtime_host.hpp, deployment_profile.hpp (MVP-3/2)
  ipc/         protocol.hpp, named_pipe_server.hpp, framing.hpp (MVP-3)
  cognition/   bundle.hpp, publisher.hpp (MVP-2; extends port/)
  journal/     runtime_journal.hpp, schema.hpp, recovery.hpp (MVP-1)
  authority/   workspace_lease.hpp, fence.hpp (MVP-6)
  record/      command.hpp, domain.hpp, renderer.hpp, index.hpp (MVP-5)
  gitx/        candidate_transaction.hpp, ref_cas.hpp (MVP-5; "gitx"
               avoids colliding with casual uses of "git" in comments)
  processx/    process_runner.hpp (MVP-3 minimal qiven-process slice)
  jsonx/       json_codec.hpp (bounded JSON; D-1)
  auth.hpp scope.hpp identity.hpp decision.hpp ... (landed MVP-0)
  port/        (existing ports unchanged; new ports join here)
apps/
  runtime_host_main.cpp (MVP-3)  zcode_hook_main.cpp (MVP-4)
  record_main.cpp (MVP-5)         runtimectl_main.cpp (MVP-3)
```

`qiven-runtime-app` (bootstrap printer) is retired when
`runtime_host_main.cpp` lands (ARCH §14.2 row 1). The adapter bridge
stays a fixture (row 2).

## 4. Concurrency and lifecycle model

**Threads (fixed set, all joinable, none detached):**

| Thread | Owns | Blocking primitives |
| --- | --- | --- |
| Control thread (main) | journal, control state, phase transitions, IPC accept loop | pipe accept + ingress `pop_wait` with 20 ms slices (existing pattern) |
| Connection N (≤4) | one pipe connection's read/write frames | overlapped pipe IO |
| Executor workers (existing `Executor`) | resolver mechanisms | condition-variable task queue |
| Child supervisors (transient, MVP-3+) | one `qiven-process` child each | WaitForSingleObject + pipe readers |

Synchronization law: control state is touched ONLY on the control
thread (existing `ControlCore` discipline, ARCH §5); everything crossing
a thread boundary is a value message through a bounded queue. Pipes and
process handles are OS handles owned by RAII wrappers on their owning
thread; handle transfer across threads is forbidden (re-open or
duplicate explicitly with a comment).

**Startup (ARCH §13.2, fixed order, each step typed):** acquire process
mutex (`Local\qiven-runtime-<install-id>`) → open journal +
`quick_check` + audit-chain verify → bump boot epoch → stale-mark
unconsumed decisions → expire leases (fence preserved) → reconcile
dispatches against Git → verify/repair projection (barrier on dirty) →
load+verify active cognition bundle → mint RuntimeGeneration → open IPC
endpoint. Before recovery completes the host answers only `status` /
`doctor`; everything else returns `HostRecovering` (typed IPC error 61).

**Shutdown:** stop accepting → drain ingress to quiesce → finish or
checkpoint any open dispatch decision (never exit with an unresolved
dispatch — ARCH §6.1) → `wal_checkpoint(TRUNCATE)` → release lease →
close pipe. A dispatch whose child is still running is waited on up to
its deadline, then the tri-state protocol takes over (Indeterminate, not
blind kill).

**Time authority (D-8):** wall clock (`system_clock`, ms UTC) for
expiry/issuance/audit; steady clock for deadlines/latency. Journal
records wall time + boot epoch; rollback tolerance = decisions carry
`expires_at_ms` and consumers re-check against journal time (a wall
clock earlier than the last journal event by > 300 s fails closed as
`ClockRegression` until an operator runs `doctor`; the §16.4 security
test drives exactly this).

## 5. Error taxonomy

All subsystem errors are `qiven::Result<T, E>` with module-scoped codes
(extend the existing allocation; collisions forbidden):

| Range | Module | Existing examples |
| --- | --- | --- |
| 20–29 | resolver registry | 20 identity, 21 duplicate |
| 30–39 | decision binding/consumption | 30 phase precondition |
| 40–49 | resource scope | 40 ambiguous path, 41 duplicate |
| 50–59 | journal | 51 open/corrupt, 52 migration, 53 constraint, 54 chain |
| 60–69 | IPC/protocol | 61 host-recovering, 62 auth, 63 replay, 64 frame, 65 deadline |
| 70–79 | process runner | 71 spawn, 72 deadline, 73 output-limit, 74 allowlist |
| 80–89 | record/render | 81 schema, 82 render determinism, 83 index |
| 90–99 | git candidate/CAS | 91 base-mismatch, 92 cas-conflict, 93 plumbing |
| 100–109 | host lifecycle | 101 singleton, 102 generation, 103 quarantine |

Recoverable-vs-programmer-error split follows the existing law
(assertions for construction defects; typed results for everything
environmental). Every denial at a governed boundary also appends a
journal audit event (§7) naming the rule that fired.

## 6. Identity, tokens, digests (post-MVP-0 state and completion plan)

MVP-0 landed: `SortableId128` (UUIDv7-shaped) + minter, HMAC-SHA256
decision tokens with CSPRNG nonce, hash-only ledger storage, full-value
correlation, complete receipts. Completion plan:

- **MVP-1**: `DecisionId` and new `DispatchId` are `SortableId128` from
  a minter seeded per boot; journal persists the minter's last value per
  boot epoch (ids never repeat within an installation — enforced by the
  decisions/dispatches primary key, fail-closed on collision).
- **D-10**: `RuntimeGenerationId` / `ControlTransactionId` stay u64
  per-boot counters; the journal adds `boot_epoch` to every row, making
  `(boot_epoch, counter)` the durable identity. Full 128-bit migration
  for these is DEFERRED with falsifiable trigger: a second host process
  sharing one journal (multi-workspace) — not in the MVP profile
  (ARCH §3.1), revisit then.
- Digest law unchanged: SHA-256 content digests for integrity; fnv1a64
  only as non-security identity/bucketing (ARCH §10.3).

## 7. Journal subsystem (MVP-1) — `journal/`

### 7.1 SQLite binding (D-2)

Vendored amalgamation `qiven::tp::sqlite3` (§17) used through a thin
RAII wrapper — **no interface abstraction over `sqlite3*`** (a seam
without a second engine is speculative; the wrapper is the seam).

```cpp
class JournalDb {                    // journal/runtime_journal.hpp
public:
    static qiven::Result<JournalDb> open(const std::filesystem::path& file,
                                         JournalOpenIntent intent); // 51/54 on corrupt
    // owned statements, single control-thread discipline asserted in debug
    qiven::Result<void> txn(std::invocable<Stmts&> auto&& body);    // BEGIN IMMEDIATE
private:
    sqlite3* m_db {};                // never leaked; closed in dtor (no throw)
};
```

Configuration on open (ARCH §10.1): `journal_mode=WAL`,
`synchronous=FULL`, `foreign_keys=ON`, `busy_timeout=1500` (bounded —
single-writer discipline makes contention a defect, not a wait), and a
fixed page cache. `quick_check` at startup; `integrity_check` under
`doctor` only (bounded operational policy).

### 7.2 Location and files (GR-5)

`<repo>/.qiven/runtime/journal.sqlite3` (+ `-wal`, `-shm`),
`client.secret.dpapi` (MVP-3), `bundles/` (MVP-2). Rationale: inside the
workspace (owner constraint), per-checkout identity (the profile governs
exactly one checkout — ARCH §3.1), durable across rebuilds, excluded
from git by `.gitignore` entries landed with MVP-1. Quarantine flag is
a row in `runtime_meta` (never a sidecar file that could desync).

### 7.3 Physical schema (first cut; migrations append-only)

> **Correction pointer (MVP-1, 2026-09-23):** the DDL below contains two
> defects found by the batch design's pre-implementation review
> (`docs/design/mvp1-journal.md` §1) and corrected in the landed schema
> (`src/journal/schema.cpp`): (1) the `decisions` CHECK references a
> `state` column the column list never declared — the landed schema
> declares `state TEXT NOT NULL CHECK(state IN ('bound','consumed',`
> `'stale'))` with `CHECK((state='consumed') = (consumed_ms IS NOT NULL))`;
> (2) `transaction` is an SQLite keyword and must be quoted in DDL and
> DML. Lease rows are never deleted (expiry derived; the row is the
> fencing-epoch high-water mark). The DDL text here is retained as the
> reviewed historical basis.

```sql
-- schema_version 1 (MVP-1); migrations are validated, never silent
CREATE TABLE runtime_meta(key TEXT PRIMARY KEY, value TEXT NOT NULL);
  -- schema_version, install_id, boot_epoch, quarantine, last_wall_ms
CREATE TABLE generations(id INTEGER PRIMARY KEY, bundle_digest BLOB NOT NULL,
  profile_digest BLOB NOT NULL, build_id TEXT NOT NULL, activated_ms INTEGER NOT NULL,
  status TEXT NOT NULL CHECK(status IN ('active','retired')));
CREATE TABLE sessions(id INTEGER PRIMARY KEY, generation INTEGER NOT NULL REFERENCES generations(id),
  actor INTEGER, harness TEXT, opened_ms INTEGER NOT NULL, closed_ms INTEGER);
CREATE TABLE transactions(id INTEGER PRIMARY KEY, boot_epoch INTEGER NOT NULL,
  causal_parent INTEGER, correlation TEXT NOT NULL, request_digest BLOB NOT NULL,
  base_revision TEXT, state TEXT NOT NULL, UNIQUE(boot_epoch, id));
CREATE TABLE decisions(id BLOB PRIMARY KEY,            -- SortableId128 bytes
  token_hash BLOB UNIQUE NOT NULL, transaction INTEGER NOT NULL, generation INTEGER NOT NULL,
  binding_digest BLOB NOT NULL, expires_ms INTEGER NOT NULL, consumed_ms INTEGER,
  CHECK((consumed_ms IS NULL) = (state='bound')));     -- single consumption at the storage layer
CREATE TABLE leases(workspace TEXT PRIMARY KEY, holder_install TEXT NOT NULL,
  boot_epoch INTEGER NOT NULL, fencing_epoch INTEGER NOT NULL, acquired_ms INTEGER NOT NULL,
  expires_ms INTEGER NOT NULL);
CREATE TABLE dispatches(id BLOB PRIMARY KEY, transaction INTEGER NOT NULL,
  plan_digest BLOB NOT NULL, state TEXT NOT NULL, started_ms INTEGER NOT NULL, done_ms INTEGER,
  CHECK(state IN ('prepared','dispatched','succeeded','failed','indeterminate','quarantined')));
CREATE TABLE outcomes(dispatch BLOB PRIMARY KEY REFERENCES dispatches(id),
  status TEXT NOT NULL, ref_observed TEXT, validator_digest BLOB, observed_ms INTEGER NOT NULL);
CREATE TABLE barriers(scope TEXT PRIMARY KEY, reason TEXT NOT NULL, opened_ms INTEGER NOT NULL,
  closed_ms INTEGER, evidence TEXT NOT NULL);
CREATE TABLE audit_events(seq INTEGER PRIMARY KEY AUTOINCREMENT, prev_hash BLOB NOT NULL,
  event_hash BLOB NOT NULL, kind TEXT NOT NULL, payload BLOB NOT NULL); -- append-only hash chain
```

The logical tables of ARCH §10.2 are all present; normalization choices
(e.g. separate `evidence_receipts` table landing with resolver
persistence in MVP-1's second slice) are recorded in `journal/schema.hpp`
as the schema evolves — the invariant "MUST NOT collapse back into the
thin image" (ARCH §10.2) is checked by a schema-conformance test
(§16).

### 7.4 Command API and audit chain

State transitions are journal COMMANDS only (callers never write rows):
`open_transaction`, `accept_evidence`, `bind_decision`,
`consume_decision`, `acquire_lease`, `record_dispatch_prepared`,
`record_outcome`, `open_barrier`, `close_barrier`, `advance_generation`,
`open_session`. Each command is one `BEGIN IMMEDIATE` transaction that
also appends its audit event; the chain law is
`event_hash = SHA256(prev_hash || kind || canonical_payload)` with
`prev_hash` read inside the same transaction. Verification rehashes the
full chain at startup and under `doctor` (fail → quarantine, ARCH §10.4).

### 7.5 Crash-injection framework (exit-gate backbone)

`journal/recovery.hpp` exposes a debug-only hook set:

```cpp
#if defined(QIVEN_RUNTIME_TEST_CRASH_POINTS)
using CrashPoint = enum { after_create_tx, after_evidence_persist,
    after_decision_persist, after_token_consume, after_lease_fence,
    after_dispatch_commit, after_ref_cas, after_outcome_commit, /* …all §16.3 points */ };
void arm_crash_point(CrashPoint, CrashMode mode);  // die_hard | die_before_commit | die_after_commit
#endif
```

The crash test (`tests/journal_crash_recovery.cpp`) is a self-spawning
pattern: the test executable re-execs itself with
`QIVEN_TEST_CRASH_AT=<point>`; the child arms the hook, performs a
scripted transaction, dies at the point (hard `_exit` — no unwinding,
mimicking power loss); the parent then reopens the journal and asserts
the DETERMINISTIC recovery classification for that point (each §16.3
point names its authoritative source and its one permitted outcome).
"The process restarted" is never an assertion (ARCH §16.3).

## 8. Cognition bundle & publisher (MVP-2) — `cognition/`

- `cognition/publisher.hpp`: `publish(PublishRequest{repo_root, ref})`
  runs Git plumbing through the process runner (§11): `git rev-parse
  <ref>^{commit}` → `git ls-tree -r <tree>` → `git cat-file --batch`
  streaming exact blobs. It NEVER reads the working tree (ARCH §7.1).
  Output bundle layout per ARCH §7.2 under `.qiven/runtime/bundles/<digest>/`,
  written to a temp dir then atomic-rename; the active pointer
  `.qiven/runtime/bundles/ACTIVE` is a symlink-free single-line file
  swapped by rename.
- Every manifest-named file is re-hash-verified on load
  (`ICognitionBundlePort::pin_from_bundle`, landed MVP-0 as port).
- `invocation-policy.yaml` machine instance: qiven-context owns the
  file; the runtime parses ONLY the strict subset it defines (mapping
  table + boundaries + freshness + digests) with a bounded line parser
  (D-4: the runtime never gains a general YAML parser); reference-check
  against the ADL happens in qiven-context's validators, not here.
- RuntimeGeneration minting composes bundle digest + profile digest +
  build id (existing `GenerationMinter`, now journal-backed); generation
  change invalidates unconsumed decisions (storage-layer UNIQUE +
  stale-marking at boot, §7.4).

## 9. Profile subsystem (MVP-2) — `host/deployment_profile.hpp`

The accepted profile instance for the MVP is fixed:
`zcode-jason-context-record-mvp` (ARCH §3). File-backed profile loading
(`IProfilePort`, landed MVP-0) validates: non-empty explicit
`ResourceScope` enumeration, governed paths accepted by the current
qiven-context schemas, actor set non-empty, conformance-evidence
reference present, mediation inventory covering every FileSystemWrite
producer (complete-mediation precondition, ARCH §3.3). Profiles are
loaded, never synthesized; the in-code `ProfileBuilder` remains for
tests only.

## 10. IPC subsystem (MVP-3) — `ipc/`

- **Transport**: named pipe `\\.\pipe\qiven-runtime\<install-id>\v1`,
  `FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED`, DACL =
  owner SID only (`ConvertStringSecurityDescriptorToSecurityDescriptor`
  with `SDDL` "OW" — owner-only). Client validation: `GetNamedPipeClientProcessId`
  → open process → query owner SID + image path → both must match the
  installation record (ARCH §12.1).
- **Secret**: 256-bit CSPRNG secret at first boot, persisted via
  `CryptProtectData` (DPAPI CurrentUser) in `.qiven/runtime/client.secret.dpapi`.
- **Framing** (`ipc/framing.hpp`): `[u32 magic 'QVR1'][u16
  proto_version=1][u16 flags][u64 body_len][u64 request_id][u64
  connection_seq][HMAC-SHA256 over header+body]` then UTF-8 JSON body.
  Limits: body ≤ 1 MiB, depth ≤ 16, connection_seq strictly monotonic,
  timestamp window ±120 s with replay cache of the last 256 nonces
  (bounded LRU). Violations → typed IPC errors 62/63/64, connection
  dropped.
- **Handshake**: first frame `hello{client_kind, client_build,
  deadline_ms}`; version mismatch → explicit typed rejection (never a
  hang inside the hook deadline, ARCH §12.1).
- **Hook response mapping** (`zcode_hook_main.cpp`, MVP-4): Allow →
  exit 0 empty; Deny → the ZCode deny JSON with reason code;
  NotGoverned → exit 0 with telemetry field; ReDeliberate → deny with
  `resume_context` + one-time token. Host timeout/unreachable/version
  mismatch inside a hard-governed scope → **Deny** (fail-closed,
  ARCH §6.2); outside governed scope → NotGoverned honesty.
- JSON codec is the bounded first-party `jsonx/` (D-1); frames are
  diagnosable text (ARCH §12.1 rationale preserved).

## 11. Process runner (MVP-3) — `processx/` (the minimal qiven-process slice)

One class, one purpose (ARCH §6.5): run an allowlisted executable with
an explicit argv, bounded lifetime, and captured outputs.

- `CreateProcessW` with `EXTENDED_STARTUPINFO_PRESENT` +
  `PROC_THREAD_ATTRIBUTE_JOB_LIST`: Job Object with
  `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`; no console inheritance; no
  inherited stdin (a closed pipe is attached).
- Environment: an EXPLICIT allowlist built by the caller (SystemRoot,
  PATH-restricted-to-pinned-tools, TMP pointing into the transaction
  temp dir, GIT_* allowlist for plumbing, locale); anything not listed
  is absent (ARCH §12.3).
- stdout/stderr: two dedicated reader threads with fixed ring buffers
  (default 4 MiB each); on overflow the run fails typed 73 (output
  limit) — never unbounded growth, never a blocked child.
- Deadline ladder: `deadline_ms` → `TerminateProcess` (graceful console
  signals are not reliable for arbitrary children; the ladder is
  wait → kill-tree) → classification `TimedOut` with partial-output
  digests (73).
- Result: `{exit_code, termination_reason, stdout_digest, stderr_digest,
  durations_ms}` — bytes are digested, only bounded tails are retained
  for diagnostics.
- Allowlist: executable absolute path + expected product version or
  SHA-256, recorded in the profile; a mismatched binary is a typed 74
  denial (ARCH §16.4 executable-replacement test drives this).

## 12. Hook adapter (MVP-4) — `apps/zcode_hook_main.cpp`

Thin client per ARCH §6.2: read stdin event, envelope, submit, map
response, exit within the hook deadline (deadline budget minus fixed
250 ms safety margin; a slow host yields Deny-not-hang). Event surface:
SessionStart / PreToolUse / PostToolUse only. Write-capable tool
enumeration is data shipped with the profile (checked by conformance
tests), and unknown tools fail closed. The current bridge app remains
the conformance fixture (ARCH §6.2).

## 13. Record subsystem (MVP-5) — `record/`

- `record/command.hpp`: the four operations (`memory.add`,
  `obligation.create`, `obligation.transition`, `active_work.patch`,
  `session_checkpoint.write` — five surfaces, four families) as tagged
  variants; request envelope `{schema:"qiven-record-request-v1",
  request_id, workspace_id, expected_base, operations[]}` parsed by
  `jsonx` with total-bytes/depth/count limits and unknown-field
  rejection; `request_id` idempotency at the journal layer (§7).
- `record/domain.hpp`: typed domain objects; the runtime assigns ids
  and timestamps; Unicode normalization = NFC on semantic text fields,
  recorded in the rendered file header.
- `record/renderer.hpp` (D-4): a deterministic EMITTER for exactly the
  schemas qiven-context defines (front matter subset + Markdown body +
  YAML state files): UTF-8, LF, exactly one trailing newline, no
  trailing whitespace, fixed key order per schema, pinned emitter
  version stamped in each file's generated-by line. Golden-file tests
  pin bytes; identical input + generation + renderer build → identical
  candidate tree (ARCH §9.2). The runtime NEVER parses general YAML;
  validation belongs to the existing qiven-context validator executed
  through the process runner (ARCH §9.3 step 6).
- `record/index.hpp`: index derivation as a pure function of the record
  set (sorted, digest-stamped); indexes are never directly editable
  through the request surface.
- `gitx/candidate_transaction.hpp`: isolated temp index
  (`GIT_INDEX_FILE` inside the transaction temp dir): `read-tree
  <base>` → apply typed operations as blob writes (`hash-object -w`)
  → rebuild affected indexes → `write-tree` → validator run (process
  runner) → commit-tree with attribution trailer (existing Role/LLM
  conventions) → **CAS**: `git update-ref refs/heads/<branch> <new>
  <expected-old>` (the two-value form IS compare-and-swap; mismatch =
  typed 92, never overwrite — ARCH §9.3) → observe ref → projection
  update via `checkout -- <paths>` guarded by a preimage
  `diff --quiet` check; uncommitted user changes → `DirtyProjection`
  barrier, never destructive reset (ARCH §13.3).
- Commit object existence without an authorized ref pointing at it =
  unpublished, not success (typed classification, ARCH §16.2).

## 14. ExecutionAuthority (MVP-6) — `authority/`

Lease + fence live in the journal (§7.3 `leases`): acquire =
`UPDATE leases SET …, fencing_epoch = fencing_epoch + 1 WHERE
workspace = ? AND (expires_ms < now OR holder = me)` inside the
dispatch-preparing transaction — epoch increments ONLY inside that
transaction (monotonic by construction; the exit-gate test proves
"never decreases" across crash/restart). Fence revalidation
immediately before ref CAS: re-read epoch in a fresh read transaction;
a changed fence → typed 92-class denial + the dispatch is marked
`indeterminate` + barrier (ARCH §11.1/§11.2). Durability order,
reconciliation classes, and complete PostActionObserver correlation are
already specified by ARCH §11 and landed in observer form by MVP-0;
MVP-6 wires them to the journal + real Git.

## 15. Observability (MVP-7 prep)

Metrics are journal-backed counters + a bounded JSONL event stream at
`.qiven/runtime/metrics.jsonl` (size-capped, rotated at 4 MiB, two
generations). The ARCH §17.1 list maps 1:1 to event kinds; `runtimectl
doctor` renders them. Validator and refresh latencies are reported
independently (ARCH §17.2). No daemon, no background sampler — events
are emitted on the control thread at their natural points.

## 16. Test strategy per batch (exit-gate spine)

Existing law: one self-contained executable per topic,
`QIVEN_VERIFY`, no framework churn, header self-containment checks,
Debug+Release both green. New required tests (each names its ARCH
exit-gate row):

| Test | Proves (ARCH) |
| --- | --- |
| `journal_crash_recovery` (self-spawning, §7.5) | §16.3 all 13 points; §15 MVP-1 exit gate |
| `journal_schema_conformance` | §10.2 no-collapse invariant; migration law |
| `audit_chain_verify` | §10.4 chain verify + quarantine on damage |
| `bundle_publish_pin` | §15 MVP-2 gates (dirty checkout, digest mismatch, revision≠digest) |
| `profile_accept` | §3.2/§3.3 acceptance rules fail-closed |
| `pipe_frame_security` | §16.4 replay/oversize/HMAC/ACL |
| `process_runner_bounds` | §16.4 flooding/hang/tree-reclaim |
| `hook_conformance` (real-ZCode H1 cases enumerated, run under owner H1) | §15 MVP-4 gates |
| `record_renderer_golden` | §9.2 determinism |
| `candidate_cas` | §16.2 atomicity rows |
| `lease_fence_race` | two-host competition, epoch monotonicity |

H1 boundary: real-ZCode hook tests (MVP-4) and the MVP-7 dogfood
installation are owner-hands acceptance points — the session prepares
everything paste-ready (H1-preparation duty).

## 17. Third-party dependencies (Devkit standard applied)

| Slot | Artifact | Source | Notes |
| --- | --- | --- | --- |
| TP-1 | SQLite 3.53.4 | workspace singleton `qiven-third-party-win/packages/sqlite3` (class S) | consumed per the Devkit third-party standard v2: `QIVEN_THIRD_PARTY_ROOT` resolution + exact-SHA pin (`QIVEN_THIRD_PARTY_PIN`) + consumer spot-verification gate task; target `qiven::tp::sqlite3` with the standard's scoped flag law (`/W3`, `_CRT_SECURE_NO_WARNINGS` PRIVATE, `SQLITE_THREADSAFE=1`, `SQLITE_OMIT_LOAD_EXTENSION`, `SQLITE_OMIT_DEPRECATED`, `SQLITE_DEFAULT_WAL_SYNCHRONOUS=1`, no PCH). |

Additions require: a singleton package (provenance + per-class CMake
under the standard), an entry here, the consumer pin move, and a §16
test touching the dependency. (D-1/D-4 keep JSON, YAML-emission,
crypto, and process management first-party on purpose.)

## 18. Deployment surface

Products ship per the Devkit deployment standard (workspace-bounded
bundle: `bin/` + `docs/` + `licenses/` + digest manifest, smoke-run
from the bundle). MVP-0 already validated the mechanism with the
runtime library + apps bundle (validation record in the bundle and the
v18 checkpoint). RuntimeHost-specific operational runbooks land with
MVP-3+ batches (ARCH §17.3 list).

## 19. Deferred items (falsifiable, per constitution §15)

| # | Deferral | Failure signal / revisit trigger |
| --- | --- | --- |
| D-1 | first-party bounded JSON codec (no vendored parser) | a profile/interop need for JSON Schema validation or measured codec hotspots > 5 % of control path p95 |
| D-4 | deterministic YAML EMITTER subset (no general YAML in runtime) | any requirement to PARSE arbitrary YAML inside the runtime (none exists in the MVP) |
| D-8 | wall-clock + 300 s regression window policy | a real clock-rollback incident or dual-boot skew reports |
| D-10 | u64+boot_epoch ids for generation/transaction | a second host process sharing one journal (out of MVP profile) |
| — | CBOR IPC framing | ARCH §12.1 already defers; trigger = measured frame cost on hook p95 |

## 20. Compliance map (ARCH → this design)

| ARCH | Section here |
| --- | --- |
| §3 profile/scope/claim | §9, scope.hpp (MVP-0) |
| §4 invariants 1–14 | §2 GR-3/GR-4, §6, §12–§14, MVP-0 tests |
| §6 components | §3 topology, §10–§13 per-app |
| §7 cognition bundle | §8 |
| §8 control path + §8.2/§8.3 | landed MVP-0 (state.cpp/resolver.cpp) + §7.4 |
| §9 record transaction | §13 |
| §10 journal | §7 |
| §11 authority/unknown outcomes | §14 + observer (MVP-0) |
| §12 IPC/security | §10, §11, §12 |
| §13 state machines/recovery | §4, §7.5 |
| §14 layout/corrections | §3 migration law; MVP-0 landed the corrections |
| §15 batch gates | §16 table |
| §16 verification matrix | §16 + per-batch tests |
| §17 observability | §15 |
| §18 DoD | §16 + MVP-7 runbook items |

## Review record

Self-review 2026-09-23 (authoring session, pre-publication):

1. *D-2 vs implementation-standard "minimal abstraction"* — the
   JournalDb wrapper IS the seam; confirmed no interface layer added.
2. *§7.3 schema*: `sessions.actor` nullable contradicts nothing (loopback
   sessions day-one); noted for MVP-3 tightening. Checked ARCH §10.2
   table-by-table — complete.
3. *§10 replay window ±120 s* interacts with D-8 clock regression: the
   replay window must clamp to the regression policy — fix folded in
   (connection timestamps validated against journal wall time, not only
   peer time).
4. *§11 graceful termination*: ARCH asks for "graceful termination, and
   forced termination" — on Windows the honest graceful story for
   console children without cooperation is limited; documented the
   ladder (wait → kill-tree) and the deliberate omission of
   console-signal injection (unreliable); revisited when a governed
   child declares a graceful mode (validator CLI may, MVP-5).
5. *§13 five surfaces / four families* — ARCH §6.3 lists four
   operations; `session_checkpoint.write` is the fifth surface within
   the same family class; wording aligned.
6. Verified every MUST in this document has a named test or exit-gate
   row (spot-checked §7.5/§10/§11/§13/§14).
7. Confirmed no second source of truth introduced: journal = control
   facts only; bundle = derivative; Git = domain truth (ARCH §2/§5
   planes preserved).
