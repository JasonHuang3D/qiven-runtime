# C++ Diagnostics I0 Census — Executables, Handlers, Mechanisms, Budgets

> Status: I0 deliverable of the C++ diagnostics infrastructure program
> (qiven-docs PR6, accepted 2026-09-29; adopted as program law by ADR-0059,
> accepted 2026-09-29). Census only — no implementation is claimed; all
> mechanism selections below are **provisional pending the predeclared
> measurement batch** (section 6), which has NOT been executed. Baseline
> SHAs: qiven-runtime `339d27d`; qiven-foundation
> `032a6152efa18407d7b8d29d3203a5def179385a` (F0 landed there 2026-09-29); workspace generation
> `sha256:861ea68381acafc654e01813190c170a3c33687c36b8dc5a77e9ee739ede5010`
> (lock read 2026-09-29; foundation node `032a615`, context `d5a8f2b`,
> runtime `339d27d`, docs `2dc2f0f`). No build ran; every measured quantity
> is NOT-MEASURED with its predeclared plan.

## 1. Authority and scope

Decision chain: ADR-0059 adopts the PR6 three-document program (program doc
00 §3 = this stage's contract; architecture doc 01; boundary amendment doc
02) and authorizes F0 — which has now LANDED at qiven-foundation `032a615`
(amended foundation.md §3/§9/§10/§11/§13 + F0 baseline document). This
census is the I0 work item: enumerate the governed executable/fault
surfaces, study pinned third-party mechanisms, make provisional per-class
selections, predeclare the comparison protocol and pass/fail budgets, and
fix the per-class evidence contract — all BEFORE implementation (doc 00 §3).

What this is not: no build, configure, test run, or measurement was
executed in producing it (production-run constraint); no provider is
"accepted" (ADR-0059 Decision 6 — dependency admissions are recorded
decisions presented to the owner, and the measured comparison that decides
them is predeclared in section 6, not run); no OS support claim beyond
Windows-first declarations (F0 baseline §7 is cross-referenced, not
duplicated).

## 2. Governed executable and module census

### 2.1 Production executables (qiven-runtime `339d27d`)

All four are CMake `add_executable` targets from the root `CMakeLists.txt`
(lines 257-307), each linking the static `qiven-runtime` library
(`qiven::runtime`), which links `qiven::foundation` (STATIC, foundation
CMakeLists.txt:16) and `qiven::tp::sqlite3` (PRIVATE), plus `crypt32` on
Windows. Entry form for all four: `int main(int argc, char** argv)`.

| Executable | Source | Process-wide handlers installed at startup | stdio redirection | Logging/output practice | Loader-order / duplicate-global risk |
| --- | --- | --- | --- | --- | --- |
| qiven-runtime-host | apps/runtime_host_main.cpp | `qiven::install_headless_crt_failure_behavior()` first statement (line 87; installs UCRT invalid-parameter handler, `_set_abort_behavior`, Debug-only `_CrtSetReportMode/_CrtSetReportFile`); `SetConsoleCtrlHandler(console_handler, TRUE)` (line 247, before serving) | ONE site: `--log <file>` mode `_open` + `_dup2` onto stdout and stderr fds (lines 183-194; the 2026-09-27 incident fix — comment block lines 160-182 records the mechanism) | `std::printf`+`std::fflush(stdout)` staged markers, per-request `[conn]` lines, 30 s `[beat]` heartbeat; appends unboundedly to one `--log` file (no rotation, no size cap); durable audit surface is SQLite journal (`journal.sqlite3`, WAL) | threads: serve pool (thread-per-connection, typed 125 cap), refresh worker, heartbeat thread; ends `std::exit(0)` (line 477) skipping local destructors by design |
| qiven-adapter-bridge | apps/adapter_bridge_main.cpp | `install_headless_crt_failure_behavior()` (line 87) | none | `printf`/`fprintf(stderr)`/`cout`; state file `.generated-temp/.../state.log` atomic temp+rename | one-shot CLI; no threads |
| qiven-runtimectl | apps/runtimectl_main.cpp | `install_headless_crt_failure_behavior()` (line 1355) | none | `std::cout` human text; exit 0/1/2 | one-shot CLI; spawns nothing |
| qiven-zcode-hook | apps/zcode_hook_main.cpp | `install_headless_crt_failure_behavior()` (line 56) | none | stderr notes only (verdict mapping); optional `--dump-stdin` payload probe file (append) | one-shot per harness hook invocation; 15 s/10 s harness budgets |

