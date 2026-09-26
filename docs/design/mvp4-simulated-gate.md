# MVP-4 Simulated ZCode Hook Lifecycle Gate — Design

> Batch: OBL-20260926T234500Z-B4C5D6 item 4 (qiven-context) under ADR-0055
> (accepted). Status: design (R3 anchor; precedes all semantics-changing
> commits of this batch per Devkit design-first workflow v2). This batch
> also folds the trial-4 one-line hello-deadline fix and the kit-preflight
> `session_start` coverage per ADR-0055 Consequences.

## 1. Problem and decision

Four owner-live MVP-4 H1 trials failed on four distinct infrastructure
defects, each invisible to the layer below it (evidence: the four incident
audits + the separate kit-preflight incident). ADR-0055 retired the
owner-live trial and adopted a deterministic simulated gate as the interim
MVP-4 exit. This design specifies that gate as a machine-checked rig that:

1. launches the **candidate production executables** — `qiven-zcode-hook.exe`
   driven as an external process with the exact stdin JSON and argument/env
   form the harness supplies, and `qiven-runtime-host.exe` serving the real
   named pipe with the packaged profile — in isolated scratch roots under
   `.generated-temp/h1-sim/`;
2. replaces ONLY the caller and external inputs (ADR-0055 decision 1): no
   host handler is called directly and labeled integration;
3. derives fixtures from three independent sources with recorded provenance
   (decision 2), and takes every expected verdict from an independently
   specified oracle (accepted contract or recorded incident), never from
   current implementation output;
4. turns the five incidents into distinct old-fail/new-pass discriminating
   cases with plausible wrong fixes the suite rejects (decision 4);
5. treats any skip as NOT_VALIDATED (decision 5) and binds a receipt to the
   exact candidate head (decision 7);
6. records the writable-child/delegation bypass as a required negative
   coverage case that blocks complete-mediation claims (decision 8).

## 2. Components

| Component | Path | Role |
| --- | --- | --- |
| Rig driver | `tools/h1_sim_gate.py` | Scenario runner, receipt writer/verifier. Python stdlib only (Devkit python-standard law). Windows-only; a non-Windows invocation reports `[NOT_VALIDATED] platform leg skipped` and exits non-zero. |
| Fixture corpus | `tests/fixtures/h1-sim/` | Payload fixtures + `catalogue.json` (provenance + digests + oracle class per fixture). Tracked, canonical test inputs. |
| Wire fault client | inside the rig | A minimal protocol client (DPAPI-unprotected installation secret, HMAC-SHA256 framing per `src/ipc/framing.cpp`) used ONLY for fault-injection legs (bad deadline, wrong seq, oversized body) and the authenticated shutdown — never as a substitute for the real hook exe in lifecycle cases. |
| Gate wiring | `.qiven/operator.json` | Tasks `h1-sim` and `h1-kit-test` join the `local` publication gate after build/test, before diff-check. |
| Kit regressions | `tools/h1_kit_test.py` | Skip-success semantics fixed: absent host exe is `[NOT_RUN]` and FAILS the suite (ADR-0055 decision 5). Packaging checks unchanged. |
| Folded fixes | `src/adapter/zcode_hook.cpp`, `tools/h1_kit.py` | Trial-4 hello-deadline fix + preflight `session_start` registration coverage (see §7). |

## 3. Isolation model

Each scenario group gets a fresh scratch governed root:

```
.generated-temp/h1-sim/<run-id>/g<NN>/
  config/profiles/zcode-jason-context-record-mvp.yaml   # packaged profile (copied; fault variants marked)
  .qiven/runtime/                                        # journal.sqlite3, install.id,
                                                         # client.secret.dpapi, clients.json (host-created)
  target-files/                                          # effect-assertion targets (deny must leave these absent)
```

- The host boots with `--root <scratch> [--profile <file>]` exactly as the
  kit launchers do; the hook runs with `--event … --root <scratch>
  [--tool …] [--session-handle …]` and the fixture payload on stdin.
- Admission stays real: the rig never disables the install record; the
  same-directory rule (host boot merges sibling client images) is what the
  admission fault case deliberately violates by copying the hook exe
  elsewhere (the trial-3 second cause, reproduced for the same reason).
- Cognition refresh on a scratch root fails its `git fetch` (no origin) and
  falls back inside the boot-seeded freshness window (`local_fallback`), so
  sessions register healthy without network — deterministic and honest. A
  dedicated `--refresh expired` fault profile (`freshness_window_ms: 1`
  pre-seeded journal meta) produces the deny-117 class.
- Journal and effect assertions run after the group's host has exited
  (authenticated shutdown), plus interim verdict assertions from each hook
  invocation's exit code / stderr / session id.

## 4. Fixture provenance (ADR-0055 decision 2)

Every fixture records its source class in `catalogue.json`:

- **`pinned-zcode-29628c9`** — payload shapes constructed exactly per the
  pinned official interfaces (`apps/zcode-cli/packages/contracts/src/hooks/index.ts`
  at `zai-org/ZCode@29628c9`): camelCase `PreToolUseHookInput`,
  `PostToolUseHookInput`, `SessionStartHookInput` with the contract's
  `source ∈ {startup, resume, clear, compact}` enum and required
  `BaseHookInput` keys.
