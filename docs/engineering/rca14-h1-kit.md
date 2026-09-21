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

## Owner steps (the bridge EXISTS — the H1 is executable)

The real adapter bridge landed (adapter/bridge.hpp + the
qiven-adapter-bridge CLI, atomic file-persisted ledger). Three steps,
each one block:

**Step 1 — build the bridge (any session, normal work):**

```cmd
cd D:\JasonWork\qiven-runtime
tools\qiven.cmd run build-release
```

The executable is `builds2022-x64\Release\qiven-adapter-bridge.exe`.

**Step 2 — owner attaches the hooks (owner hands; workspace client
config):** add to `D:\JasonWork\.zcode\config.json`, INSIDE the
existing `hooks.events` object (keeping the exec-router PreToolUse hook
as another list entry):

```json
"SessionStart": [
  { "hooks": [ { "type": "command",
      "command": "D:/JasonWork/qiven-runtime/build/vs2022-x64/Release/qiven-adapter-bridge.exe activate --kind session-start --state D:/JasonWork/qiven-runtime/.generated-temp/adapter-bridge/real-session.log" } ] }
],
"PreToolUse": [
  { "matcher": "Bash", "hooks": [ { "type": "command",
      "command": "D:/JasonWork/qiven-runtime/build/vs2022-x64/Release/qiven-adapter-bridge.exe intercept --tool Bash --session 1 --action 1 --state D:/JasonWork/qiven-runtime/.generated-temp/adapter-bridge/real-session.log" } ] }
],
"PostToolUse": [
  { "matcher": "Bash", "hooks": [ { "type": "command",
      "command": "D:/JasonWork/qiven-runtime/build/vs2022-x64/Release/qiven-adapter-bridge.exe observe --action 1 --exit 0 --state D:/JasonWork/qiven-runtime/.generated-temp/adapter-bridge/real-session.log" } ] }
]
```

(The fixed `--action 1` records one correlated proposal/observation pair
for the session; per-action correlation ids ride the hook payload when
the hook input carries a stable event id.)

**Step 3 — owner relays the sealed evidence (verbatim):**

```cmd
D:\JasonWork\qiven-runtimeuilds2022-x64\Release\qiven-adapter-bridge.exe evidence --state D:\JasonWork\qiven-runtime\.generated-tempdapter-bridgeeal-session.log
```

Paste the output back UNMODIFIED. The authoring session grades it
against the F-suite rubric (complete surface, single observation,
tri-state from exit facts, declared window) and records PASS/FAIL.

## Honesty boundary

Loopback conformance proves the CONTRACT, not the real harness. No
hard-enforcement claim about the real harness may cite the loopback run
as evidence; the claim cites the real-adapter run only (§11 handshake
honesty, §67 claim scope).
