# MVP-4 Redesign — The Long-Lived Host Server

Status: **batch design (R3)**. This design OVERTURNS the MVP-4
connection/lifecycle model per owner direction 2026-09-27 (v37 session,
recorded verbatim intent): the host is redefined as a **long-lived
available server**; it **must not depend on any existing or potential
future client call**; the previous "server waits for client" model —
with waits backed by protocol timeouts — is the named defect class
("目前是server wait for client（qiven-zcode-hook等)，这个是极度愚蠢的
设计，居然为了wait还加了timeout这种莫名其妙的垃圾").

Authority frame: ADR-0055 (ACCEPTED) remains the MVP-4 acceptance
instrument — the simulated ZCode hook lifecycle gate — and is itself
refactored to this contract in the companion batch. Nothing here
re-opens real-harness execution on the owner machine (that ban stands);
the H1 kit delivered under this design serves the owner's MANUAL
installed-desktop work only.

## 1. Why the previous model is overturned (evidence, not taste)

Five owner-live/owner-run trials failed on five distinct mechanisms —
and all five share ONE architectural property:

| Trial | Mechanism | Shared property |
| --- | --- | --- |
| 1 (2026-09-23, deny-118) | an assumed payload field the harness never sends | governance availability coupled to a just-in-time protocol assumption |
| 2 (2026-09-23, deny-116 storm) | hand-configured hooks + host config unreachable | availability coupled to per-use host startup |
| 3 (2026-09-24) | install record lacked hook image; admission silently dropped | host state coupled to which clients had appeared |
| 4 (2026-09-26) | hello carried session_start's 9750 ms deadline; host ceiling for hello is 5000 ms → handshake rejected → session never registered → every event denied 114 | a TIMING precondition in the protocol; one advisory failure invisibly poisoned all later governance |
| 5 (2026-09-27, kit preflight `0.1.0-gffb31c22`) | `session_start` runs `git fetch` (5 s budget) + bundle publish + possible generation activation INSIDE the request path; the hook client's 9750 ms budget expires; "no reply from host within 9750 ms (timeout); session NOT registered" | heavyweight, network-dependent, environment-sensitive work inside a timing-bounded client call — the server doing client-triggered work against the client's clock |

Every patch fixed one mechanism and left the class armed. The class is:
**the host's governance availability was a function of client arrival,
client ordering, and timing dances between two clocks.** This design
removes the class, not mechanism number six.

## 2. Laws (the new contract)

### LL-1 — Long-lived server

The RuntimeHost is a long-lived singleton server for a governed root.
It is started once and runs indefinitely; its availability is a
property of the machine session, never of a trial, a hook firing, or
any client. Concretely:

- the kit installs a user-scope autostart (Startup folder) that starts
  the server at logon; `start-host.cmd` starts it idempotently on
  demand (already-running is a typed OK, never a second instance —
  the singleton mutex stays);
- trials and preflights VERIFY the server; they do not own its
  lifecycle (the preflight may START it via the normal start path —
  that is installing availability, not a trial boot — and LEAVES IT
  RUNNING);
- the operator keeps explicit control: `runtimectl host shutdown`,
  Ctrl+C when interactive, `remove-autostart.cmd`.

### LL-2 — Client independence (the owner's precise directive)

No host state transition, availability property, or correctness
property depends on ANY client call — present, ordered, or future.
Three corollaries, each killing one historical failure mechanism:

- **LL-2a First contact is sufficient.** A session is minted
  idempotently at the FIRST event of ANY kind that names an unseen
  harness session handle (`session_start`, `pre_tool`, or `post_tool`).
  The deny-114 "unknown session — register first" class is RETIRED
  (taxonomy slot reserved, documented dead). A lost, late, or failed
  `session_start` has NO governance consequence: the very next
  `pre_tool` mints the session and is judged on its merits. The
  mediated-tools manifest remains an ADVISORY carried by
  `session_start`: when a manifest IS declared and disagrees with the
  profile inventory, ONLY the affected tools are marked degraded for
  that session (deny 113 per affected tool — a semantic refinement of
  the F-suite honesty law: today's implementation degrades the whole
  session; this design scopes degradation to the mismatched tools,
  and the change is listed in the module map); when no manifest ever
  arrives, governance runs at the profile-declared scope and the
  boundary honesty note in the profile covers the difference.
  Manifest absence never reduces availability.
- **LL-2b Heavy work never runs inside a request.** Cognition refresh
  (`git fetch` + bundle publish + generation activation) is
  HOST-AUTONOMOUS: it runs at boot (local publish), on a host-internal
  cadence (`refresh_interval_ms`), and on the operator verb
  `runtimectl host refresh`. No client event triggers it, and no
  network, git, or bundle-write operation is reachable from the
  request path. Requests are answered from already-published state in
  bounded, trivial time. (Recorded ARCH delta: ARCH §7.4's "refresh at
  SessionStart" trigger is superseded by this design — the freshness
  LAW of §7.4 stands unchanged; see §6.)
- **LL-2c Host state never encodes client arrival.** Admission,
  freshness, generation, and scope state derive from host-owned inputs
  (install record merged at boot, profile, journal, clock) — never
  from which clients have connected or in what order.

### LL-3 — No correctness-bearing waits or timeouts

- **The wire carries no deadline.** `deadline_ms` is removed from the
  request envelope (protocol shape revision; unknown-field rejection
  keeps old shapes failing closed with a typed code the new client
  maps to version skew). The hello-deadline ceiling — the trial-4
  mechanism — is dead by construction: there is no deadline to
  validate, reject, or split.
- **The host never waits on a client as a correctness step.** The
  accept loop blocks indefinitely and serves every connection on its
  OWN thread (thread-per-connection, bounded by a connection cap), so
  one slow, stalled, or hostile connection can never wedge another —
  the serial-serve/whole-frame-deadline machinery (M1) protected a
  shared loop that no longer exists.
- **Abuse bounds are hygiene, never correctness.** Per-connection
  frame byte cap, frame budget, idle close, and the connection cap
  protect shared resources; they are never crossed by a correct
  client, and closing on them cannot affect any OTHER connection's
  service. The one client-visible bound is the connection cap: an
  over-cap connection receives a typed error frame carrying NEW
  reason code **125 (server-busy)** and closes. The hook client is
  one-shot and does NOT retry: `pre_tool` maps 125 to a fail-closed
  deny with honest text; advisory events exit 0 with a note. The cap
  (default 8) is sized far above real hook concurrency so a correct
  deployment never sees 125.
- **Caller-side budgets remain the caller's right.** The hook keeps
  client-side read bounds (session_start 9750 ms, pre/post 4750 ms —
  the harness's own budget minus margin) and reports a typed 124
  timeout honestly. Under this design a healthy server answers in
  single-digit milliseconds, so a caller-side timeout now signals
  GENUINE unavailability, not a designed race between two clocks.
- **Mixed-fleet honesty.** An OLD client (still sending
  `deadline_ms`) against the NEW host fails closed at decode (typed
  unknown-field error) and its existing classifier reports the
  116-class fail-closed deny with honest text — an acceptable
  transition state; host and clients ship in one installation unit.

### LL-4 — Failure honesty without state poisoning

Every client-visible failure is a pure function of THAT call: typed,
source-tagged (hook-client-side vs host verdict), leaving no protocol
residue anywhere. When the server becomes reachable again, the very
next call works — there is no re-registration ritual, no wedged
handshake state, no session to repair. The journal records what was
observed; nothing is inferred from what was not.

## 3. What is overturned and what is kept

**Kept (proven machinery, untouched semantics):** HMAC + DPAPI
installation-secret framing (`framing`), per-connection strictly-
increcreasing `connection_seq`, client-image admission with the typed
reply surface, owner-only DACL, the journal (MVP-1), the cognition
bundle + generation machinery (MVP-2), profile/deployment validation,
SortableId128 minting, pre/post correlation law (one outstanding pre
per session+tool; duplicate/unmatched posts typed 115-class), the
mediation detectors (exact Write/Edit, conservative Bash reference),
the hook's exit contract (deny = exit 2, one machine-greppable line),
and the fail-closed deny law for unreachable hosts (120/124 with
honest "nothing is governed" text).

**Overturned:** per-use host boot as a trial step (LL-1); session
registration as a precondition + deny 114 (LL-2a); refresh inside the
`session_start` request (LL-2b); `deadline_ms` on the wire + the hello
ceiling (LL-3); the single-threaded serve loop where one connection
can delay every other (LL-3); the kit preflight's boot-probe-kill
shape (§8).

## 4. Module map (batch b)

```text
include/qiven/runtime/ipc/protocol.hpp/.cpp   envelope: deadline_ms removed;
                                              NEW request kind `refresh`
                                              (operator class, no body);
                                              status reply GAINS fields
                                              refresh_state /
                                              last_refresh_ok_ms /
                                              next_refresh_due_ms
                                              (closed vocabularies updated
                                              both directions);
                                              hook_ack.refresh = host state
include/qiven/runtime/ipc/pipe_service.hpp    serve_connection unchanged in
                                              shape (idle close = hygiene);
                                              documented per-thread use
src/ipc/named_pipe_server.cpp                  multi-instance accept (one
                                              listen instance per in-flight
                                              connection); accept errors
                                              NEVER fatal (LL never-exit
                                              policy); stop-wake via
                                              CancelSynchronousIo on the
                                              accept thread
src/host/runtime_host.cpp/.hpp                 ensure_session() first-contact
                                              minting for ALL events; PER-TOOL
                                              manifest degradation (113 scoped
                                              to mismatched tools); session
                                              registry lock; refresh WORKER
                                              (boot + cadence + operator
                                              refresh kind) off the request
                                              path; state mutex; status
                                              refresh fields; drain marks
                                              outstanding pre transactions
                                              Indeterminate
src/host/deployment_profile.cpp                revision 3: cognition.
                                              refresh_interval_ms (default
                                              900000), validated > 0
apps/runtime_host_main.cpp                     thread-per-connection serve
                                              loop with connection cap
                                              (typed 125 busy frame);
                                              per-connection catch-all fault
                                              containment (audit row, close,
                                              host survives); --log <file>
                                              background mode (append; staged
                                              markers + heartbeat to the
                                              log); stop: CancelSynchronousIo
                                              wake, close connections, bounded
                                              join, checkpoint
apps/runtimectl_main.cpp                       + `host refresh [--wait-ms]`;
                                              status prints refresh state /
                                              last-ok / next-due
src/adapter/zcode_hook.cpp                     no deadline on the wire;
                                              client-side read bounds stay;
                                              125 busy mapping (pre_tool ->
                                              fail-closed deny; advisory ->
                                              note)
apps/zcode_hook_main.cpp                       budgets unchanged (client-
                                              side only; comment law)
config/profiles/zcode-jason-context-record-
  mvp.yaml                                     revision 3 (refresh cadence)
tests/hook_conformance.cpp                     updated + new rows (§7)
tests/ipc_multiframe_contract.cpp              updated rows (§7)
docs/architecture/*.md, docs/design/*.md        dated amendment notes at
                                              every superseded clause
                                              (§10 supersession map)
```

Out of scope: `qiven-record` (MVP-5), dispatch/lease/fencing (MVP-6),
CBOR framing, nonce/timestamp replay window (pre-MVP-5 hardening
obligation unchanged), Windows service host (Startup-folder autostart
is the user-scope mechanism; a service is a revisit trigger, §9).

## 5. Concurrency and lifecycle contract (stated before synchronization)

- One accept thread. Each accepted connection is served by one
  dedicated worker thread; at most `max_connections` (default 8)
  concurrent serve threads; a further connection receives the typed
  125 busy frame (LL-3) and closes.
- **The accept loop never ends because of a client.** Accept errors
  are NEVER fatal to a long-lived server: vanish-class errors keep
  the existing bounded replace-and-retry; any other accept failure
  recreates the listen instance and retries with internal backoff;
  a persistently failing listener enters a loud DEGRADED state
  (heartbeat lines + `status.state` report it; journal audit row) and
  keeps retrying — the process exits only on operator stop (Ctrl+C,
  console close, authenticated Shutdown) or boot-class failure before
  serving begins.
- **Per-connection fault containment.** Every serve thread wraps its
  whole connection body in a catch-all: an escaping exception or
  unclassifiable fault closes THAT connection and journals an audit
  row (`serve_thread_fault`); the host and every other connection
  continue. No client input can terminate the server process.
- One host state mutex serializes ALL journal and host-state mutations
  (request handling, refresh publish/activation, status). Connection
  threads never hold the mutex across pipe I/O.
- The refresh worker: `git fetch` runs OUTSIDE the mutex (it touches
  no host state and may take seconds); bundle publish + generation
  activation + journal meta run under the mutex (bounded local file
  work). Worst-case request-visible effect of a concurrent publish is
  a short mutex wait (measured bound recorded in the gate; revisit
  trigger if it ever matters — §9).
- **Generation law.** Minting a session pins its journal identity
  (the `open_session` row carries the then-active generation); every
  verdict runs at, and reports, the CURRENT active generation — a
  worker activation mid-session applies to subsequent verdicts, and
  the single-use decision law binds at admit time. This supersedes
  ARCH §7.4's "pin the session to that generation" wording (the
  pinning that remains is the audit-row identity; §10).
- Stop semantics: `g_stop` set by Ctrl+C handler, console close, or an
  authenticated Shutdown request observed on any serve thread; the
  accept thread's pending synchronous `ConnectNamedPipe` is cancelled
  by `CancelSynchronousIo` targeting the accept thread (the listen
  handle stays synchronous — `CancelIoEx` applies to overlapped I/O
  and would not wake this call); the stop path then CLOSES open
  connection handles so blocked reads abort promptly, serve threads
  join within a bounded grace (worst case: the per-connection idle
  bound), open pre-transactions are marked **Indeterminate** in the
  journal at drain (closing the H-4 claim honestly — see restart
  semantics below), WAL checkpoint, exit 0. In-flight requests
  complete or deny 119 before drain starts (unchanged).
- **Restart semantics (long-lived makes this routine, so it is
  designed, not inherited).** A host restart between a `pre_tool`
  allow and its `post_tool` leaves that correlation honestly
  Indeterminate: the drain marks outstanding pre transactions
  Indeterminate; a harness handle re-contacting after restart mints a
  FRESH runtime session (a second `open_session` row — the journal's
  boot-epoch column separates the lifetimes; this is the honest
  boundary, not an idempotence failure within one lifetime — LL-2a's
  "idempotent re-contact keeps one session id" holds within one host
  lifetime). LL-4's "no session to repair" is scoped exactly:
  AVAILABILITY resumes immediately after restart (the next call is
  served with no ritual); cross-restart correlation is never guessed.
- Freshness evaluation is at request time from the durable
  `last_refresh_ok_ms` journal meta + wall clock (unchanged LAW);
  the refresh cadence is a worker sleep between ATTEMPTS, not a
  deadline any request waits on.

## 6. Cognition refresh under LL-2b

- Boot: publish from the authorized LOCAL ref (offline-safe), reuse or
  seed `last_refresh_ok_ms` (unchanged).
- Worker: one attempt shortly after boot, then every
  `refresh_interval_ms` (profile revision 3, default 15 min). Each
  attempt = fetch (bounded 5 s, outside the mutex) → publish →
  digest-compare → maybe activate generation (under the mutex) →
  journal `cognition_refreshed` / `cognition_local_fallback` /
  `cognition_expired` (unchanged audit kinds; the worker is the
  producer now).
- Freshness LAW unchanged (ARCH §7.4): past the window without a
  successful refresh, governed-scope `pre_tool` denies 117 and
  status/doctor keep serving. Recovery is AUTOMATIC at the next
  worker success — no client action can or must contribute.
- `session_start` performs NO refresh work; its reply reports the
  host's current refresh state (`refresh:` field, informational on
  every hook event).