Additional terminate paths (not handler installs): Foundation
`qiven::detail::contract_fail` (`QIVEN_ASSERT`/`QIVEN_VERIFY`/
`QIVEN_UNREACHABLE`, foundation src/contracts.cpp:75-108) — snprintf on
the fault path, stderr-handle `WriteFile`, `OutputDebugStringA`,
`DebugBreak` if debugger attached, then `std::_Exit(3)` on Windows
(avoiding the abort modal; contracts.cpp comment records the 2026-09-19
incident). `QIVEN_ASSERT` compiles to nothing in Release;
`QIVEN_VERIFY`/`QIVEN_UNREACHABLE` are active in both. Runtime production
sources contain ~20 QIVEN_ASSERT/VERIFY sites. Test-only
`TerminateProcess` crash injection exists solely behind
`QIVEN_RUNTIME_TEST_CRASH_POINTS` (src/journal/recovery.cpp:35-73; the
production build contains zero hook state).

### 2.2 Static-initializer and dynamic-module audit

- Grep-anchored audit of `src/`, `apps/`, `include/` at `339d27d`: the
  only file-scope mutable globals in production code are
  `volatile BOOL g_stop` / `ServeLoop* g_loop` (runtime_host_main.cpp:48,53
  — zero/pointer constant-init, no dynamic initializer) and the
  test-guarded crash-hook globals (recovery.cpp:42-43, absent from
  production builds). No dynamically-initialized statics were found.
- No `LoadLibrary`/`FreeLibrary`/`GetModuleHandle` use exists anywhere in
  runtime production sources. No dynamically loaded modules, plugins, or
  additional CRT instances are declared; every target is compiled from
  first-party source in-tree (vendored sqlite3 is compiled into the
  consumer build tree, root CMakeLists.txt:51).
