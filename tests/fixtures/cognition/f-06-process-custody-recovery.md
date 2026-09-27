# Fixture F-06 — Supervising an external build worker

Class: process custody and recovery (protocol §4.2 F-06).
Sealed: 2026-09-24 (CA-0). Risk: R2/R3. Phase: design.

## Task descriptor (verbatim consumer prompt)

> Our tool spawns a compiler child process, streams its output, and must
> never leave orphans behind — even if our own process dies, the user kills
> the console, or the child hangs. Design the supervision and the recovery
> story on restart. Keep it simple where simplicity is honest.

Condition-blind envelope facts: repositories `qiven-devkit` (Python tools)
or `qiven-runtime` (C++), languages python/cpp, platform win32, phase
design, risk R2→R3, boundary kinds: process-custody, platform, concurrency.

## Hidden failure surface

"Keep it simple" hides: PID reuse, handle lifetime, who owns termination
authority, what a stale run record means after a crash (business exit code
unknown and never guessed), node-reuse leaks (msbuild class), console-window
discipline, and blind replay on recovery re-executing an external effect.

## Protected cognition expected

- MUST-INCLUDE:
  - ADR-0048 (bounded process custody LAW: watchdog + Job Object
    KILL_ON_JOB_CLOSE + lease; completion reap; sweep insurance)
  - MEM-20260923T233000Z-E7F8A1 (custody is mechanical, never behavioral)
  - MEM-20260923T212000Z-D4E5F6 (CREATE_NO_WINDOW vs DETACHED_PROCESS;
    0xC0000142 class)
- MUST-EXPLAIN:
  - Operating contract §hang classification (apparent hang vs healthy long
    work; heartbeat as discriminator; bounded invocations)
  - Evidence: evidence/audits/2026-09-23-exec-process-leak-incident.md
    (on-demand)
- MAY-RANK:
  - Devkit python-standard (custody requirements for Python tools);
    operator-usage.md exec semantics; ADR-0046 layering

## Semantic-owner expectations

Custody mechanism is kernel-enforced (Job Object/lease), owned by the tool
layer (operator/RuntimeHost subsystem); the action/decision boundary is not
confused with raw process utility; durable control facts vs transient
handles are separated; recovery classifies (succeeded / not performed /
stale / conflicting / indeterminate) without replaying effects.

## Critical findings

1. Process ownership, PID-reuse handling, handle lifetime, termination
   authority, and crash recovery are explicit.
2. The tree dies no later than its lease even if the supervisor dies
   (kernel-enforced, not discipline-enforced).
3. Durable control facts and transient handles are separated.
4. Recovery does not blindly replay an external effect; unobserved exits are
   `indeterminate`.
5. Independent fault injection covers partial start, parent crash, stale
   record, and shutdown.

## Required independent evidence

Fault injection across the actual state-machine boundary (kill supervisor,
orphan the child, expire the lease, restart with a stale record); fresh
review of the custody ownership table.

## Prohibited shortcuts

Relying on the child exiting politely; discipline-only custody ("we always
clean up"); guessing exit codes from elapsed time.

## Rubric application

§7.4 weights; focus: failure/recovery and safe-state design (10, floor 8),
external/platform/concurrency assumptions (15, floor 12).
