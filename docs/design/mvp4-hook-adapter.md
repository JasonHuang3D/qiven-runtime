# MVP-4 — Production ZCode Hook Adapter (batch design)

Status: **batch design for MVP-4** (design-first standard; extends
`docs/architecture/runtime-production-mvp-cpp-design.md` — **DESIGN** —
§12, §10, §4, §3 topology; implementing
`docs/architecture/runtime-production-mvp-architecture.md` — **ARCH** —
§6.2, §7.4, §12.1/§12.2, §13, §15 MVP-4, §16 verification rows; and
closing the MVP-3 deferrals H-2 (SessionStart refresh + freshness
window) and H-4 (`shutdown` request kind) recorded in
`docs/design/mvp3-host-ipc.md` §8).

Batch: MVP-4 of OBL-20260922T155800Z-9A7B41. Exit gate (ARCH §15):

1. a real ZCode H1 test proves deny has no fallthrough (owner-H1
   acceptance point; the session prepares the kit);
2. Bash, Write, Edit, and every discovered write path cannot bypass
   governed resources;
3. unknown tools, fields, and host versions fail closed;
4. repeated real actions receive unique IDs and PostAction performs
   complete correlation;
5. an uninstalled or disconnected hook is never reported as governed.

---

## 1. Basis and recorded deltas

### 1.1 Basis

- ARCH §6.2 (thin adapter law: no classification, no counters, no
  decisions, no failure-to-allow), §7.4 (SessionStart refresh rules),
  §12.1 (hook response mapping, deadlines), §12.2 (identity boundary:
  host assigns session IDs), §13.1 (transaction states), §15 MVP-4
  work list, §16 verification matrix.
- DESIGN §12 (hook adapter shape, 250 ms safety margin), §10 (IPC
  kinds/response mapping), §5 (error taxonomy), §16 (`hook_conformance`
  test spine row).
- MVP-3 landed: pipe + framing + HMAC/DPAPI identity + install-record
  client validation + deadline handling; `PipeClient`;
  `encode_request_body`/`decode_reply`; host `handle()` with the
  mutation placeholder (61-always).
- MVP-0 control core landed: ObservedAction/IntentSet/DecisionBinder
  value types, journal commands (`open_transaction`, `bind_decision`,
  `consume_decision`, `open_session`, audit chain).
- Real ZCode hook contract facts (ground truth used by this design):
  registration shape `hooks.events.{SessionStart,PreToolUse,PostToolUse}`
  with `matcher` + `command` + per-hook `timeout` (observed live in the
  workspace `.zcode/config.json` running the devkit exec router);
  hook stdin carries the event JSON (session id, tool name, tool
  input); a PreToolUse hook DENIES the tool call by exiting non-zero
  with the reason on stderr (observed live this workspace: the
  `[qiven-hook]` denial blocks the tool call and the stderr text
  reaches the model); hooks load at session start (no hot reload).

### 1.2 Recorded deltas (not silent deviations)

1. **The hook parses the event payload at LOCATE grade, not validate
   grade.** DESIGN §12 says "read the host event from stdin"; the real
   ZCode payload may contain signed/float numbers jsonx rejects by
   design (u64-only law). The hook therefore does NOT full-parse the
   payload: it (a) digests the raw bytes VERBATIM (bridge precedent —
   the argument digest binds exactly what the harness handed over),
   and (b) extracts the explicit fields it needs with a bounded
   state-machine scanner (`adapter/zcode_event.hpp`). The scanner is
   string/escape/depth-aware for SKIPPING, not validating; the
   governed surface on the wire is the digest + extracted fields,
   which the host decodes strictly. Full-document schema validation of
   harness payloads is explicitly NOT a claim of this batch.
2. **Bash mediation is a conservative text-reference detector, exact
   for Write/Edit.** Write/Edit expose the exact target path
   (`tool_input.file_path`); Bash exposes only a command string, and
   no static analysis of shell text proves write intent. The detector
   DENIES any Bash command whose text references a governed path
   prefix or the governed checkout root (over-approximation:
   `grep state/ x` denies alongside `echo > state/x`). The residual
   (obfuscated/indirect writes the text never names) is recorded in
   the profile's mediation claim as an explicit detector-scope gap,
   measured by MVP-7 dogfood — NOT claimed away. Complete mediation is
   claimed for the BOUNDARY (every Bash/Write/Edit invocation passes
   the PreToolUse gate and a deny is enforced with no fallthrough),
   with the Bash path-classifier scope stated honestly (ADL §11
   honesty law).
