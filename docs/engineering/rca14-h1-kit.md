# RCA-14 Real-Harness Adapter Conformance — Owner H1 Kit

RCA-14 is H1-designated (owner direction 2026-09-21): real-harness
adapter conformance crosses an isolation boundary — the owner's hands
are required to launch the fresh real-adapter session, install the
adapter surface it drives, and relay the sealed evidence back. This kit
makes the owner's part paste-ready (human-handoff-boundary H1 duty).

## What has already been proven without H1

The adapter CONTRACT is proven in-process by the conformance suite
(`tests/adapter_conformance.cpp`) against the LoopbackAdapter:

- F1 manifest completeness/consistency
- F2 §14 incomplete-surface rejection (six fields, individually)
- F3 §14 full-surface materialization (identities + argument digest)
- F4 §14 observation immutability across re-submission
- F5 §46 single-correlated-observation (duplicate = integrity conflict)
- F6 §15/C-14 no-ack → Indeterminate, never Failed
- F7 §16/§67 tool-mediated governed; free-response NotGoverned
- F8 §13 activation tracked, correctness-neutral
- F9 §42 residual check-to-execution window declared

## What the real-adapter execution adds (the H1 part)

The SAME suite driven by a REAL harness adapter fixture. Concretely:

1. A real adapter implementation (harness-side bridge exposing the four
   channels: activation, interception, post-action observation, claims)
   is built and registered against the manifest handshake (RCA-1).
2. The owner launches a FRESH session with the real adapter attached —
   the runtime in that session must not be contaminated by this
   authoring session (isolation boundary).
3. The conformance suite runs there and produces a SEALED result:
   repository ref, suite version, PASS/FAIL per check, adapter identity,
   declared residual window.
4. The owner relays the sealed output back VERBATIM (no paraphrase);
   the authoring session grades it against the same rubric.

## Owner steps (when you choose to execute the H1)

The real-adapter bridge does not exist yet — building it is normal
(non-H1) work that happens FIRST. When it exists, this section becomes
one copyable block per step: the launch command/prompt for the fresh
session, the exact suite invocation, and the relay format. Until then,
**there is nothing for the owner to execute**: the H1 is armed, not
overdue.

## Honesty boundary

Loopback conformance proves the CONTRACT, not the real harness. No
hard-enforcement claim about the real harness may cite the loopback run
as evidence; the claim cites the real-adapter run only (§11 handshake
honesty, §67 claim scope).
