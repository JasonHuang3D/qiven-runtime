# qiven-runtime bundle {{VERSION}}

Deployed {{DATE}} from exact head `{{HEAD}}` (gate receipt recorded in
`manifest.json`). Bundle layout: `bin/` executables, `lib/`+`include/`
the runtime static library and its public headers, `docs/`, `licenses/`,
`manifest.json` (per-file SHA-256; verify with the Devkit deploy tool's
`--verify`).

## What this is (and is not)

The Qiven Cognitive Control Runtime control-plane library and its
current applications, at MVP-0 maturity (production contracts closed;
see ARCHITECTURE.md / CPP-DESIGN.md in `docs/`). This bundle is NOT the
RuntimeHost product yet: `qiven-runtime-app.exe` is the bootstrap
self-check binary; `qiven-adapter-bridge.exe` is the adapter protocol
probe/conformance fixture, not a production hook.

## How to run

- `bin\qiven-runtime-app.exe` — prints the runtime bootstrap banner and
  exits 0 (self-check).
- `bin\qiven-adapter-bridge.exe` — adapter protocol probe; requires the
  test harness input on stdin; not for standalone production use.

No configuration, no environment variables, no installation: run from
where the bundle stands. Nothing here writes outside its own working
directory.

## Safety boundaries

This bundle does not push, does not create/merge pull requests, does
not write outside the workspace, and contains no daemon/service.

## Verification

`python <qiven-devkit>/tools/deploy_bundle.py --verify <this-directory>`
recomputes every file digest against `manifest.json`.

## Provenance

Head `{{HEAD}}`, gate `local` PASS at that head, licenses under
`licenses/` (SQLite Public Domain notice included via third_party
provenance).