3. **Governed writes via raw tools are DENIED in MVP-4 (not
   re-routed).** The mediated typed path (`qiven-record`) is MVP-5; in
   MVP-4 a Bash/Write/Edit action whose target intersects the governed
   scope receives Deny with a reason naming the mediated path. The
   deny carries the re-deliberate context shape (reason + guidance) so
   MVP-5 can turn it into a real re-deliberate flow without changing
   the hook.
4. **`runtimectl` gains one non-read verb: `host shutdown`** (H-4
   closure). ARCH §6.4 lists the initial read-only verbs; H-4 names
   the hook lifecycle as the reason `shutdown` exists over IPC, and an
   operator-side client is required to exercise it. The verb is
   authenticated IPC + journaled audit event; recorded here as a
   deliberate ARCH §6.4 extension, not a silent scope creep.
5. **Host outcomes for harness-dispatched actions ride audit events in
   MVP-4.** The journal command set is unchanged (DESIGN §7.4); there
   are no dispatch rows because the "dispatch" of an allowed hook
   action is ZCode executing the tool, not the runtime. PreToolUse
   admit = `open_transaction` + `bind_decision` + `consume_decision`
   (single-use law: the allow is consumed when ZCode is told "allow");
   PostToolUse = outcome audit event + transaction Closed/Indeterminate.
   MVP-6 wires real dispatches; the correlation law (one pre, one
   post; duplicate post = typed integrity conflict; missing post =
   Indeterminate) is enforced at the host now.

## 2. Module map

```text
include/qiven/runtime/adapter/zcode_event.hpp   locate-grade event field
                                                extraction + verbatim digest
include/qiven/runtime/adapter/zcode_hook.hpp    the thin client library:
                                                build request, transact over
                                                pipe, map verdict to exit
src/adapter/{zcode_event,zcode_hook}.cpp
apps/zcode_hook_main.cpp                        qiven-zcode-hook entrypoint
src/ipc/protocol.cpp  (+HookEvent, +Shutdown kinds, verdict/reason tables)
src/host/runtime_host.cpp  (session registry, hook handling, refresh,
                            shutdown drain; H-2/H-4)
src/cognition/publisher.cpp (+bounded fetch entrypoint, H-2)
src/host/deployment_profile.cpp (+tool inventory + mediation scope v2)
config/profiles/zcode-jason-context-record-mvp.yaml  (revision 2)
apps/runtimectl_main.cpp (+`host shutdown`)
tests/hook_conformance.cpp  (exit-gate rows 2-5 local part + H-2/H-4)
tests/headers/adapter_zcode_event.cpp, adapter_zcode_hook.cpp
CMakeLists.txt (qiven-zcode-hook target), .qiven/deploy.json
docs/engineering/mvp4-h1-kit.md (owner-H1 real-ZCode kit)
```

Out of scope: `qiven-record` client (MVP-5), dispatch/lease/fencing
(MVP-6), connection pool >1 (H-3, unchanged trigger), lazy host start
from SessionStart (§8 deferral), CBOR framing (DESIGN §19).

## 3. Contracts

### 3.1 Event extraction (`adapter/zcode_event.hpp`)

```cpp
namespace qiven::runtime::adapter {
struct ZcodeEventFields {
    std::string session_handle;   // harness session id (EVIDENCE ONLY)
    std::string event_name;       // cross-check against the --event flag
    std::string tool_name;        // "Bash" | "Write" | "Edit" | other
    std::string command;          // Bash: tool_input.command (if present)
    std::string file_path;        // Write/Edit: tool_input.file_path
    qiven::runtime::ContentDigest payload_digest;  // sha256, raw bytes
    usize payload_bytes = 0;
};
// Locate-grade scan: string/escape/depth-aware skipper; input cap
// 1 MiB, depth cap 16; missing optional fields are empty, not errors.
// The event_name/tool_name/session_handle keys are REQUIRED for the
// request to be built; their absence is a typed parse failure (deny).
[[nodiscard]] qiven::Result<ZcodeEventFields, i32> extract_zcode_event(
    std::span<const std::byte> raw);
}
```