- **`capture-2026-09-23`** — the real dual-key wire form (both camelCase and
  snake_case copies of identity fields, `transcript_path`, `riskLevel`,
  `sideEffectScope`) captured live during the trials, from the trial-4 kit
  evidence `payload-probe.log`. Machine-identity literals are replaced by
  `{WORKSPACE_ROOT}`-class placeholders per the public-repo hygiene law;
  shapes are otherwise verbatim. The catalogue records the substitution.
- **`contract-derived`** — parameter sets derived from published contract
  statements (e.g. the SessionStart source enum; the required-ness of
  `cwd`/`sessionId`/`timestamp`).
- **`incident-audit`** — oracle values (verdict classes, deny codes, journal
  row expectations) taken from the five accepted incident audits, not from
  the implementation.

Oracle law: expected outcomes live in the scenario table (§6) and cite
their source (profile clause, protocol contract, incident audit). A
scenario may never be generated by running the candidate and copying its
output. Source parity with the installed Desktop build is a documented
assumption (`PINNED_SOURCE_CONTRACT_REVIEWED` covers the pinned-source
reading; `INSTALLED_DESKTOP_EXECUTION_UNVERIFIED` stays standing).

## 5. Case taxonomy and invariants

Groups (one host boot each, ordered steps inside), ~104 named cases:

| Series | Groups | Covers |
| --- | --- | --- |
| S — SessionStart registration | S1-S4 | hello+`session_start` on one real connection; one journal session row; stable id; typed verdict; idempotent repeat; all four contract sources; captured dual-key payload. |
| B — PreToolUse classification | B1-B6 | governed Write/Edit deny 110 without effect; Bash conservative detector deny 111; repo-root reference; outside-scope `not_governed` under real preconditions; correlation (one outstanding per tool); unregistered session deny 114 BEFORE classification; unknown tool; traversal form; empty extraction fail-closed; oversize/unreadable payload deny 118; payload/template tool contradiction deny 118. |
| C — PostToolUse & closure | C1-C3 | correlated post_tool clears outstanding; unregistered post_tool degrades to Indeterminate (never silent success); authenticated shutdown acks and the process EXITS; restart isolation (journal survives, sessions re-register); duplicate handle idempotent; cross-session identity isolation. |
| D — Packaging & path forms | D1-D3 | shipped-kit entrypoint form (assemble_kit → boot from kit cwd with kit-internal profile); kit-without-profile fails closed naming the ROOT-derived path (CWD cannot substitute — the preflight incident); backslash/trailing-slash/case/relative/Unicode/short-name fixtures asserting the documented lexical policy, with detector-scope limits recorded in the coverage map (never claimed as authorization equivalence). |
| I — the five incidents as distinct old-fail/new-pass cases | I1-I5 | see §6. |
| W — writable-child bypass negative control | W1 | a child process with no hook mediation writes inside the governed root and SUCCEEDS unmediated; the rig asserts this observable fact and marks complete-mediation blocked for delegation paths (pinned source: subagents carry no hook runner). |

Invariant map (every case cites one or more; the receipt's coverage map is
generated from these bindings):

- INV-1 registration exactly once per handle (idempotent, stable identity)
- INV-2 unknown-session pre_tool denies 114 before any classification
- INV-3 governed mutations denied without executing an effect
- INV-4 `not_governed` only under its real preconditions
- INV-5 unreachable host denies 120 with honest fail-closed text
- INV-6 the payload digest binds the exact bytes sent (no re-encoding)
- INV-7 correlation completeness (outstanding per tool; post_tool clears)
- INV-8 every lifecycle verdict leaves its journal row
- INV-9 hello+event ride ONE connection, multi-frame (the wire contract)
- INV-10 the whole hello+event transaction closes inside one monotonic
  bound (a per-frame-only timer cannot close trial 4)
- INV-11 kit/profile self-containment (root-derived default; kit-internal
  explicit profile; CWD cannot substitute)
- INV-12 admission is typed (121) and governed by the install record
- INV-13 the Bash detector conservatively over-approximates
- INV-14 exit-contract mapping (0 allow/not_governed/advisory; 2 deny)
- INV-15 a deny never executes the effect (target file stays absent)
- INV-16 shutdown acks, then the host process exits
- INV-17 restart preserves the journal and re-establishes isolation
- INV-18 delegation paths are NOT mediated (recorded negative; blocks
  complete-mediation claims)

## 6. The five incidents (distinct old-fail/new-pass, wrong fixes rejected)

Each case runs its old-fail leg on the **historical defective revision or
an explicit fault injection for the same reason** (ADR-0055 decision 4) and
its new-pass leg on the candidate:

- **I1 trial 1 (assumed REQUIRED field)** — old-fail via fault injection:
  the rig class-reimplements nothing; it feeds the exact breaking payload
  (no session-identity field anywhere) and asserts the candidate SUBMITS it
  (registration proceeds; no deny 118). Wrong fix rejected: a payload whose
  tool CONTRADICTS the registration template must still deny 118
  (misregistration) — "ignore the payload entirely" is rejected by this
  case.
