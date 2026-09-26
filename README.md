# qiven-runtime

Home of the Qiven Cognitive Control Runtime: the trusted component system
that converts observable participant proposals into policy-derived,
evidence-backed, exact-action decisions at the boundary where Judgment
becomes world-changing Mechanism (ADR-0038).

## Current status

The RCA proof program (RCA-0..RCA-16) is complete; the **Runtime
Production MVP program** (ADR-0047; MVP-0..MVP-7) is executing:
MVP-0..MVP-3 passed their exit gates; MVP-4 (production ZCode hook
adapter) is implemented with rows 2-5 locally proven and its interim exit
gate amended to the **simulated ZCode hook lifecycle gate** (ADR-0055,
2026-09-26: `MVP4_SIMULATION_ACCEPTED` with the standing
`INSTALLED_DESKTOP_EXECUTION_UNVERIFIED` residual; the owner-live trial
instrument is retired after four evidenced trials; real-harness
build/execution on the owner machine is banned); MVP-5 stays frozen until
CA-2 and the simulated-gate MVP-4 exit.

**Machine-local mutation authority is RuntimeHost-internal** (ADR-0043):
the ADR-0026 execution-authority invariants — single-writer lease,
monotonic fencing, fail-closed quarantine, reconciliation, bounded
journal — are absorbed as RuntimeHost subsystem semantics behind
`IExecutionAuthorityPort`. The former separate `qiven-host` broker is
sealed and its repository deleted with the history forfeited
(`DELETED_REMOTE + ARCHIVE_FORFEITED`, 2026-09-26); a direct mutating
path around RuntimeHost remains a safety defect.

## Entry points

- Production architecture (accepted, ADR-0047):
  `docs/architecture/runtime-production-mvp-architecture.md`; TCA
  amendment: `docs/architecture/runtime-mvp-roadmap-amendment.md`
  (ADR-0050, with the 2026-09-26 ADR-0055 amendment note).
- Component model: `docs/architecture/runtime-component-adl.md` (with
  dated Host/DCR reconciliation amendments; frozen v4 semantics of
  `qiven-context-draft` @ `4cbc995` are the immutable semantic
  dependency).
- Engineering conventions AND standards: canonical in the Devkit
  (`JasonHuang3D/qiven-devkit`: `docs/conventions/README.md` and
  `docs/engineering/README.md` there, ADR-0046 — this repository carries
  no local `docs/engineering/` copy; earlier pointer wording to one is
  superseded).
- Historical: `docs/architecture/legacy/` (including the superseded
  process-execution architecture, `runtime-process-execution.md`).

## Build and CI claim scope

Configure is workspace-resolved (the control-repository lock supplies the
adapter resolution file; the repository gate `tools\qiven.cmd gate` wraps
the path). CI is **explicit-dispatch** and its Windows unit checks out a
**workspace-locked snapshot** (see `.github/workflows/ci.yml`'s pinned
revision), not necessarily this repository's current head; non-Windows
legs are typed skips on that path. A green CI run validates the pinned
snapshot's configuration — it is not by itself a validation claim for an
arbitrary current candidate head. Local validation evidence for a
candidate is the repository gate at the exact candidate head.