### 3.2 Protocol additions (`ipc/protocol.hpp`)

Request kinds `HookEvent` and `Shutdown` join the envelope (bodies
stay bounded jsonx, flat, u64/string/bool/array-of-string; unknown
kinds/fields still fail closed 64).

```text
hook_event {event: "session_start"|"pre_tool"|"post_tool",
            session_handle: <string>, tool_name: <string>,
            payload_sha256: <hex>, payload_bytes: N,
            command?: <string>, file_path?: <string>,
            mediated_tools: ["Bash","Write","Edit"],   // session_start only
            manifest_note: <string>,                    // session_start only
            deadline_ms: <= 5000 (pre/post) | <= 10000 (session_start)}

hook_ack {verdict: "allow"|"deny"|"not_governed"|"degraded",
          session_id: <hex16>,        // host-assigned (host minted; empty
                                      // for pre/post on unknown sessions
                                      // → verdict deny 44)
          action_id: <hex16>,         // pre_tool/post_tool; host-assigned
          reason_code: N, reason_detail: <string>,
          refresh: "current"|"local_fallback"|"expired",  // session_start
          generation: N}

shutdown {grace_ms: <= 30000} → shutdown_ack {draining: true}
```

Verdict reason codes (new taxonomy range 110–119, DESIGN §5 extension,
recorded here): 110 governed-write-requires-mediated-path; 111
bash-governed-reference (conservative detector); 112 unknown-tool
fail-closed; 113 manifest/scope mismatch; 114 unknown-session
(register first); 115 correlation conflict (duplicate post); 116
host-unavailable-classification (the hook's own fail-closed code when
the host is unreachable — issued client-side, never by the host; since
the 2026-09-24 corrective lane it covers only GENUINELY UNKNOWN
transport shapes — the classable causes carry their own codes, §7); 117
cognition-expired (freshness window exceeded — closes H-2's deny
path); 118 payload-too-large / unparseable (client-side); 119
shutdown-in-progress; 120 no-listener; 121 admission-rejected;
122 version-skew; 123 secret-skew; 124 timeout (corrective-lane
split — disjoint, diagnosable; see §7).

Mapping to the existing deny family: 117 rides typed IPC error 66
(new: cognition-expired) at the mutation boundary; all others are
hook-domain reasons inside a normal `hook_ack` deny verdict.

### 3.3 The thin client (`adapter/zcode_hook.hpp`)

```cpp
namespace qiven::runtime::adapter {
struct HookRun {
    std::filesystem::path runtime_root; // <governed checkout>/.qiven/runtime
    std::string event;                  // session_start|pre_tool|post_tool
    std::span<const std::byte> payload; // stdin bytes, verbatim
    u64 deadline_ms = 4500;             // ZCode budget minus 250 ms margin
    u64 now_ms();
};
struct HookOutcome {            // everything main() needs to exit
    int exit_code;              // 0 allow/not_governed/degraded; 2 deny
    std::string stderr_text;    // deny reason / honesty notes
};
[[nodiscard]] HookOutcome run_zcode_hook(const HookRun&);
}
```

Laws: no classification, no counters, no session/action minting, no
policy data, no caching (one-shot process; state lives in the host).
Fail-closed mapping: for `pre_tool`, ANY failure to obtain a verified
`hook_ack` → exit 2 with the CLASSIFIED reason (120/121/122/123/124;
116 only for genuinely unknown shapes — the 2026-09-24 corrective-lane
split, §7) and honest text "-- fail-closed deny (nothing is governed
while the host cannot be reached)" (exit
gate 5: nothing is reported as governed). For `session_start` and
`post_tool`, unreachable host → exit 0 with an honesty note (activation
is advisory, ADL §13; a completed action cannot be retroactively
denied — the outcome records as unobserved). A deny is ALWAYS
`[qiven] deny <code>: <detail>` on stderr, exit 2 — one line, no
decorations, machine-greppable.