- **I2 trial 2 (borrowed-payload lifetime)** — new-pass: HookRun owns the
  bytes; the host-observed `payload_sha256` equals the sha256 of the exact
  stdin bytes for payloads containing quotes, backslashes, non-ASCII and
  re-encoding-sensitive forms (extra whitespace, duplicate keys). Wrong fix
  rejected: a re-serialization digest mismatches the sent bytes.
- **I3 trial 3 (one-frame host + admission mismatch)** — new-pass: the real
  hook's hello+event on ONE connection completes (registration). Old-fail
  fault: a fault-injection leg drives the same two-frame sequence against
  the wire with `connection_seq` regression and asserts the typed 63/64
  replies; the admission fault (hook exe copied outside the host's install
  directory at boot) reproduces the typed deny-121 second cause for the
  same reason. Wrong fix rejected: a single-frame client shape (the
  runtimectl pattern) cannot satisfy the registration assertion.
- **I4 trial 4 (hello/event deadline split)** — old-fail: the PRE-FIX
  revision is the current `main` binary set (the defect is live there), so
  the rig's session_start registration case run against a pre-fix host/hook
  pair fails for the recorded reason (hello 9750 > 5000 ceiling → advisory
  reject → deny 114 everywhere). New-pass: the candidate (§7 fix) registers.
  The rig also carries the wire-level fault injection (hello deadline 9750)
  for the same reason so the leg survives after the historical binary is
  gone. Wrong fix rejected: INV-10's monotonic whole-transaction bound — a
  per-frame-only clamp that lets the total exceed the hook budget fails.
- **I5 kit-preflight incident (CWD profile)** — old-fail fault: an assembled
  kit stripped of `config/profiles` and launched without `--profile` from
  the kit cwd fails closed (exit 2) naming the ROOT-derived profile path;
  the CWD profile that happens to exist is never used (asserted directly).
  New-pass: the intact kit boots from its own root. `h1_kit_test.py` keeps
  its packaging regressions; this leg exercises the shipped entrypoint form
  live.

## 7. Folded fixes (ADR-0055 Consequences)

1. **Hello deadline (`src/adapter/zcode_hook.cpp`)** — the hello frame is a
   handshake, not the event: the client sends (and times) hello at the
   interaction ceiling (≤ 5000 ms) regardless of the event's refresh-grade
   budget; the event frame keeps `run.deadline_ms`. This is the trial-4
   one-line fix; the conformance "hello-with-refresh-deadline" case (a
   session_start round trip) is the regression.
2. **Preflight `session_start` coverage (`tools/h1_kit.py`)** — the enable-
   gated preflight now drives a session_start registration (not only a
   pre_tool round trip) before its PASS line, closing the trial-4 preflight
   blind spot.

## 8. Receipt binding and publication gate (ADR-0055 decisions 5 + 7)

`tools/h1_sim_gate.py run` (the `h1-sim` gate task):

- asserts a clean tree, resolves the exact HEAD, and records: fixture
  catalogue digest, per-fixture digests, scenario table digest, candidate
  exe digests (host/hook/ctl), profile digest, git HEADs (runtime +
  `.qiven/dependencies.json` content digest), platform (OS version),
  toolchain (compiler line), Python version, and the exact command line;
- writes `.generated-temp/h1-sim/receipts/sim-<head>.json` containing the
  per-group/per-case results, the invariant coverage map, zero-skip
  attestation, and the typed states `SIMULATED_HOOK_HOST_PASS` /
  `PINNED_SOURCE_CONTRACT_REVIEWED` / `INSTALLED_DESKTOP_EXECUTION_UNVERIFIED`;
- exits non-zero on ANY failed OR skipped case (`NOT_VALIDATED`), so the
  publication gate (which treats task exit codes as verdicts) rejects
  absent, skipped, stale and failed receipts by construction: a new head
  always re-runs (no receipt caching), and `verify-receipt` re-checks an
  existing receipt's digests against the live tree for audits and kit
  building.

The `local` gate order becomes: check-toolchain, third-party-verify,
format-check, configure, build-debug/release, test-debug/test-release,
**h1-sim**, **h1-kit-test**, diff-check, clean-tree.

## 9. Deliberate scope boundaries

- No Foundation typed-path prerequisite, no ZCode transplant, no real
  harness build/launch on this machine (ADR-0055 decision 9; the ban is
  honored — the rig runs only Qiven binaries and reads pinned source).
- Lexical path-policy fixtures assert the DOCUMENTED policy and record
  detector-scope limits; they make no authorization-equivalence claim.
- The rig's wire client exists for fault injection and shutdown only; every
  lifecycle case goes through the real hook exe.
- C++-level regressions already covered by `ipc_multiframe_contract`,
  `hook_conformance`, `host_lifecycle` are not duplicated; the rig adds the
  external-process, real-pipe, whole-exe dimension those tests cannot.
