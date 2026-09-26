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
  Ctrl+C when interactive, `remove-autostart.cmd`; the console
  handler also covers logoff and system shutdown (a Startup-launched
  server's DOMINANT termination source is logoff/reboot — the
  graceful drain must be the routine path, not the rare one; the
  unclean-kill backstop is the boot recovery walk, §5).

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
  `runtimectl host refresh` — a convenience TRIGGER only: an
  authenticated operator request that signals the worker; it never
  executes work on any request path and the host is complete without
  it. NO HOOK EVENT triggers refresh, no network, git, or bundle-write
  operation is reachable from the request path, and requests are
  answered from already-published state in bounded, trivial time.
  (Recorded ARCH delta: ARCH §7.4's "refresh at SessionStart" trigger
  is superseded by this design — the freshness LAW of §7.4 stands
  unchanged; see §6.)
- **LL-2c Host state never encodes client arrival.** Admission,
  freshness, generation, and scope state derive from host-owned inputs
  (install record merged at boot, profile, journal, clock) — never
  from which clients have connected or in what order.

### LL-3 — No correctness-bearing waits or timeouts

- **The wire carries no deadline.** `deadline_ms` is removed from the
  request envelope (protocol shape revision; unknown-field rejection
  keeps old shapes failing closed with a typed error — the old-client
  transition is the honest 116-class fail-closed deny per the
  mixed-fleet clause below; the NEW client never sends the field and
  needs no mapping). The hello-deadline ceiling — the trial-4
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
  reason code **125 (server-busy)** and closes — using the SAME
  bounded-linger mechanism every typed error close uses (the linger
  helper is shared from `pipe_service` to the listen pool's busy
  path), so the frame is never discarded by the close that follows
  it. The hook client is one-shot and does NOT retry: `pre_tool`
  maps 125 to a fail-closed deny with honest text; advisory events
  exit 0 with a note. The cap (default 8) is sized far above real
  hook concurrency so a correct deployment never sees 125; the 125
  audit row records the occupancy breakdown (active serve threads
  vs waiting arms) so the S-5 discriminator is observable.
- **Caller-side budgets remain the caller's right.** The hook keeps
  client-side read bounds — session_start 9750 ms, pre/post 4750 ms —
  chosen conservatively INSIDE the harness hook budgets (15 s / 10 s
  in the registration template) to also cover process spawn and
  verdict mapping; the numbers coincide with the retired protocol
  ceilings only because both derive from the same round-number
  budgets, and they are now purely caller-side choices. Every client
  read is bounded, runtimectl included (default 5000 ms, per-verb
  overridable) — no operator tool may hang against the server. A
  typed 124 timeout is reported honestly. Under this design a
  healthy server answers in single-digit milliseconds, so a
  caller-side timeout now signals GENUINE unavailability, not a
  designed race between two clocks.
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
increasing `connection_seq`, client-image admission with the typed
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
include/qiven/runtime/ipc/pipe_service.hpp    serve_connection loop body
                                              EXTENDED via ServeOptions
                                              (g_stop check between
                                              requests; bounded writes
                                              per the §5 I/O model);
                                              framing/seq/error surface
                                              unchanged; per-thread use
                                              documented
src/ipc/named_pipe_server.cpp                  LISTEN POOL: M concurrently-
                                              armed instances (default 4),
                                              replenished per accept; the
                                              CONNECTION CAP + typed 125
                                              busy emission live HERE
                                              (library layer, testable);
                                              accept errors NEVER fatal;
                                              stop-wake via g_stop recheck
                                              + CancelSynchronousIo per
                                              slot; PipeClient::connect
                                              gains WaitNamedPipe etiquette
                                              on ERROR_PIPE_BUSY; writes
                                              bounded by a deadline
src/host/runtime_host.cpp/.hpp                 ensure_session() first-contact
                                              minting for ALL events; PER-TOOL
                                              manifest degradation (113 scoped
                                              to mismatched tools); ONE state
                                              mutex covering the session
                                              registry + journal + host state
                                              (no second lock); refresh WORKER
                                              (boot + cadence + coalesced
                                              operator refresh kind) off the
                                              request path; status
                                              refresh fields; drain marks
                                              outstanding pre transactions
                                              Indeterminate