`qiven-zcode-hook --event <kind> --root <governed checkout>` reads
stdin to EOF (cap 1 MiB; oversize → deny 118 for pre_tool, note for
the advisory events), extracts fields, transacts ONE request over the
pipe (hello is folded into the same connection: connect → hello →
hook_event → hook_ack → close), maps, exits. The `--root` flag comes
from the hook registration template; `QIVEN_CONTEXT_ROOT` env is the
fallback; neither → usage exit 2 (fail-closed for pre_tool callers).

### 3.4 Host-side hook handling (`runtime_host.cpp`)

**Session registry (ARCH §12.2).** `session_start` with an unseen
`session_handle`: journal `open_session` (host-minted SortableId128
session id, generation pinned); seen handle → idempotent re-ack of the
SAME session id (hooks are one-shot; ZCode may fire SessionStart
again). The manifest note + `mediated_tools` list is compared against
the profile's declared tool inventory; mismatch → session registered
with `scope_degraded` and every pre_tool for affected tools denies
113 (fail-closed for the affected class — the F-suite handshake law).

**Action IDs.** Per-session monotonic u64 counter (host memory,
journal-correlated through the transaction's harness reference);
uniqueness across boots is carried by the never-repeating session id
(minter law, DESIGN §6). Every pre_tool AND post_tool receives its own
action id; correlation pairs them by (session, pre-action id) — the
post must name the pre it observes via the digest of the payload it
reports (ZCode passes the tool response payload; the host matches it
to the outstanding allowed action by session + tool + FIFO order
within the redeliberate window, and a duplicate or unmatched post is a
typed 115/114 — no guessing).

**Pre-tool judgment.** The control path runs Observed → Classified →
JudgmentOpened (generation pinned) → RequirementsDerived → Decision:
(a) tool not in the inventory → deny 112 (unknown tools fail closed);
(b) Write/Edit with `file_path` normalizing inside a governed path
prefix under the governed checkout root → deny 110; (c) Bash whose
command text references a governed prefix or the checkout root → deny
111 (conservative); (d) everything else → not_governed (allow,
telemetry). Allow is bound + consumed at admit (single-use), the
transaction stays open awaiting the post observation; deny terminates
its transaction Closed. Path normalization: lexical normalize +
case-insensitive compare (Windows) + reparse-point rejection (ARCH
§12.3; junction/traversal → deny 110-class, never a bypass).

**Post-tool observation.** Exit facts (payload digest; ZCode
tool-response status string when extracted) close the paired
transaction: Closed (acknowledged success/failure) or Indeterminate
(no matching pre / ambiguous). One outstanding pre per (session,
tool) at a time: a second pre while one is unobserved denies 115
forces observation ordering (complete-correlation law, ARCH §11.3
shape).

**H-2 SessionStart refresh (ARCH §7.4).** On `session_start` (only):
bounded remote fetch via the process runner (`git fetch origin
<authorized-ref>` with a 5 s budget, env allowlist, allowlisted git
executable); success → resolve the remote-tracking ref, publish a new
bundle from that commit, `advance_generation`, pin the session;
fetch failure → local-fallback IF the active bundle's age is within
`freshness_window_ms` (reply `refresh: local_fallback`, audit event);
expired → `refresh: expired` AND every governed-scope pre_tool denies
117 until a SessionStart succeeds (typed IPC 66 at the mutation
boundary; status/doctor still serve). The dirty checkout is never
read (bundle-from-Git law, DESIGN §8). Refresh work runs on the
control thread inside the request deadline; overrun degrades to the
fetch-failure path.

**H-4 shutdown.** `shutdown {grace_ms}` from an authenticated
client → reply `shutdown_ack` FIRST, journal audit event, then drain:
stop accepting, drain ingress bounded by grace, checkpoint WAL,
release, exit 0 (MVP-3 law: never exit with an unresolved dispatch —
none exist; open pre-transactions are marked Indeterminate at exit).

### 3.5 Profile revision 2 (`zcode-jason-context-record-mvp.yaml`)

`revision: 2`. Changes: capabilities 4-6 mediation kind
`ActionInterception` (from `Unmediated`); new `tool_inventory` block
declaring the write-capable entry points the hook mediates —

```yaml
tool_inventory:
  - tool: Bash
    capability: 4
    extraction: command
    detector: conservative_text_reference
  - tool: Write
    capability: 5
    extraction: file_path
    detector: exact_path
  - tool: Edit
    capability: 6
    extraction: file_path
    detector: exact_path
```

— plus the mediation-claim honesty note (boundary complete, Bash
detector scope explicit). The loader validates: every inventory row
references a declared, ActionInterception-mediated FileSystemWrite
capability, and unknown detector/extraction values reject.
(Batch-time correction of this section's original wording, found by
profile_accept: a DOWNWARD completeness rule — "every FileSystemWrite
capability has an inventory row" — is not well typed, because
capabilities 1-3 are the mediated qiven-record path, not harness tool
surfaces; harness-surface completeness is the inventory enumeration
itself plus the H1 real-tool proof.) The
`qiven-record` grammar pointer (ARCH §6.3): `record_launcher:
qiven-record --request-file <path> | --stdin` recorded as accepted
grammar surface (the executable itself is MVP-5).

## 4. Concurrency and lifecycle

Unchanged from MVP-3: one control thread; the hook client is a
one-shot process with NO threads (single synchronous pipe round-trip;
the 250 ms margin guarantees the answer maps before the ZCode hook
timeout). The refresh fetch reuses ProcessRunner (joined readers). No
new threads, queues, timers, or background work; freshness is
evaluated at request time (MVP-3 law).

## 5. Failure modes (fail-closed table)

| Failure class | Behavior |
| --- | --- |
| stdin oversize / unextractable required fields | pre_tool: deny 118; advisory events: exit 0 + note |
| host unreachable / timeout / HMAC / protocol / version | pre_tool: deny 116 (honest text); advisory: exit 0 + note |
| unknown tool | deny 112 |
| Write/Edit target inside governed scope | deny 110 |
| Bash text references governed scope | deny 111 (conservative) |
| pre_tool before session_start (unknown handle) | deny 114 |
| duplicate/unmatched post | typed 115/114; transaction Indeterminate |
| manifest/tool-inventory mismatch | session degraded; affected tools deny 113 |
| fetch fails, bundle fresh | local_fallback; session proceeds |
| fetch fails, bundle expired | session degraded; governed pre_tool deny 117 |
| shutdown during in-flight hook request | request completes or denies 119 before drain starts |
| journal write fails mid-judgment | deny (61-class) — no decision without durable record |

## 6. Test spine (each row names its exit-gate proof)

| Test | Proves (ARCH §15 MVP-4) |
| --- | --- |
| `hook_conformance` — extraction table | payload digests bind verbatim bytes; signed/float values in unneeded fields do not break extraction (delta 1.1); missing required fields deny |
| `hook_conformance` — verdict mapping | gate 3: unknown tool → 112; unknown reply shape → deny; version mismatch → deny |
| `hook_conformance` — mediation matrix | gate 2: Write/Edit governed+ungoverned paths; Bash reference matrix (governed token, checkout root, clean command); junction/traversal path rejects |
| `hook_conformance` — identity/correlation | gate 4: unique action ids across repeated identical actions; duplicate post → 115; missing post → Indeterminate; session id host-assigned, idempotent re-start |
| `hook_conformance` — disconnect honesty | gate 5: unreachable host → pre_tool deny 116 whose text claims NO governed status; session_start/post exit 0 with note |
| `hook_conformance` — H-2 refresh | fetch success → new generation + session pinned; fetch fail + fresh → local_fallback; fetch fail + expired → deny 117/typed 66 |
| `hook_conformance` — H-4 shutdown | shutdown_ack precedes drain; WAL checkpoint verified by journal reopen; in-flight request completes |
| `hook_conformance` — end-to-end | real `qiven-zcode-hook.exe` child processes against a real host on the real pipe (fixture stdin, exit-code + stderr assertions) |
| real-ZCode H1 kit (`docs/engineering/mvp4-h1-kit.md`) | gate 1: deny has no fallthrough in the LIVE harness — owner hands, paste-ready kit, sealed evidence relayed back |

## 7. Dependencies and deployment

No new third-party slots (jsonx/processx reused). Deploy: the bundle
gains `qiven-zcode-hook.exe`; smoke adds `qiven-zcode-hook --help`
(exit 0). Profile revision 2 ships in-repo (config/profiles). The
qiven-context side is untouched this batch (policy digest unchanged);
the workspace `.zcode/config.json` registration is the owner-H1 kit's
step, not a repository change.

## 8. Deferrals (falsifiable, constitution §15)

| # | Deferral | Failure signal / revisit trigger |
| --- | --- | --- |
| J-1 | lazy host start from SessionStart | measured SessionStart latency pain or dogfood friction starting the host manually (MVP-7); revisit with a bounded spawn-from-hook design |
| J-2 | Bash write-intent analysis beyond conservative text reference | an MVP-7 dogfood bypass attempt or a false-deny storm; revisit with a structured command grammar (shellcheck-class) behind the same boundary |
| J-3 | post-tool pairing beyond FIFO+tool within the window | observed mis-pairing under parallel tool calls in real sessions; revisit with payload-shape correlation keys |
| J-4 | hook-side stdout telemetry for not_governed | owner asks for visible allow telemetry in the transcript; none today (silence = allow, per ZCode contract) |

## 9. Review record

Self-review 2026-09-23 (v21 session, designation
`jason-extended-cognition`), performed against the design ALONE,
before any implementation code exists in the branch:

1. Checked every exit-gate row 1–5 against a named proof (§6) — gate 1
   split into its locally-provable parts and the recorded owner-H1
   live remainder; no row silently claimed.
2. Delta 1.1 (locate-grade extraction) checked against the bridge
   precedent and the jsonx u64 law — the alternative (extending jsonx
   to signed/float) would weaken the machine-file law for a harness
   input that only needs digesting; rejected.
3. Delta 1.2 honesty: the complete-mediation claim is scoped to the
   BOUNDARY plus exact detectors (Write/Edit) and a conservative
   detector (Bash) with the residual recorded in the profile itself —
   consistent with ADL §11 (a dishonest manifest is undetectable; ours
   declares its scope).
4. Verified the deny-always consequence of delta 1.3 against the
   mediated-path requirement (ARCH §3.3): MVP-4 denies governed raw
   writes; it does not pretend the typed path exists. MVP-5 turns the
   same deny into re-deliberate routing without hook changes.
5. Checked H-2 against ARCH §7.4 word by word: bounded fetch ✓,
   authorized exact ref ✓, validate+publish+generation+pin ✓,
   local-fallback within window ✓, expired → governed mutations
   denied ✓, never the dirty checkout ✓.
6. Checked H-4 drain order against MVP-3 shutdown law (WAL checkpoint,
   bounded drain, no unresolved dispatch — none exist) and the
   reply-before-drain ordering (the client must observe the ack even
   if drain outlives it).
7. Correlation design (FIFO + tool + one-outstanding-pre) checked
   against ZCode's serial tool-call model within one turn; J-3
   records the parallel-call revisit trigger rather than assuming
   serialty forever.
8. Confirmed the thin-client law (ARCH §6.2 MUST-NOTs) line by line
   against §3.3: no classification, no counters, no decisions, no
   mutations, no failure-to-allow — all hold; the client's only
   judgment is the mechanical verdict→exit mapping.
9. Confirmed no new truth: sessions/actions/decisions live in the
   journal; the hook retains nothing; the profile remains the only
   policy instance (revision bump explicit).

## 10. Corrective lane (2026-09-24; deny-116 incident amendments)

Incident record: qiven-context `evidence/audits/2026-09-24-mvp4-h1-deny116-incident.md`
(the third MVP-4 H1 trial). Four amendments to this design, all landed in
the same corrective batch:

1. **Connection model (decision D1).** The wire contract stands as
   declared (`framing.hpp`: `connection_seq` "strictly increasing per
   connection") and §3.3's client shape (hello + hook_event on ONE
   connection) is CORRECT. The production serve loop now lives as a
   testable library unit — `ipc/pipe_service.hpp/.cpp`
   (`serve_connection`) — and serves a bounded frame SEQUENCE per
   connection: until the client disconnects, a frame does not arrive
   within the idle bound (default 30 s), a typed protocol/security class
   fails (error frame, then close), or the per-connection frame budget
   (64) is exhausted. The exe loop delegates to it. Single-frame clients
   (runtimectl) are unchanged.
2. **Admission has a reply surface (decision D2).** A rejected client
   image receives a typed 62 error frame ("admission rejected: ...")
   before the connection ends; the hook client maps it to deny 121.
   Silence is no longer an admission verdict the client must guess.
3. **Denial taxonomy split (decision D3).** Client-side transport
   failures classify disjointly (`classify_transport_failure`): 120
   no-listener (connect refused / no install), 121 admission-rejected,
   122 version-skew (typed 64 at decode), 123 secret-skew (HMAC 62 /
   unreadable DPAPI secret), 124 timeout (deadline-bounded reads via
   `read_frame(timeout_ms)` on both client and server). 116 remains ONLY
   for genuinely unknown shapes (post-connect silent death, unexpected
   reply shapes) — never silently absorbing a classable cause.
4. **Replay honesty note.** `ReplayGuard`'s nonce/timestamp window is
   NOT wired into the wire format: request bodies carry no nonce or
   timestamp (the removed main() comment claimed otherwise — a dead
   claim, deleted). Enforced per connection today: HMAC, frame caps,
   strictly-increasing connection_seq (typed 63), frame budget, idle
   bound. Wiring the nonce/timestamp window into the request envelope is
   a recorded PRE-MVP-5 hardening obligation (owner-governed; requires
   a wire-format decision across all three clients).

Kit law addition: the H1 kit carries `preflight.cmd` (enable-gated
functional self-check — host boot, real-pipe verdict round trip,
authenticated-shutdown EXIT, typed 120 no-listener honesty after stop)
which the owner runs BEFORE approving the workspace config in the ZCode
UI. Regression proof: `tests/ipc_multiframe_contract.cpp` (each test
fails under the actual prior implementations — one-frame loop, silent
admission, unchecked seq, unbounded idle, undifferentiated 116; the
drip-stall and budget tests carry a ctest TIMEOUT so a regression FAILS
instead of hanging).

### 7.1 Adversarial-review amendments (same batch, fresh-context review)

An independent fresh-context review (R2 class
`fresh-cognitive-same-family-isolated-context`) empirically falsified
four properties of the first cut; all four are fixed in this batch:

- **M1 — whole-frame deadline.** The idle bound originally covered only
  first-byte arrival; a client writing one byte of a header and stalling
  held the serve thread in a blocking `ReadFile` forever (reproduced:
  host wedged 40 s; runtimectl starved behind it). `read_frame(timeout)`
  now bounds the COMPLETE frame under one deadline (peek-poll before
  every chunk).
- **M2 — vanished-client accept retry.** A client that connected and
  died while the host was busy made `ConnectNamedPipe` fail
  NO_DATA/BROKEN_PIPE, which the accept path treated as fatal — killing
  the host. Vanish-class errors now replace the listen instance and keep
  accepting (bounded internal retry).
- **M3 — shutdown exits the exe.** `runtimectl host shutdown` acked
  draining while the exe served forever (g_stop was Ctrl+C-only). The
  serve handler now sets the stop flag after the ShutdownAck rides the
  connection; the preflight ASSERTS real process exit (a kill fallback
  is a FAIL, not a success).
- **M4 — preflight custody.** Probe timeouts raise `SubprocessError`
  (not OSError): uncaught, they escaped as a traceback with a leaked
  host and an "[ OK ]" wrapper line. The preflight now catches
  everything, kills the host on every exit path (finally), and the .cmd
  echoes FAIL on nonzero exit.
- Minor in the same pass: taxonomy producers pinned by a REAL wrong-key
  decode in the test (not only synthetic strings); host-answered
  handshake rejections no longer claim "host cannot be reached";
  CryptProtectData failures classify 123; residual documented — the
  500 ms admission linger can still lose an error frame to a client
  whose read is delayed beyond the bound (the frame loss window is
  bounded and the class then degrades to 116-with-honest-text, never a
  false allow).