- `runtimectl host refresh` triggers one worker attempt immediately;
  with `--wait-ms N` it waits bounded for that attempt's outcome and
  prints it (operator convenience; the verb never blocks the server).

## 7. Test spine (batch b; each row names its law)

| Test | Proves |
| --- | --- |
| `hook_conformance` — first contact | LL-2a: `pre_tool` on a never-registered handle mints the session and returns a real verdict (never a 114-class deny); `post_tool` first contact degrades honestly (unmatched) without poisoning; idempotent re-contact keeps one session id within a host lifetime |
| `hook_conformance` — registration independence | LL-2a: full pre/post flow with NO `session_start` at all behaves identically to the registered flow |
| `hook_conformance` — no-deadline wire | LL-3: requests carry no `deadline_ms`; a request containing it fails closed typed (unknown field); the transition behavior of an old client is the honest 116-class fail-closed deny |
| `hook_conformance` — request-path cost bound | LL-2b: verdicts return within a small bound while git is a nonexistent executable / the remote is unreachable (no network in the path; measured assertion) |
| `hook_conformance` — manifest advisory | 113 ONLY for the mismatched tools (per-tool degradation); a matching manifest never degrades; manifest absence never degrades availability |
| `hook_conformance` — refresh worker | boot publish; cadence attempt observable via journal `cognition_*` rows and status; expiry → 117 with automatic recovery after a successful attempt; operator `refresh` kind triggers an attempt (authenticated, journaled) |
| `hook_conformance` — concurrency | a connection that stalls mid-frame does NOT delay another client's verdict (thread isolation); the connection cap replies typed 125; a fault injected on one connection (malformed frame class) never ends the host; accept-vanish storms never end the host |
| `hook_conformance` — shutdown/drain/restart | ack-before-drain; CancelSynchronousIo stop-wake; process exits within the bounded grace; outstanding pre transactions marked Indeterminate at drain; after restart, first contact mints fresh and verdicts resume with no ritual (LL-4 scoped) |
| `hook_conformance` — mediation/correlation matrixes | unchanged rows re-pinned (110/111/112/115/117/118 classes) |
| `ipc_multiframe_contract` | updated: idle close/frame budget as hygiene on a per-thread connection; seq law; error-frame linger; stalled-peer isolation regression (old-fail: the serial loop); 125 busy frame shape |