src/host/deployment_profile.cpp                revision 3: cognition.
                                              refresh_interval_ms (default
                                              900000), validated > 0 AND
                                              cross-validated
                                              refresh_interval_ms + fetch
                                              bound (5000) <
                                              freshness_window_ms (a
                                              healthy worker must never
                                              straddle a window edge into
                                              recurring 117)
apps/runtime_host_main.cpp                     serve loop WIRES the library
                                              listen pool (cap + 125 live
                                              in the library) with a
                                              live-connection registry
                                              (ownership transfer at stop);
                                              per-connection catch-all fault
                                              containment (audit row, close,
                                              host survives); console
                                              handler covers Ctrl+C/close/
                                              logoff/shutdown; --log <file>
                                              background mode (append;
                                              staged markers + heartbeat to
                                              the log); stop: bounded-grace
                                              join, checkpoint, teardown
apps/runtimectl_main.cpp                       + `host refresh [--wait-ms]`;
                                              status prints refresh state /
                                              last-ok / next-due; ALL client
                                              reads bounded (default 5000)
src/adapter/zcode_hook.cpp +
apps/zcode_hook_main.cpp                  no deadline on the wire;
                                              client-side read bounds stay
                                              (provenance per LL-3); 125
                                              busy mapping TAUGHT to the
                                              classifier (pre_tool ->
                                              fail-closed deny; advisory ->
                                              note); WaitNamedPipe connect
                                              etiquette consumed
config/profiles/zcode-jason-context-record-
  mvp.yaml                                     revision 3 (refresh cadence)
tests/hook_conformance.cpp                     handle()-level rows ONLY
                                              (§7) — no process-level
                                              assertions here
tests/host_server_lifecycle.cpp                NEW real-exe test: spawns
                                              qiven-runtime-host.exe as a
                                              child and asserts the
                                              PROCESS-level laws (§7) —
                                              stop/exit, vanish storms,
                                              stalled-peer isolation,
                                              concurrent connects, restart
tests/ipc_multiframe_contract.cpp              updated library-level rows
                                              (§7)