- Static-copy topology: Foundation and qiven-runtime are STATIC; each of
  the four executables embeds its own copy — one per process, so no
  cross-image duplicate process-global state exists TODAY. The duplicate-
  global risk is prospective (F1 shared distribution / F2 module ABI),
  already covered by amended foundation.md §10 ("multiple static copies of
  the process service in one process are a configuration error").
- MSVC runtime linkage: no `/MT`/`/MD` override exists in either
  CMakeLists; CMake VS-generator defaults apply (Debug `/MDd`, Release
  `/MD` → dynamic UCRT). NOT-VERIFIED against build output (no build ran).

### 2.3 Symbols/PDB production in Debug/Release

No `/Zi`, `/DEBUG`, or PDB-related flag appears in either repository's
CMakeLists or CMakePresets.json. VS-generator CMake defaults produce PDBs
for Debug but NOT for the plain Release configuration (the gate's
`vs2022-x64-release` leg; `RelWithDebInfo` is the symbol-bearing variant
and is not built). `.qiven/deploy.json` ships four Release exes with no
`.pdb` products. Consequence: **Release symbol retention is currently
absent** — a standing program obligation (doc 01 §5 "matching PDBs … in
Release as well as Debug") that I1/I2 must add (build flag + deploy
product + receipt binding). NOT-VERIFIED against actual build output; the
claim rests on the declared build configuration.

### 2.4 Gate-built test-binary classes (workspace repos)

qiven-runtime tests/CMakeLists.txt at `339d27d`: **52 test executables**
(30 direct `add_executable` + 22 via the `qiven_runtime_add_journal_test`
helper) and **52 ctest registrations**, plus 1 OBJECT header-check library
(`qiven-runtime-header-check`, 57 header-inclusion TUs). Classes relevant
to diagnostics: (a) all link `qiven::runtime` statically and inherit the
Foundation callback surface; (b) `qiven-runtime-journal-crash-recovery`
compiles the journal sources directly with `QIVEN_RUNTIME_TEST_CRASH_POINTS`
(the only production-code copy compiled with live TerminateProcess hooks);
(c) `host-server-lifecycle` boots the REAL production executables
(`QIVEN_RUNTIME_BIN_DIR`, TIMEOUT 180) — it is the standing gate task
carrying the CRT regression (section 3); (d) `third-party-sqlite` and the
journal class exercise the vendored sqlite in-process. qiven-foundation's
own test set includes `tests/crt_failure.cpp` (the primitive's unit
surface). Python gate rigs (`tools/h1_sim_gate.py`, `h1_kit_test.py`) are
drivers, not governed binaries; they are wired as gate TASKS
(`.qiven/operator.json`: `h1-sim`, `h1-kit-test` inside the `local` gate
order after test-debug/test-release).

### 2.5 Handler-site classification and unknown-site count

Complete handler/redirect census over tracked `*.cpp`/`*.hpp` at
`339d27d` (git grep for `SetConsoleCtrlHandler`,
`SetUnhandledExceptionFilter`, `_set_invalid_parameter_handler`,
`_set_abort_behavior`, `set_terminate`, `_set_purecall_handler`,
`signal(`, `AddVectoredExceptionHandler`, `SetErrorMode`,
`_CrtSetReportMode`, `_set_se_translator`, `freopen`, `_dup2`, `SetStdHandle`):

- Process-wide handler installs: exactly TWO sites — the Foundation
  installer invoked once per executable (4 call sites, one install set
  each) and `SetConsoleCtrlHandler` (host only). The Foundation install
  set itself is three Windows setter groups (crt_failure.cpp:91-105).
- NOT installed anywhere: unhandled-exception filter, `std::terminate`
  handler, purecall handler, signal handlers, vectored handlers, error
  mode. An uncaught C++ exception or an access violation therefore still
  follows the CRT/OS default path today.
- stdio redirection: exactly ONE site (host `--log`, section 2.1).
- **Unknown process-global handler sites: 0** in first-party tracked
  sources. Explicit gap (not zero-knowledge): the vendored
  `qiven-third-party-win` sqlite3 amalgamation was NOT line-audited — that
  repository is outside this census's admissible read set. The I1 Devkit
  static forbidden-pattern check must therefore cover vendored sources as
  an explicit step before "zero" can be claimed for linked images.

## 3. The two CRT incident classes

Both classes are bound to their named reproducibility wiring; the h1-sim
catalogue (`tests/fixtures/h1-sim/catalogue.json`) itself carries the
hook-lifecycle scenarios (oracle_bindings for deny 110/111/118/120/121/125
etc.) — the CRT classes' reproducibility lives in the C++ suite and the
kit/gate tasks, named below.

| Axis | Class A — UCRT invalid parameter on corrupted/aliased stdio (2026-09-27 MVP-4 H1 kit Release incident; MEM-20260927T093500Z-B2C3D4 per ADR-0059) | Class B — Debug CRT assert/abort modal in headless operation (2026-09-19 modal-abort law; MEM-20260919T153758Z-D6B4E7 per crt_failure.hpp) |
| --- | --- | --- |
| Mechanism | spawned tool aliases stderr onto stdout as ONE inherited handle; host `--log` redirects both streams; UCRT lowio table corrupts; first `printf` dies inside `fwrite` (invalid parameter) | CRT fault (assert/abort/uncaught-terminate) in unattended run raises "Debug Assertion Failed"/"abort() has been called" modal; process waits for operator click; terminates on every button |
| Release, pre-fix | `_invoke_watson` → `__fastfail(FAST_FAIL_INVALID_ARG)` (pinned MS doc) → exit `0xC0000409`, zero-byte log, no output | Debug-CRT report surface does not exist in Release; the same fault routes to Class A machinery or plain abort text |
| Debug, pre-fix | invalid parameter routes through `_CrtDbgReportW` → "Debug Assertion Failed" modal (blocks headless) | modal UI hang (the recorded ctest "hang" was operator click latency; an uncaught-exception terminate reached the abort dialog — foundation contracts.cpp comment) |
| Release, current fix | `qiven_invalid_parameter` runs (no expression/file available in Release — crt_failure.cpp:77-79) → evidence line via `WriteFile` on the stderr HANDLE + `TerminateProcess(3143)`; if that handle is unusable: silent 3143, no durable evidence | n/a (Class B is Debug-shaped; current Release behavior unchanged from pre-fix row) |
| Debug, current fix | same handler takes precedence in either CRT flavor (crt_failure.hpp header); verbose expression/function/file in the evidence line | `_CrtSetReportMode(_CRT_ASSERT/_CRT_ERROR, _CRTDBG_MODE_DEBUG|_CRTDBG_MODE_FILE)` + `_CRTDBG_FILE_STDERR` (no `_CRTDBG_MODE_WDW`); `_set_abort_behavior(_WRITE_ABORT_MSG, …|_CALL_REPORTFAULT)`; `contract_fail` avoids abort entirely (`std::_Exit(3)` + DebugBreak-if-debugger) |
| Usable stack | none in-process in any leg (callback has no EXCEPTION_POINTERS; 2026-27 diagnosis required an external CDB symbol session) | none |
| Evidence owner | the CRT callback's stderr-handle write (best effort); nothing beyond process lifetime; no dump, no supervisor correlation | same (contract_fail's stderr+debugger write for the assert channel) |
| Standing reproducibility | `tests/host_server_lifecycle.cpp` "0. ALIASED-STDIO boot" row (lines 350-424; boots the real host with one aliased inherited handle + `--log`; asserts boot + nonzero log; on death prints the 3143-vs-0xC0000409 discriminator) — ctest `qiven-runtime.host-server-lifecycle` in both gate test legs; kit `start-host` classifies exit 3143 as the CRT-fault evidence class (tools/h1_kit.py, preflight troubleshooting text) | the same lifecycle row + Foundation `tests/crt_failure.cpp`; the modal shape itself is not driven by an automated leg (a modal cannot be asserted headless — covered by install-time assertions on the report modes) |

