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

The REAL adapter bridge (`adapter/bridge.hpp` + the
`qiven-adapter-bridge` CLI) passes the same contract shape in tests
(`tests/adapter_bridge.cpp`, 22/22) with real semantics: verbatim
hook-payload digest binding, atomic evidence-grade state file,
single-observation across one-shot process instances.

## What the real-adapter execution adds (the H1 part)

Real harness traffic flowing through the bridge in a live session: the
hooks fire on actual tool calls, the bridge records real
proposals/observations, and the evidence dump reflects genuine harness
behavior rather than test fixtures.

## Owner steps (the bridge EXISTS — the H1 is executable)

Three steps, each one block:

**Step 1 — build the bridge (any session, normal work; already done in
the landing batch — verify the exe exists):**

```cmd
cd D:\JasonWork\qiven-runtime
tools\qiven.cmd run build-release
```

The executable is `build\vs2022-x64\Release\qiven-adapter-bridge.exe`.

**Step 2 — owner attaches the hooks (owner hands; workspace client
config).** Add to `D:\JasonWork\.zcode\config.json`, INSIDE the existing
`hooks.events` object (keeping the exec-router PreToolUse hook as
another entry in the PreToolUse list):

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

(The fixed `--action 1` records one correlated proposal/observation
pair for the session; per-action correlation ids ride the hook payload
when the hook input carries a stable event id.)

**Step 2.5 — start a NEW session.** Workspace hooks do NOT hot-reload:
they load at session start. After the config change, open a fresh
session (or restart this one) and make a few Bash tool calls in it (the
bridge is advisory — nothing is blocked). Until a hook fires at least
once, the state file does not exist.

**Step 3 — owner relays the sealed evidence (verbatim):**

```cmd
D:\JasonWork\qiven-runtime\build\vs2022-x64\Release\qiven-adapter-bridge.exe evidence --state D:\JasonWork\qiven-runtime\.generated-temp\adapter-bridge\real-session.log
```

The command ALWAYS speaks (owner direction 2026-09-22: silence is a
defect):

- state missing → stderr names the path, stdout explains the
  new-session requirement, exit 1;
- state present → a `bridge-state <path>` header plus the recorded
  events, exit 0.

Paste the output back UNMODIFIED. The authoring session grades it
against the F-suite rubric (complete surface, single observation,
tri-state from exit facts, declared window) and records PASS/FAIL.

## Honesty boundary

Loopback and bridge-test conformance prove the CONTRACT and the BRIDGE,
not real harness behavior. No hard-enforcement claim about the real
harness may cite them as evidence; the claim cites the real-adapter H1
run only (§11 handshake honesty, §67 claim scope).