docs/architecture/*.md, docs/design/*.md        dated amendment notes at
                                              every superseded clause
                                              (§10 supersession map)
```

Out of scope: `qiven-record` (MVP-5), dispatch/lease/fencing (MVP-6),
CBOR framing, nonce/timestamp replay window (pre-MVP-5 hardening
obligation unchanged), Windows service host (Startup-folder autostart
is the user-scope mechanism; a service is a revisit trigger, §9).

## 5. Concurrency and lifecycle contract (stated before synchronization)

- **Accept topology — a listen pool, never a single listener.** The
  server keeps M (default 4) concurrently-ARMED pipe instances, each
  blocked in `ConnectNamedPipe` on its own accept slot; every accept
  immediately replenishes its slot. This is required for correctness,
  not scale: with one armed instance, a second simultaneous client's
  `CreateFileW` fails `ERROR_PIPE_BUSY` and the hook would deny 120
  "no listener" against a healthy server — availability coupled to
  client-arrival timing, the named defect class. Client-side connect
  etiquette (both hook and runtimectl, transport layer — NOT a
  verdict retry): on `ERROR_PIPE_BUSY`, `WaitNamedPipe` bounded by
  the caller's own budget, then reconnect; 120 is classified only
  when no instance arms within that budget.
- Each accepted connection is served by one dedicated worker thread;
  at most `max_connections` (default 8) concurrent serve threads; an
  over-cap connection receives the typed 125 busy frame (LL-3) and
  closes.
- **The accept loop never ends because of a client.** Accept errors
  are NEVER fatal to a long-lived server: vanish-class errors keep
  the existing bounded replace-and-retry; EXHAUSTION of that bound
  (a vanish storm) and any other accept failure both degrade to the
  never-fatal path — recreate the listen instance and retry with
  internal backoff; a persistently failing listener enters a loud
  DEGRADED state (heartbeat lines + `status.state` report it;
  journal audit row) and keeps retrying. No accept-path outcome
  reaches process exit; the process exits only on operator stop
  (Ctrl+C, console close, logoff/shutdown, authenticated Shutdown)
  or boot-class failure before serving begins.
- **Per-connection fault containment.** Every serve thread wraps its
  whole connection body in a catch-all: an escaping exception or
  unclassifiable fault closes THAT connection and journals an audit
  row (`serve_thread_fault`); the host and every other connection
  continue. No client input can terminate the server process.
- **Locking is one state mutex.** A SINGLE state mutex serializes ALL
  journal and host-state mutations: request handling (including the
  session registry — there is no separate registry lock), status,
  eviction sweeps, and the FINAL generation/bundle-id swap of a
  refresh. One lock means no lock-ordering contract to get wrong; the
  registry map, the outstanding-pre state, and the journal all live
  under it. Its hold times are millisecond-scale by construction
  (§ request-path cost bound).
- The refresh worker's LONG work (network fetch AND bundle publish
  file operations) runs OUTSIDE the state mutex under a refresh-path
  serialization that only refresh workers take — a verdict NEVER
  waits behind publish I/O; the refresh completes by taking the state
  mutex briefly to publish ids + journal rows. The gate MEASURES
  verdict latency during a concurrent publish and enforces a bound
  (§7) — the claim is tested, not assumed.
- **Connection I/O model.** Connection instances are created
  `FILE_FLAG_OVERLAPPED`; reads keep the existing peek-poll deadline
  loop (semantics unchanged); writes submit overlapped with a
  deadline wait, and `CancelIoEx` on that connection's handle cancels
  an expired write (the documented cancellation for overlapped I/O)
  — a write that cannot complete fails the reply, the connection
  closes (hygiene class), and the client's honest classification
  applies. No unbounded blocking write exists.
- **Refresh trigger coalescing.** Operator `refresh` triggers are
  coalesced with a minimum attempt interval (default 30 s): a
  trigger during cooldown sets a pending flag and the worker runs
  ONE attempt when the cooldown elapses — repeated triggers cannot
  drive back-to-back network fetches.
- **Generation law.** Minting a session pins its journal identity
  (the `open_session` row carries the then-active generation); every
  verdict runs at, and reports, the CURRENT active generation — a
  worker activation mid-session applies to subsequent verdicts, and
  the single-use decision law binds at admit time. This supersedes
  ARCH §7.4's "pin the session to that generation" wording (the
  pinning that remains is the audit-row identity; §10).
- **Stop semantics (complete contract, phased).** `g_stop` may be set
  by the Ctrl+C / console-close / logoff / shutdown handler or by an
  authenticated Shutdown request on any serve thread. Phase 1 — stop
  accepting: accept slots re-check `g_stop` AFTER creating an
  instance and BEFORE blocking in `ConnectNamedPipe` (closing the
  lost-wakeup window); the stop path issues `CancelSynchronousIo`
  against each slot and re-issues it on every grace tick until the
  slot joins. Phase 2 — bounded drain grace (default 5 s): live
  connections are NOT closed; serve threads check `g_stop` BETWEEN
  requests only, so a request already in flight completes (or denies
  119 at its own boundary) while no NEW request is read. Phase 3 —
  cancel and join: after the grace, the stop path aborts every
  remaining (idle, blocked, or hostile) serve thread with the
  DOCUMENTED mechanism — `CancelSynchronousIo` targeting that
  thread, re-issued on every grace tick until it joins. Closing a
  handle another thread is blocked on is NOT a contractual abort on
  Windows (handle-value reuse) and is never used as the abort
  mechanism: connection handles are closed by the stop path only
  AFTER the owning thread joins (ownership transfer happens at join,
  never mid-I/O). If a thread still lives after join-wait, the
  journal checkpoints and process teardown terminates the straggler
  (documented: grace plus teardown is the exit worst case).
  Outstanding pre transactions are marked Indeterminate, WAL
  checkpoint, exit 0.
- **Restart semantics (long-lived makes this routine, so it is
  designed, not inherited).** A host restart between a `pre_tool`
  allow and its `post_tool` leaves that correlation honestly
  Indeterminate: the drain marks outstanding pre transactions
  Indeterminate; if the process died uncleanly instead, boot
  recovery reconciles interrupted transactions per the MVP-1
  recovery walk (unconsumed decisions from older boot epochs are
  stale-marked at recovery — verified in the journal code). A
  harness handle re-contacting after restart mints a FRESH runtime
  session (a second `open_session` row — the minting host's boot
  epoch, recorded in the journal, distinguishes the lifetimes, and
  the restart test row asserts this; the honest boundary, not an
  idempotence failure within one lifetime — LL-2a's "idempotent
  re-contact keeps one session id" holds within one host lifetime).
  LL-4's "no session to repair" is scoped exactly: AVAILABILITY
  resumes immediately after restart (the next call is served with no
  ritual); cross-restart correlation is never guessed.
- **Registry eviction.** The in-memory session registry does not
  grow forever: a session with NO outstanding pre and idle beyond
  `session_idle_evict_ms` (default 24 h) is evicted (journal rows are
  permanent); a re-contact after eviction mints fresh — the same
  honest semantics as a restart (so LL-2a's idempotence holds within
  one host lifetime AND before eviction).
- **The refresh worker is fault-contained like every other unit.**
  The worker's whole attempt body runs under a catch-all: an
  escaping exception or unclassifiable fault journals an audit row
  (`refresh_fault`), marks refresh state degraded (visible in
  `status`), and the worker CONTINUES at its next cadence tick — a
  host-internal fault never terminates the long-lived process
  (LL-1 applies to every thread the host owns, not only serve
  threads).
- **Boot order: the singleton is acquired BEFORE any journal open or
  recovery.** The mutex key changes from install-id to a stable hash
  of the root's CANONICAL IDENTITY: a read-only directory handle on
  the resolved root resolved through `GetFinalPathFromHandle` (the
  OS-truth spelling — resolves case variance, 8.3 short names, subst
  drives, and symlinks), then case-folded and hashed. This closes the
  path-aliasing hole a verbatim `--root` hash would open (a second
  start through an alias spelling must NOT reach the journal), and
  makes the mutex-first order IMPLEMENTABLE for the first time:
  `mvp3-host-ipc.md` §3.4 specified mutex-before-journal but keyed
  the mutex by install-id — an id the journal itself mints, so the
  shipped code's journal-first order was forced by that keying, not
  an implementation slip (the §10 row records this precisely). With
  the root-identity key, an idempotent `start-host.cmd`
  racing a healthy server must fail fast at the mutex and exit
  WITHOUT opening the live journal or running recovery against it.
  The install record and pipe naming stay install-id-based.
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

| Test | Carrier | Proves |
| --- | --- | --- |
| first contact | `hook_conformance` | LL-2a: `pre_tool` on a never-registered handle mints the session and returns a real verdict (never a 114-class deny); `post_tool` first contact degrades honestly (unmatched) without poisoning; idempotent re-contact keeps one session id within a host lifetime |
| registration independence | `hook_conformance` | LL-2a: full pre/post flow with NO `session_start` at all behaves identically to the registered flow |
| no-deadline wire | `hook_conformance` | LL-3: requests carry no `deadline_ms`; a request containing it fails closed typed (unknown field); the transition behavior of an old client is the honest 116-class fail-closed deny |
| request-path cost bound | `hook_conformance` | LL-2b, structural + timed: during verdicts with git a marker-writing stub / nonexistent executable, NO `cognition_*` audit row appears and the marker file stays absent (no child spawned, no fetch attempted — the timing bound below cannot be fooled by a fast-failing environment), AND verdicts return within **250 ms** (expected single-digit ms) |
| degraded listener observability | `host_server_lifecycle` (real exe) | §5: with the pipe instance forced uncreatable, the host enters DEGRADED — heartbeat lines report it, `status.state` carries it, an audit row records it, and the process keeps retrying without exiting |
| stop lost-wakeup window | `host_server_lifecycle` (real exe) | §5: a stop issued exactly while an accept slot is between instance creation and `ConnectNamedPipe` still exits the process within the grace (the recheck law is exercised by a stop timed against a slot cycle) |
| hook client 125 mapping | `hook_conformance` | LL-3: a typed 125 busy error frame maps client-side to a fail-closed deny with honest text for `pre_tool` and an exit-0 note for advisory events (the classifier is taught the code) |
| refresh worker faults | `hook_conformance` | §5: an injected fault in the worker's attempt body journals `refresh_fault`, degrades refresh state, and the host KEEPS SERVING verdicts; the next cadence tick runs a normal attempt |
| registry eviction | `hook_conformance` | §5: with a test-injected small `session_idle_evict_ms`, an idle session evicts and a re-contact mints FRESH (same honest semantics as restart); the sim-gate oracle's one-`session_registered`-row-per-handle expectation is amended in batch (c) accordingly |
| verdict latency under concurrent publish | `hook_conformance` | §5 two-level locking: a verdict served WHILE a refresh publish runs returns within the bound (default assert < 1000 ms) — the claim is measured, not assumed |
| manifest advisory | `hook_conformance` | 113 ONLY for the mismatched tools (per-tool degradation); a matching manifest never degrades; manifest absence never degrades availability |
| refresh worker | `hook_conformance` | boot publish; cadence attempt observable via journal `cognition_*` rows and status; expiry → 117 with automatic recovery after a successful attempt; operator `refresh` kind triggers an attempt (authenticated, journaled) |
| mediation/correlation matrixes | `hook_conformance` | unchanged rows re-pinned (110/111/112/115/117/118 classes) |
| library serve semantics | `ipc_multiframe_contract` | idle close/frame budget as hygiene on a per-thread connection; seq law; error-frame linger; bounded writes; 125 busy frame shape |
| stop/exit + restart | `host_server_lifecycle` (real exe) | ack-before-drain; stop exits the real process within the bounded grace; outstanding pre transactions marked Indeterminate at drain; after restart, first contact mints fresh and verdicts resume with no ritual (LL-4 scoped) |
| client-independence of survival | `host_server_lifecycle` (real exe) | a vanished-client accept storm never ends the host; a stalled mid-frame connection never delays another client's verdict (thread isolation — the serial-loop old-fail); a malformed-frame fault on one connection never ends the host; the connection cap replies typed 125 |
| concurrent connects | `host_server_lifecycle` (real exe) | simultaneous clients all connect and receive verdicts (listen-pool admission; no ERROR_PIPE_BUSY → 120 against a healthy server); WaitNamedPipe etiquette verified from the client side under a brief artificial slot pressure |

## 8. H1 kit (batch d) — server-shaped

The kit ships: `bin/`, the kit-internal profile (ADR-0049), the
workspace `config.json` template (unchanged registration shape), and:

**Image-consistency invariant (trial-3 class, pinned):** the ONE
installation unit is the REPO BUILD DIRECTORY. The config template
registers the build-dir `qiven-zcode-hook.exe`; `start-host.cmd`,
`install-autostart.cmd`, and the autostart shortcut all launch the
build-dir `qiven-runtime-host.exe` (whose boot merges the sibling
client images into the install record); the kit's `bin/` copies are
pinned reference artifacts ONLY — never launched, never registered.
Every kit launcher states the exact image it runs.

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
  (LL-4). When the preflight did NOT start the server (it was
  already running — the normal case under LL-1), legs (6)-(7) are
  SKIPPED with an honest label: a verification tool does not bounce
  the autostart-owned server mid-flight (stop-host.cmd +
  start-host.cmd remain the owner's manual exercise of that path).
  The preflight LEAVES THE SERVER RUNNING (the long-lived
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
| S-5 | connection-cap raise / queueing | typed 125 observed in real sessions — DISCRIMINATED by the 125 audit row's occupancy breakdown: >8 genuinely concurrent hook clients → raise the cap; ≤8 stalled/hostile occupants → tighten idle/write bounds or linger policy (same observable, opposite fixes) |

## 10. Supersession map (dated amendments land with batch b)

| Superseded clause | Carrier | By |
| --- | --- | --- |
| `mvp4-hook-adapter.md` §3.2 `deadline_ms` envelope + ceilings | this doc LL-3 | wire revision |
| `mvp4-hook-adapter.md` §3.4 session registry precondition + 114 | this doc LL-2a | first-contact minting |
| `mvp4-hook-adapter.md` §3.4 H-2 refresh inside `session_start` | this doc LL-2b/§6 | refresh worker |
| `mvp4-hook-adapter.md` §3.4 whole-session manifest degradation | this doc LL-2a | per-tool 113 |
| `mvp4-hook-adapter.md` §5 failure row "pre_tool before session_start → deny 114" | this doc LL-2a | row retires |
| `mvp3-host-ipc.md` §3.2 request deadline law (`deadline_ms` ≤ 5000, answer-or-65 within it) | this doc LL-3 | wire revision |
| `mvp3-host-ipc.md` §3.4 singleton mutex keyed by install-id (mutex-first order unimplementable with a journal-minted key; shipped code opens the journal first as a forced consequence) | this doc §5 boot order | root-identity-hash mutex, acquired first |
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

## 12. Batch-b implementation notes (2026-09-27)

Deviations and refinements recorded at landing, none contradicting the
laws:

1. **Sliced overlapped waits instead of CancelSynchronousIo.** §5 names
   CancelSynchronousIo for the stop wake. The implementation creates every
   instance and client handle FILE_FLAG_OVERLAPPED and waits each pending
   operation in slices against the stop flag/deadline, cancelling via
   CancelIoEx on the ISSUING thread. This meets the same contract (no
   cross-thread handle close, prompt stop) with a strictly smaller
   surface; reads keep whole-frame deadline semantics (the overlapped
   read is issued once and waited — no partial-read loss).
2. **Drain marking is audit-level.** The journal has no public
   transaction terminal-transition command today (record_outcome is
   dispatch-based; dispatches are MVP-6). Drain appends
   `hook_outcome_indeterminate` audit rows for outstanding pres; the open
   rows reconcile at the next boot's recovery walk (MVP-1 law). A
   terminal-transition command is added to the pre-MVP-5 hardening list
   beside the replay-window wiring.
3. **Shutdown ack linger.** The serve thread that observes an
   authenticated Shutdown lingers (the shared bounded linger) before its
   connection closes: DisconnectNamedPipe discards unread bytes, and the
   loop tears down fast once stopped — without the linger the ctl
   delivered shutdown, the host logged the ack, and the client still
   observed "no reply" (found live by host_server_lifecycle).
4. **The exe's per-request `[conn]` logging and the heartbeat stay** (the
   human-facing output law); with `--log` they land in the log file.
5. **Test-carrier notes.** host_server_lifecycle drives the REAL sibling
   client executables (hook/runtimectl — the same installation unit the
   kit uses); ServeLoop-level laws (125, pool admission, phased stop,
   stalled-peer) run in ipc_multiframe_contract as library rows; the
   request-path cost row asserts the timed bound at handle level and the
   structural no-child/no-cognition-row property is covered by the
   lifecycle rig's session_start < 2000 ms leg (no git is reachable from
   the request path; the refresh worker observability leg proves the
   worker is the only cognition-touching path).

## 13. Review record

Design authored 2026-09-27 (v37 session, designation
jason-extended-cognition) BEFORE any implementation code of this
batch exists, per the design-first standard. Fresh-review loop
parameters (owner assignment this batch): K=3 consecutive clean
rounds approve; StandingLaw=5 — worst case after 5 consecutive
revision-warranting rounds the node is deemed owner-accepted after one
final fix, no interruption stop.