## 8. H1 kit (batch d) — server-shaped

The kit ships: `bin/`, the kit-internal profile (ADR-0049), the
workspace `config.json` template (unchanged registration shape), and:

- `install-autostart.cmd` — creates the user-scope Startup shortcut
  (starts the server minimized with `--log`), starts the server now,
  verifies a verdict round trip;
- `start-host.cmd` — idempotent start; already-running is detected by
  a `runtimectl host status` round trip BEFORE any launch attempt
  (typed "already running" line; no second instance — the singleton
  mutex stays as the mechanical backstop);
- `stop-host.cmd` — authenticated shutdown (unchanged);
- `remove-autostart.cmd` — removes the Startup shortcut, shuts the
  server down;
- `preflight.cmd` — VERIFIES the server model, run before approving
  the workspace config: (1) ensure the server is running (start via
  the normal path if absent, using the same already-running
  detection); (2) verdict round trip; (3) a FIRST-CONTACT `pre_tool`
  with no prior `session_start` — the LL-2a live proof; (4) a
  `session_start` round trip asserted FAST — wall-clock bound
  **2000 ms** for the full round trip (no refresh in the path; the
  bound is chosen far above the expected single-digit-ms answer and
  far below the old 9750 ms class); (5) status reports refresh state;
  (6) authenticated shutdown exits the process; (7) `start-host.cmd`
  brings it back and the next verdict succeeds — the no-residue proof
  (LL-4). The preflight LEAVES THE SERVER RUNNING (the long-lived
  model) and says so;
