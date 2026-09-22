# Runtime Production Activation (RPA) — Program Proposal

Status: **proposal** (owner-endorsed direction 2026-09-22 in
conversation; formal acceptance and batch authorization expected at the
next session's cold boot). Program home: qiven-runtime. Context record:
qiven-context v15 session close (2026-09-22).

## 1. Problem

The Runtime's cognitive-control semantics are proven (RCA-0..16
complete), but daily production use still routes MECHANICAL work
through the LLM as a typist for machine-checkable formats. The v15
session's failure taxonomy is the evidence baseline:

| Failure class | Evidence (v15 session) | Root cause |
| --- | --- | --- |
| YAML serialization | bare colons, CRLF trailing whitespace (>=6), python writes producing CRLF | LLM hand-authors bytes; the validator catches AFTER the wrong write |
| Index synchronization | memory/index title vs frontmatter mismatch | hand-maintained dual write surface |
| Template completeness | checkpoint literal-phrase misses (2) | schema constraints carried in LLM memory |
| Shell/escape corruption | heredoc truncation x2, byte corruption, literal newlines in constants | file content crossing shell parse layers |
| Git ceremony | ~93 PRs x 6-8 git/gh calls = 600+ mechanical tool calls | correct process, but every step costs a full-history LLM round-trip |
| Gate retry cycles | each schema failure cost 2-3 repair+rerun rounds | detection-after-write |

None of these are judgment failures: the CONTENT was always right, the
SERIALIZATION was wrong. The goal is not "the gate catches errors" but
"invalid state is unwritable by construction".

## 2. Principle

**The LLM emits intent (typed fields); the mechanism emits bytes.**

Every operation that must pass a schema check becomes a typed operation
whose implementation validates input, serializes canonical output, and
runs the validator INSIDE the write path. The LLM never authors YAML or
markdown bytes for schema-governed records again.

## 3. Layer A — mechanical record layer

`qiven-record` CLI (operator conventions; links the runtime library):

- `memory add --kind --title --body ...` (auto-updates the index;
  title consistency by construction)
- `state set <path>=<value>`, `state checkpoint ...` over
  active-work/current
- `checkpoint new/refresh` (template-embedded mandatory sections; the
  cold-boot literal phrases become unwritable-missing)
- `obligation transition ...`

Each operation: input schema validation -> canonical serialization
(LF, quoting, no trailing whitespace) -> atomic write -> validator in
the write path. Scope by friction order: state/active-work, memory+
index, session checkpoints — exactly the v15 failure set.

Positioning vs the qiven exec hook: the hook is the HANG backstop and
stays; the record layer is the CORRECTNESS mechanism. Orthogonal; both
remain.

## 4. Layer B — store layer and the git role split

Three tiers, authority model UNCHANGED:

```text
LLM/tools -> local store   (working surface: typed read/write,
                            immediate, no process spawn per op)
              | periodic checkpoint (CAS semantics:
              |   Committed / CompareFailed / OutcomeUnknown)
              v
            git local commit (durability snapshot)
              | publication boundary (unchanged: push -> PR -> owner H2)
              v
            GitHub remote   (canonical authority — governance intact)
```

Legality: the working tree was ALWAYS non-authoritative; the store is a
more efficient working surface, not an authority cutover. Merge-class
publication stays H2 with merge-proof. Checkpoint cadence: gate PASS OR
time threshold OR session boundary (whichever first). Divergence uses
the frozen v4 CAS semantics (ICognitionStore/StoreReceipt — the draft
defined them for exactly this).

## 5. Extensions

1. **Real ExecutionAuthority subsystem** (RPA-4): RCA-13's Null denies
   RealMutation fail-closed; production activation implements the real
   RuntimeHost-internal single-writer lease over the file-backed store
   as journal — every record-layer write becomes a genuinely governed
   machine-local mutation, not a bypass.
2. **CLI-first, no daemon yet** (operator philosophy; a resident host
   waits for a concrete need: subscriptions, session-resident
   interception).
3. **Metric closure**: the v15 failure taxonomy is the baseline; RPA
   acceptance compares schema-failure count, gate-retry rounds, and
   mechanical call count before/after.

## 6. Anti-patterns (explicitly out)

- No governance change: store checkpoint != canonical publication.
- Gates stay: they retire from error-catching to acceptance-proof.
- No daemon/plugin framework before concrete need.
- No hand-written serializers for new schema formats (typed operations
  only).
- The Python validators remain the acceptance authority.

## 7. Batch plan

| Batch | Content | Depends on |
| --- | --- | --- |
| RPA-1 | file-backed RuntimeStateStore + restart journal (ADL §57 production store; extends RCA-12's serialized image) | — |
| RPA-2 | qiven-record: state/memory+index/checkpoint ops; validator in write path | RPA-1 |
| RPA-3 | store<->git checkpoint cadence + CAS reconciliation + publication boundary | RPA-1/2 |
| RPA-4 | real ExecutionAuthority subsystem (Null -> real single-writer lease) | RPA-1 |
| RPA-5 | daily-loop switchover (this workspace's context ops move to the record layer) + metric comparison | all |

Standard batch discipline throughout (branch / gate / PR / delegated H2
/ verified merge).

## 8. Decision points for the owner

1. Accept RPA as the next program (this proposal).
2. Batch order: as listed (RPA-1 first, foundation-smallest), or RPA-2
   first for fastest felt relief.
3. Checkpoint cadence thresholds (proposed: on-gate-PASS, 30 minutes,
   session boundary).