Current callback residual weaknesses (foundation `src/crt_failure.cpp`
at `032a615`, exact lines): `std::strlen` on the fault path (line 33);
`std::snprintf` evidence/tail formatting (lines 50, 69, 77);
`GetStdHandle(STD_ERROR_HANDLE)` dependence (line 45) — the very handle
class the incident corrupts; fixed-code-only termination
`TerminateProcess(3143)` (lines 43, 57) with no fault-context capture, no
dump, no inspector handoff, and one exit code doubling as both
"CRT-fault evidence" and generic termination. Present and correct:
`InterlockedExchange` reentrancy guard (line 29), no stdio-stream use,
no return from the callback.

## 4. Pinned third-party mechanism study

Pins resolved live 2026-09-29. Study axes per program doc 00 §3
(security/maintenance history, shutdown, backpressure, writer stall,
context fidelity, crash race, platform/build support, offline packaging,
deployment privilege, license/NOTICE). Third-party precedent is evidence,
not authority.

### 4.1 Pin rows (all resolved; none NOT-VERIFIED)

| Candidate | Pin | Resolved value | Date | URL / mirror note |
| --- | --- | --- | --- | --- |
| spdlog | latest stable release tag | `v1.17.0` = `79524ddd08a4ec981b7fea76afd08ee05f83755d` | published 2026-01-04T17:11:18Z | github.com/gabime/spdlog (releases.atom) |
| Quill | latest stable release tag | `v13.0.0` = `eb802a37c7d585840324886a3d8648c9c2159952` | published 2026-08-30T10:33:41Z | github.com/odygrd/quill (releases.atom) |
| Crashpad | current main commit | `ce308a86daa85e65df219ff5fb385095b0a1c467` ("Default some members of IOSSystemDataCollector") | fetched 2026-09-29 (repo page rendered "4 days ago" ≈ 2026-09-25; the googlesource commit page itself 503'd, so the exact timestamp is recorded as approximate) | chromium.googlesource.com/crashpad/crashpad — read at that host; the getsentry/crashpad GitHub mirror was used ONLY to verify it exposes the SAME commit ce308a8… and to read `doc/status.md` + LICENSE after googlesource rate-limited (503) |
| MS RaiseFailFastException | platform doc | ms.date 2018-12-05; updated 2025-08-27 | — | learn.microsoft.com/…/nf-errhandlingapi-raisefailfastexception |
| MS CRT invalid-parameter | platform doc | ms.date 2020-04-02; updated 2022-12-02 (cpp-docs git `fe1e4033…`) | — | learn.microsoft.com/…/invalid-parameter-functions |
| MS MiniDumpWriteDump | platform doc | ms.date 2018-12-05; updated 2024-02-22 | — | learn.microsoft.com/…/nf-minidumpapiset-minidumpwritedump |
| MS MINIDUMP_EXCEPTION_INFORMATION | platform doc | ms.date 2018-12-05; updated 2024-02-22 | — | learn.microsoft.com/…/ns-minidumpapiset-minidump_exception_information |
| MS WER LocalDumps | platform doc | ms.date 2024-07-18; updated 2025-03-11 (win32-pr git `03b700b5…`) | — | learn.microsoft.com/…/collecting-user-mode-dumps |

### 4.2 Study rows

| Axis | Quill v13.0.0 | spdlog v1.17.0 | Crashpad @ ce308a8 | DbgHelp/MiniDumpWriteDump + WER + external profile | Smallest-Qiven alternative |
| --- | --- | --- | --- | --- | --- |
| Architecture fact (pinned source) | per-producer-thread lock-free SPSC queue; caller binary-serializes arguments (no formatting on caller); single backend worker drains, merges by timestamp, formats with bundled fmt | async loggers push each message + shared_ptr to a shared global thread-pool queue (default 8192 items, one worker) | in-process client + out-of-process handler + on-disk database + uploader; Chromium's reporter on macOS/Win/Android | OS facility: kernel/WER-mediated | MPSC bounded queue + one writer thread + preallocated ring (doc 01 §4-5 shape) |
| Backpressure | bounded (default 131072 B) or unbounded frontend queues; overflow blocking or dropping; dropped/realloc/blocked counters observable | `block` (default: producer waits when full — can stall the app) vs `overrun_oldest` (drop oldest, never block) | client→handler IPC bounded by design; database/retention managed | n/a | drop/replace + loss counters + reserved lane per doc 01 §4.3 |
| Writer stall | backend thread stalls back up into queue policy; grace window for late producers before timestamp merge | pool worker stall → queue fills → policy engages | handler is a separate process — a wedged target does not wedge the writer of the report | inspector/supervisor owns timeout (doc 01 §7.3 law) | bounded wait + health counter; crash path never waits for writer |
| Shutdown | documented hazard: do not log from static/global destructors (singleton destruction order); fork requires backend per child + separate files | thread pool flush/join on teardown; same destructor-order class of hazard | handler outlives client by design; report finalization asynchronous | process exit is the boundary | explicit shutdown+timeout contract (I1 test list) |
| Crash context fidelity | logging only; not a crash mechanism (backend must not be on the fault path) | same | native exception capture + dump streams, cross-platform | MiniDumpWriteDump from separate process recommended; native record/context only where supplied (SEH); ClientPointers semantics fixed by pinned doc | capsule + labeled callback context (no invented native context) |
| Crash race | n/a | n/a | the reason the handler is out-of-process | in-process DbgHelp inherits corrupted state; single-threaded API (pinned doc) | inspector + supervisor timeout; no in-process lock |
| Platform/build support | C++17+; CI Fedora/Ubuntu/BSD/macOS/Windows/Intel LLVM; Android NDK | header-only optional; C++11-era baseline, broad compiler matrix | doc/status.md: complete clients for macOS, Windows, Linux (incl. Android/Chromium OS), Fuchsia; iOS in development; processor only planned | Windows-native (WER per-OS); Linux/macOS need their own profiles (F3) | Windows first (UCRT/SEH law); others gated at F3 |
| Offline packaging / deployment privilege | source build; no privilege | source build; no privilege | client lib + handler executable + database directory in the deployment; no admin privilege for the app-local handler; Chromium-scale build closure (gn/cmake) | WER LocalDumps: NOT enabled by default, requires ADMIN to set HKLM registry keys; per-application key overrides (DumpFolder/DumpCount=10/DumpType=1) | source build; no privilege; artifacts under `.qiven/diagnostics/` |
| Security/maintenance history | single-maintainer project, active (v11→v13 in 2026); no pinned CVE review performed in this run (NOT-MEASURED; the I1 provider decision must add it) | mature, broad user base; same CVE-review gap (NOT-MEASURED) | Chromium-maintained, security-reviewed continuously as Chromium infra; doc-only review here (NOT-MEASURED beyond status doc) | OS-maintained | Qiven-owned maintenance cost by definition |
| License/NOTICE | MIT (© 2020-present Odysseas Georgoudis); bundled fmt + doctest also MIT | MIT (© 2016-present Gabi Melman and spdlog contributors); bundled fmt MIT | Apache-2.0 (LICENSE read at the mirror; standard terms: license copy, marked modifications, NOTICE carry-over, patent grant) | OS documentation, no code obligation | none (first-party) |

Key pinned-doc invariants feeding Qiven contracts (mapping rows, doc 00
§3 work item 4): `_invoke_watson` uses `__fastfail(FAST_FAIL_INVALID_ARG)`
where supported, else a fast-fail exception catchable by an ATTACHED
DEBUGGER, else `TerminateProcess(STATUS_INVALID_CRUNTIME_PARAMETER)`; the
thread-local invalid-parameter handler takes precedence over the global
one; user handlers "may terminate or return", MS recommends terminating →
Qiven contract: nonreturning callback (discriminating test: a returning
handler must fail the I2 probe). `RaiseFailFastException` "bypasses all
exception handlers (frame or vector based)" → direct fail-fast cannot be
claimed by any in-process filter (external-evidence class). DbgHelp is
single-threaded; `MiniDumpWriteDump` "should be called from a separate
process if at all possible"; the calling thread's own stack may be absent
unless its state is supplied as ExceptionParam; handle rights are
PROCESS_QUERY_INFORMATION + PROCESS_VM_READ (+ PROCESS_DUP_HANDLE for
handle data) + thread access → Qiven inspector registration contract.
WER LocalDumps: "Applications that do their own custom crash reporting are
not supported by this feature" → a WER profile is a SEPARATE capability
qualification, and coexistence with a Qiven crash client must be probed,
never assumed (doc 01 §7.6 law).

## 5. Provisional mechanism selections

All rows: **provisional pending the predeclared measurement batch**
(section 6); revisit triggers recorded; per ADR-0059 Decision 5/6 these
are candidate evidence for an owner-adjudicated recorded decision, not
admissions; the amended foundation.md §3 is the admissibility gate.

| Fault class (Windows first) | Provisional selection | Rationale (evidence-cited) | Revisit trigger |
| --- | --- | --- | --- |
| UCRT invalid parameter (original H1 class) | Qiven-owned Win32 crash client extending the existing primitive per doc 01 §13 (bounded fixed-buffer encoding, preopened independent emergency handle, nonreturning, capsule publish + out-of-process inspector); Crashpad client as the measured comparator | the class is UCRT-callback-shaped (no EXCEPTION_POINTERS — pinned doc); existing primitive already owns the install point; MS handler contract allows a terminating custom handler; evidence grade is callback-origin by construction | measurement batch shows Crashpad (or another provider) meeting the same grade at lower ownership cost; or any I2 probe failure (ADR-0059 revisit law: mechanism may change, contract may not) |
| Unhandled SEH / access violation | unhandled-exception filter capturing native EXCEPTION_POINTERS → inspector-process `MiniDumpWriteDump` (ClientPointers matched to verified representation) | pinned doc: separate-process capture recommended; native context exists only here; DbgHelp single-thread constraint is satisfied by one inspector | as above |
| terminate / abort / pure virtual call | `set_terminate` + `_set_abort_behavior` + `_set_purecall_handler` unified into the same client; separately captured callback stack/context, labeled non-native | callbacks supply no native context (doc 01 §6.2 law); modal-suppression already proven by the existing install set | as above |
| Direct fail-fast (`RaiseFailFastException`/`__fastfail`/stack cookie) | NO in-process claim; external profile: supervisor exit record + independently qualified WER LocalDumps per-app key or ProcDump/CDB profile | pinned doc: bypasses all handlers; WER custom-reporting-unsupported clause forbids assuming coexistence | a qualified external profile fails to reproduce coverage → class stays visibly uncovered |
| Pre-main / loader failure | external process-start/exit observation + supervisor record; external dump only after independent qualification | no in-process code has run (doc 01 §5) | — |
| Logger transport (normal path) | comparative: Quill v13.0.0 vs spdlog v1.17.0 vs smallest-Qiven MPSC+ring, decided ONLY by the section 6 batch | per-thread SPSC (Quill) avoids shared-queue contention; spdlog `block` default violates the no-unbounded-wait law unless `overrun_oldest` is forced; both are MIT with modest NOTICE cost; Qiven-own pays maintenance but zeroes dependency terms | any I1 budget failure or saturation probe loss beyond declared policy |

Primary capture-stack preference per deployment profile (provisional):
interactive/development → console stderr + debugger sinks; resident
server (host autostart shape) → inspector + rotating structured file;
one-shot harness clients (hook/bridge) → bounded capsule + emergency
file, never a modal, never an unbounded wait. All class-specific
exceptions share the one Qiven artifact/health contract (one crash
directory, one manifest, one health surface — doc 01 §2/§7.4); no
duplicate upper-layer pipeline.

## 6. Predeclared comparison protocol (the FIRST I1-entry act; not run now)

Same-exact-build discipline: all candidates compiled in ONE build graph
per configuration (VS 2022 x64 Debug+Release, pinned workspace toolchain
node `9f6ce2a9…`), same MSVC runtime linkage, same optimization flags,
same hardware and OS session, interleaved A/B/A run order, identical
workload drivers. Workloads (from the actual governed shapes, section 2):
(W1) hook-shaped one-shot burst (single-thread, 10³ events, exit — the
qiven-zcode-hook profile); (W2) resident host shape (1 producer + serve
pool activity + 30 s beat, sustained 10 min — the host profile);
(W3) journal-heavy burst (multi-producer saturation at 2× and 10× drain
rate); (W4) reserved-lane saturation (Critical lane full while Trace
floods); (W5) crash-during-write (fault injected at writer mid-buffer).
Candidates: Quill v13.0.0, spdlog v1.17.0 (both overflow modes), smallest
Qiven transport; crash side: Qiven client+inspector vs Crashpad client
(class A/B probes only) vs WER LocalDumps per-app profile (fail-fast
class only). Counterexample probes (each must produce the DECLARED
degraded outcome, not a hang): writer stalled 30 s; sink disk-full;
unwritable root; queue overflow at each severity; shutdown flush with
stalled writer; inspector absent; inspector crashed; target early-exit
during dump; handler reentrancy; crash before normal startup. Metrics:
producer enqueue p50/p95/p99 and worst-case wait per severity (ns,
rdtsc/QPC); background CPU (% core, writer thread); queue occupancy and
loss counts; resident memory delta (working set, commit); startup cost
(process start→first event, ms); deployment footprint (binary/directory
bytes); dump capture duration and artifact bytes; crash-ring snapshot
duration; rotation/flush cost. Report per doc 00 §7: raw measurements are
NOT an acceptance receipt without these predeclared pass conditions.

## 7. Pass/fail budgets (predeclared thresholds; formulas where a measured baseline is required first)

| Axis | Budget (pass condition at I1/I2 gates) |
| --- | --- |
| Producer tail latency | candidate async p99 ≤ 1.20 × instrumented-baseline p99 AND ≤ absolute cap = 2 × baseline p50 + 1 µs; baseline measured in the SAME batch on the no-op sink (formula, not a fake absolute) |
| Saturation / worst-case producer wait | Warn/Error enqueue never blocks unbounded: hard ceiling 50 ms at 10× saturation (drop/replace then); Critical lane bounded wait ≤ 10 ms then ring-path fallback; Trace/Debug may drop with counters immediately |
| Aggregate memory | bounded by construction: queue bytes + ring + writer buffers ≤ configured cap; pass = zero unbounded-growth trend across a 10-min W3 run (slope ≤ 0 after warmup) |
| Disk / retention | rotating file: size-bound × generation-count (declared policy defaults 64 MiB × 5, deployment-profile-configurable); crash artifacts: ≤ 64 MiB/dump default type, ≤ 512 MiB aggregate, ≤ 100 crash directories, storm rate cap 10/hour with oldest-first cleanup |
| Background CPU | writer thread ≤ 5% of one core at sustained governed rate (W2); ≤ 25% at 10× saturation without loss beyond declared policy |
| Static/shared/module-call overhead | candidate p99 emit-overhead delta between static and shared linkage ≤ 10%; module ABI batch call ≤ 2 × direct C++ call p99 (F2 measurement) |
| Startup / deployment | diagnostics install ≤ 5 ms wall (W1); added deployment bytes per candidate recorded and compared (no absolute cap — selection criterion) |
| Dump duration / size | capture acknowledgement ≤ 5 s for the default dump type in the healthy profile (supervisor deadline 30 s); dump size within declared type budget; manifest finalization asynchronous |
| Failure degradation | every counterexample probe produces its declared bounded outcome: no hang, no unbounded memory, no modal, no invented success; missing evidence recorded as its own typed state |

Where a number above needs the measured baseline first, the
baseline-measurement step is declared inside section 6 (no-op-sink leg)
and the budget stays a formula until that batch runs — no fabricated
absolute is asserted. Measured coverage vs portability: all budgets
resolve on Windows x64 MSVC first; Linux/macOS uncovered until F3.

## 8. Per-class evidence contract (specified before testing)

Grades (doc 00 §3 work item 5): **(A)** native exception record/context +
completed dump resolving the faulting instruction (SEH classes only —
only they supply EXCEPTION_POINTERS); **(B)** captured callback
stack/context, labeled callback-origin (UCRT invalid parameter,
terminate, abort, purecall — never presented as native context);
**(C)** reason-only bounded evidence (callback context unobtainable:
stack exhausted, reentrancy guard hit); **(D)** external dump under an
independently qualified OS profile (direct fail-fast, pre-main);
**(E)** supervisor exit record (what the independent observer saw;
always available as floor, never inflated into a dump claim). Class→grade
capability: UCRT→B (C under degradation); SEH/AV→A; terminate/abort/
purecall→B; direct fail-fast→D+E; pre-main→E (+D when qualified);
inspector absent / writer wedged / disk full → C+E with the surviving
client observations named. Inspector transport rules: a valid exception
record/context reaches the inspector only as a VALIDATED COPY into the
inspector's address space (ClientPointers = FALSE) or as an explicitly
verified target-process representation (ClientPointers = TRUE) — raw
process-local pointers stored in shared memory are NOT cross-process
data and must be rejected at capsule validation; origin identification
rides the versioned pointer-free capsule (process-instance identity =
creation-time + verified image/build, not PID alone) matching the
cross-process artifact format boundary (amended foundation.md §10).

## 9. Platform capability + coverage matrices (declared, not claimed)

Windows-first fault-class coverage follows doc 01 §8 verbatim as the
declared minimum-evidence matrix (UCRT invalid parameter/original H1 →
grade B; terminate → B; unhandled AV → A; abort/assert/purecall →
non-modal reason + measured B; supervisor TerminateProcess → E; direct
fail-fast / stack-cookie → E+D-if-proven; pre-main/loader → E+D-if-proven;
inspector absent / wedged writer / disk full → report survivors, no
invented success; power loss/OS crash → OS evidence only). First I2
required-passing classes in the healthy profile: UCRT + access violation.
Platform matrix (Windows x64 = first Release fault-proof target; Linux
and macOS admitted-uncovered pending their own F3 probes; no-exceptions/
no-RTTI contract build stays green) and the ABI/distribution matrix
(static source API delivered; shared F1; independent ABI F2; artifact
format I2 — all unlanded except the static source API) are DECLARED by
the F0 baseline (qiven-foundation docs/architecture/
cpp-diagnostics-f0-baseline.md §7) and are cross-referenced here, not
duplicated. Nothing in this census upgrades any declaration to a claim.

## 10. Typed-path census duty (PR6 doc 01 §2.1 riding ruling, ADR-0059 Decision 4)

Path-bearing surfaces this program will introduce, each declared a
consumer of Foundation's typed path primitive when it lands: crash-artifact
directories (`.qiven/diagnostics/crashes/<crash-id>/` per doc 01 §7.4),
inspector registration destination roots, normal-log file identity
(the rotating sink's path family, replacing today's unrotated `--log`
target), and retention/cleanup paths (rotation generations, crash-storm
cleanup). Standing rule: every NEW raw path-string comparison site this
program introduces is recorded HERE and swept onto the typed primitive at
its landing. **Site count at I0: 0** — no diagnostics code exists yet at
either baseline; the existing `--log`/journal paths predate the program
and are not charged to it.

## 11. Exit-row self-assessment (program doc 00 §3 Exit, row by row)

| Exit row | Verdict | Reason |
| --- | --- | --- |
| Every governed executable and handler site classified | MET | 4 production executables + 52+1 test targets classified (section 2); unknown process-global handler sites = 0 in first-party sources, with the vendored-sources audit gap explicitly listed (NOT-VERIFIED, section 2.5) |
| Both CRT regressions + original Release/stderr-redirection H1 case have reproducible old-fail references | PARTIALLY MET | Class A: the standing aliased-stdio old-fail/new-pass discriminator exists (host-server-lifecycle "0." row + 3143-vs-0xC0000409 failure text) and h1-kit classifies 3143; Class B: install-time unit coverage exists (Foundation tests/crt_failure.cpp) but no automated leg drives the modal shape (a modal cannot be headlessly asserted); original incident audits are preserved but were outside this brief's read set (facts cited via ADR-0059) — deferred: the I2 probe suite adds the explicit Debug/Release old-fail legs per doc 01 §10.3 |
| Unknown process-global handler sites are zero | MET (with recorded gap) | zero in tracked first-party code; the vendored third-party singleton needs the I1 static gate to close the linked-image claim |
| Ownership, host-vs-module roles, linkage/ABI need, per-OS capability matrix, provider decision, budgets, coverage matrix recorded | PARTIALLY MET | ownership + matrices + budgets: recorded (sections 5, 7, 9; F0 baseline); provider decision: PROVISIONAL ONLY by law (ADR-0059 Decision 6 — the measured comparison (section 6) is the decision vehicle; deferred trigger: the I1-entry batch) |
| Any code-derived material has provenance/license disposition | MET (study-side) | section 4 license/NOTICE rows; no code-derived expression exists at I0; study-only per the brief |
| A provider has dependency justification and revisit condition | PARTIALLY MET (provisional) | justification criteria + revisit triggers recorded per candidate (sections 4-5); final justification deferred to the measurement batch — this is the predeclared state the program requires at I0, not a shortfall against it |
| Selected first slices have bounded fault/overload coverage and discriminating tests; unimplemented classes visibly uncovered; mechanism changeable without contract change | MET (as declarations) | sections 5-8 predeclare the slices, tests and visible-uncovered classes; no coverage is claimed to exist |

## 12. Provenance

- Pins: section 4.1 (all resolved live 2026-09-29; Crashpad commit date
  approximate — googlesource commit page 503'd; the getsentry/crashpad
  mirror was used for status.md/LICENSE after verifying it exposes the
  same HEAD commit, and this use is declared).
- Repositories read at exact SHAs: qiven-runtime `339d27d` (clean tree,
  main); qiven-foundation `032a6152efa18407d7b8d29d3203a5def179385a`
  (clean tree, main; F0 landed); qiven-docs accepted/2026-09-29 documents
  (migrated at docs main `2dc2f0f` per the workspace lock); qiven-context
  ADR-0059 only; qiven-workspace workspace.lock.json (generation
  sha256:861ea68…; recorded nodes cited in the status header).
- Tools: git status/rev-parse/log/ls-files/grep (read-only except the
  final branch/commit); Read/Glob/Grep file tools; WebFetch to
  github.com, raw.githubusercontent.com, chromium.googlesource.com,
  learn.microsoft.com only.
- NOT-VERIFIED (with reasons): vendored sqlite3 handler-surface line
  audit (repository outside the admissible read set — section 2.5 gap);
  MSVC runtime linkage of built images (no build ran — declared from
  CMake defaults, section 2.2); Release PDB absence on disk (declared
  from build configuration, no build ran — section 2.3); Crashpad commit
  timestamp (host 503 — approximate date recorded); third-party
  security/CVE history beyond the pinned status doc (NOT-MEASURED;
  required input to the future provider decision); Quill/spdlog/Crashpad
  source-line-level audit beyond the files named in section 4 (the
  measured comparison and the I1 provider decision deepen it).
- NOT-MEASURED (predeclared plans): every quantity in sections 6-7
  (latencies, waits, memory, CPU, startup, deployment bytes, dump
  duration/size, ring/snapshot cost) — the comparison batch is the first
  I1-entry act; no number in this document is a measurement.
