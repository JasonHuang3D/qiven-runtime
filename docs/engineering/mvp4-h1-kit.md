# MVP-4 Real-ZCode Deny-No-Fallthrough — Owner H1 Kit

The MVP-4 exit gate row 1 (ARCH §15) is H1-designated: real-harness
enforcement crosses an isolation boundary — the owner's hands install
the hooks in the live workspace client and relay the sealed evidence
back. This kit makes the owner's part paste-ready
(human-handoff-boundary H1 duty). Everything here follows the RCA-14
kit pattern (`docs/engineering/rca14-h1-kit.md`).

## What is already proven without H1

`tests/hook_conformance.cpp` (gate:local) proves locally: the verdict
matrix (governed write deny 110, Bash conservative deny 111, unknown
tool deny 112, unregistered session deny 114, duplicate/unmatched post
115, drain deny), host-assigned unique action ids and idempotent
session identity, post-action correlation, fail-closed client mapping
when the host is unreachable (deny 116 claiming NO governed status),
profile tool-inventory parsing with the complete-mediation data check,
the H-4 shutdown ordering, and protocol decode fail-closure.

NOT proven locally: that the REAL ZCode harness blocks the tool call
when the hook denies (no fallthrough), in a live owner session.

## Owner steps

**Step 0 — the session builds everything (already part of the batch;
verify the exes exist):**

```cmd
cd D:\JasonWork\qiven-runtime
tools\qiven.cmd run build-release
```

Executables: `build\vs2022-x64\Release\qiven-runtime-host.exe`,
`qiven-zcode-hook.exe`, `qiven-runtimectl.exe`.

**Step 1 — owner starts the host against the governed checkout (a
dedicated console window):**

```cmd
cd D:\JasonWork\qiven-runtime
build\vs2022-x64\Release\qiven-runtime-host.exe --root D:\JasonWork\qiven-context --profile config\profiles\zcode-jason-context-record-mvp.yaml
```

Leave it running (it holds the singleton mutex and the pipe).

**Step 2 — owner attaches the hooks** (owner hands; workspace client
config `D:\JasonWork\.zcode\config.json`, INSIDE the existing
`hooks.events` object — keep the exec-router PreToolUse entry as
another list item; the qiven hook must run FIRST, so place it before
the router entry and KEEP the router as the second entry):

```json
"SessionStart": [
  { "hooks": [ { "type": "command",
      "command": "D:/JasonWork/qiven-runtime/build/vs2022-x64/Release/qiven-zcode-hook.exe --event session_start --root D:/JasonWork/qiven-context" } ] }
],
"PreToolUse": [
  { "matcher": "Bash|Write|Edit", "hooks": [ { "type": "command",
      "command": "D:/JasonWork/qiven-runtime/build/vs2022-x64/Release/qiven-zcode-hook.exe --event pre_tool --root D:/JasonWork/qiven-context" } ] },
  { "matcher": "Bash", "hooks": [ { "type": "command",
      "command": "python \"D:/JasonWork/qiven-devkit/tools/hook_exec_router.py\"", "enabled": true } ] }
],
"PostToolUse": [
  { "matcher": "Bash|Write|Edit", "hooks": [ { "type": "command",
      "command": "D:/JasonWork/qiven-runtime/build/vs2022-x64/Release/qiven-zcode-hook.exe --event post_tool --root D:/JasonWork/qiven-context" } ] }
]
```

**Step 3 — start a NEW session** (workspace hooks load at session
start; they do not hot-reload), then in that session make exactly these
tool calls and record what happens (the session text itself is the
evidence; copy it verbatim):

1. a Write to `D:\JasonWork\qiven-context\state\probe-h1.md` → expect
   DENY (110/118-class; the write must NOT land);
2. an Edit attempting the same file → expect DENY;
3. a Bash `echo probe > D:/JasonWork/qiven-context/state/probe-h1.md`
   → expect DENY (111; the file must NOT exist afterward);
4. a Bash `echo outside > D:/JasonWork/qiven-runtime/.generated-temp/probe-h1.txt`
   → expect ALLOW (not_governed);
5. any tool call of an exotic kind (e.g. WebSearch) → for
   Bash/Write/Edit-shaped deny semantics see the host journal;
6. stop the host console (Ctrl+C or
   `qiven-runtimectl.exe host shutdown --root D:\JasonWork\qiven-context`),
   then repeat call 3 → expect DENY 116 with "cannot classify" (the
   honesty row: nothing reported governed).

**Step 4 — owner relays the sealed evidence (verbatim):**

```cmd
cd D:\JasonWork\qiven-context
type .qiven\runtime\journal.sqlite3 2>nul & echo --- & dir /b .qiven\runtime\bundles
```

and paste the new session's tool-call outcomes (calls 1-6 above with
their observed deny/allow text) back UNMODIFIED. The authoring session
grades them against the exit gate: deny has no fallthrough (rows 1-3),
no bypass via Edit/Bash (rows 2-3), unknown/disconnected honesty
(rows 5-6), unique ids across repeats.

## Honesty boundary

Local tests prove the client, host, and mapping contracts. Only this
H1 run may be cited as evidence that the REAL harness enforces the
deny. An H1 PASS closes MVP-4 exit gate row 1; the kit's observed
latency data feeds the J-1 lazy-start revisit decision.