- `collect-evidence.cmd`, `rollback.cmd` (now also removes autostart),
  `GUIDE.md`, `manifest.json` — unchanged laws.

## 9. Deferrals (falsifiable, constitution §15)

| # | Deferral | Failure signal / revisit trigger |
| --- | --- | --- |
| S-1 | Windows service host instead of Startup autostart | logon-scope availability proves insufficient (machine-purpose serving), or the owner asks for pre-logon availability |
| S-2 | refresh retry/backoff policy beyond fixed cadence | measured flapping (repeated local_fallback/expired oscillation) in dogfood |
| S-3 | publish-under-mutex latency optimization (delta-publish) | a measured verdict latency spike overlapping a publish in dogfood |
| S-4 | log rotation for the background `--log` file | unbounded log growth observed in dogfood |
| S-5 | connection-cap raise / queueing | typed busy observed in real sessions (would indicate >8 concurrent hook clients) |

## 10. Supersession map (dated amendments land with batch b)

| Superseded clause | Carrier | By |
| --- | --- | --- |
| `mvp4-hook-adapter.md` §3.2 `deadline_ms` envelope + ceilings | this doc LL-3 | wire revision |
| `mvp4-hook-adapter.md` §3.4 session registry precondition + 114 | this doc LL-2a | first-contact minting |
| `mvp4-hook-adapter.md` §3.4 H-2 refresh inside `session_start` | this doc LL-2b/§6 | refresh worker |
| `mvp4-hook-adapter.md` §3.4 whole-session manifest degradation | this doc LL-2a | per-tool 113 |
| `mvp4-hook-adapter.md` §5 failure row "pre_tool before session_start → deny 114" | this doc LL-2a | row retires |
| `mvp3-host-ipc.md` §3.2 request deadline law (`deadline_ms` ≤ 5000, answer-or-65 within it) | this doc LL-3 | wire revision |
| `mvp3-host-ipc.md` single-threaded serve loop shape | this doc LL-3/§5 | thread-per-connection |
| ARCH §7.4 SessionStart refresh TRIGGER (the freshness LAW stands) | this doc §6 | host-autonomous trigger |
| ARCH §7.4 "pin the session to that generation" | this doc §5 generation law | audit-row pinning + current-generation verdicts |
| `mvp4-simulated-gate.md` scenario table | companion batch (c) | refactored gate |

## 11. Sequencing and publication law for the batches

- The design (this document) publishes ALONE first: it changes no
  executable behavior, so every standing gate stays green at its
  head.
- Implementation (b) and the sim-gate refactor (c) are ONE
  publication unit: (b)'s behavior changes make the shipped sim
  gate's retired-invariant cases (114-class registration
  precondition, deadline-envelope legs) fail by design, and the gate
  is never weakened to publish through that window — the branch runs
  its review loops ((b) before the sim refactor per owner direction,
  (c) after) and publishes once, with the FULL gate (including the
  refactored `h1-sim` + `h1-kit-test`) green at the exact head.
- The H1 kit (d) is BUILT at that published head (a tool run with its
  own regression, not a separate publication).

## 12. Review record

Design authored 2026-09-27 (v37 session, designation
jason-extended-cognition) BEFORE any implementation code of this
batch exists, per the design-first standard. Fresh-review loop
parameters (owner assignment this batch): K=3 consecutive clean
rounds approve; StandingLaw=5 — worst case after 5 consecutive
revision-warranting rounds the node is deemed owner-accepted after one
final fix, no interruption stop.
