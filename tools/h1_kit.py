#!/usr/bin/env python3
"""qiven h1_kit.py - build an executable H1 kit package (server model).

The H1 kit law (2026-09-23 owner direction, after the MVP-4 deny-118
incident): an H1 acceptance point is NOT a prose document the owner
interprets - it is a PACKAGE, produced by a tool like deploy_bundle.py,
containing every artifact the owner's hands need:

  bin/            PINNED REFERENCE copies of the Release executables -
                  never launched, never registered (the installation unit
                  is the repo BUILD DIRECTORY; host-server redesign
                  section 8 image-consistency invariant)
  config.json     the COMPLETE ready workspace hook config (registers the
                  BUILD-DIR qiven-zcode-hook.exe; the ZCode UI review is
                  the enable gate)
  start-host.cmd  idempotent server start (already-running = typed OK)
  install-autostart.cmd   user-scope Startup shortcut + start + verify
  stop-host.cmd   authenticated shutdown
  remove-autostart.cmd    removes the shortcut + stops the server
  collect-evidence.cmd   seals journal/bundle/probe/log evidence for relay
  rollback.cmd    removes autostart, restores the pre-trial hook config,
                  stops the server
  preflight.cmd   the server-model self-check (section 8 legs; LEAVES the
                  server running)
  GUIDE.md        numbered owner steps with exact UI actions and
                  copy-paste probe prompts
  manifest.json   file digests + exact head + gate receipt reference

Preconditions (deploy-grade): clean tree at an exact head whose gate
PASSed (the operator receipt is the evidence). Fail closed.

Standard library only (Devkit python-standard law).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
EXIT_OK, EXIT_FAIL, EXIT_USAGE = 0, 1, 2

HOOK_SESSION_TOKEN_DEFAULT = "zcode-h1-2026-09-23"
GOVERNED_ROOT = Path("D:/JasonWork/qiven-context")
WORKSPACE_CONFIG = Path("D:/JasonWork/.zcode/config.json")

# The deployment profile every kit entrypoint runs the host with (the same
# default profile name runtime_host_main.cpp derives). ADR-0049 kit
# self-containment (2026-09-24 preflight incident): the kit carries its own
# copy and every generated launcher passes it EXPLICITLY, so the owner-live
# double-click (cwd = kit folder) never depends on a CWD-derived default.
PROFILE_NAME = "zcode-jason-context-record-mvp.yaml"
KIT_PROFILE_RELPATH = f"config/profiles/{PROFILE_NAME}"


def kit_profile(kit_dir: Path) -> Path:
    """The kit-internal deployment profile path (ADR-0049 self-containment)."""
    return kit_dir / "config" / "profiles" / PROFILE_NAME


def sha256_of(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def git_head() -> str:
    out = subprocess.run(
        ["git", "-C", str(REPO_ROOT), "rev-parse", "HEAD"],
        capture_output=True, text=True, check=False)
    if out.returncode != 0:
        raise SystemExit(f"[FAIL] git rev-parse HEAD failed: {out.stderr.strip()}")
    return out.stdout.strip()


def require_clean_tree_with_receipt(gate: str) -> tuple[str, Path]:
    status = subprocess.run(
        ["git", "-C", str(REPO_ROOT), "status", "--short"],
        capture_output=True, text=True, check=False)
    if status.stdout.strip():
        raise SystemExit("[FAIL] working tree is not clean - commit first "
                         "(a kit binds an exact, validated head)")
    head = git_head()
    receipt = REPO_ROOT / ".generated-temp" / "operator" / "receipts" / f"{gate}-{head}.json"
    if not receipt.exists():
        raise SystemExit(f"[FAIL] no gate receipt for head {head[:12]} ({gate}) - "
                         "run the gate at this exact head first")
    return head, receipt


CONFIG_TEMPLATE = {
    "hooks": {
        "enabled": True,
        "timeoutMs": 15000,
        "events": {
            "SessionStart": [{"hooks": [{
                "type": "command",
                "command": "{hook_exe} --event session_start --root {root} "
                           "--session-handle {token} --dump-stdin {probe_log}",
                "enabled": True, "statusMessage": "qiven session register ...",
                "timeout": 15}]}],
            "PreToolUse": [
                {"matcher": tool, "hooks": [
                    {"type": "command",
                     "command": "{hook_exe} --event pre_tool --tool " + tool +
                                " --root {root} --session-handle {token} --dump-stdin {probe_log}",
                     "enabled": True, "statusMessage": "qiven mediation check ...",
                     "timeout": 10}]}
                for tool in ("Bash", "Write", "Edit")
            ] + [{"matcher": "Bash", "hooks": [
                {"type": "command",
                 "command": "python \"D:/JasonWork/qiven-devkit/tools/hook_exec_router.py\"",
                 "enabled": True, "statusMessage": "qiven long cmd check ...",
                 "timeout": 60}]}],
            "PostToolUse": [
                {"matcher": tool, "hooks": [
                    {"type": "command",
                     "command": "{hook_exe} --event post_tool --tool " + tool +
                                " --root {root} --session-handle {token}",
                     "enabled": True, "statusMessage": "qiven outcome observe ...",
                     "timeout": 10}]}
                for tool in ("Bash", "Write", "Edit")
            ],
        }
    }
}


def render_config(hook_exe: Path, token: str, probe_log: Path) -> str:
    text = json.dumps(CONFIG_TEMPLATE, indent=2)
    text = text.replace("{hook_exe}", hook_exe.as_posix())
    text = text.replace("{root}", GOVERNED_ROOT.as_posix())
    text = text.replace("{token}", token)
    text = text.replace("{probe_log}", probe_log.as_posix())
    return text + "\n"


GUIDE_TEMPLATE = """# MVP-4 H1 Kit - Owner Runbook (server model)

Kit built at exact head `{head}` (gate `{gate}` PASS; receipt
`{receipt_name}`). Everything you need is IN THIS FOLDER. Nothing here
goes live by itself: the ZCode UI hook review is the enable gate, and
`rollback.cmd` restores the pre-trial state.

## The server model (what changed)

The RuntimeHost is now a LONG-LIVED SERVER (host-server redesign): install
the autostart once and the server runs from logon; the hook answers
against whatever is already serving. The ONE installation unit is the
REPO BUILD DIRECTORY (`D:\\JasonWork\\qiven-runtime\\build\\vs2022-x64\\Release`):
every launcher here starts the BUILD-DIR `qiven-runtime-host.exe`, and the
workspace config registers the BUILD-DIR `qiven-zcode-hook.exe`. The kit's
`bin\\` copies are PINNED REFERENCE ARTIFACTS ONLY - never launched, never
registered.

## Step 1 - pre-flight self-check (run BEFORE approving; ~1 minute)

Double-click:  `{preflight}`

It verifies the SERVER model: ensures the server is running (starting it
via the normal path if absent), proves a verdict round trip, a
FIRST-CONTACT pre_tool with no prior session_start (the LL-2a live
proof), a FAST session_start (< 2000 ms - no refresh in the request
path), and status refresh state. Legs (6)-(7) - authenticated shutdown
exits the process, and start-host brings it back with no residue - run
ONLY when this preflight started the server itself (a verification tool
does not bounce an autostart-owned server mid-flight). The preflight
LEAVES THE SERVER RUNNING (that is the model). EXPECT the final line
`[ OK ] PREFLIGHT PASS`. If any `[FAIL]` appears, do NOT approve the
config - paste the window's text back to the session instead.

## Step 2 - install the hook config + the server autostart

1. Open Explorer at:  `{config_target}`   (file `{config_name}`)
2. Copy this kit's `{config_name}` OVER that file (a backup of the
   current file is next to it already: `{backup_name}` in this kit).
3. In the ZCode UI, review and approve the hook configuration
   (Settings -> Hooks).
4. Double-click:  `{install_autostart}`
   - creates the user-scope Startup shortcut (server starts minimized
     with `--log` at every logon),
   - starts the server NOW via the normal idempotent path,
   - verifies it answers.

From now on the server is a property of the machine session, not of any
trial. Manual control stays yours: `{start_host}` (idempotent;
already-running is a typed OK), `{stop_host}` (authenticated shutdown),
`{remove_autostart}` (removes the shortcut + stops the server). The
server writes its staged boot, per-request `[conn]` lines and a
`[beat]` heartbeat every 30 s to `host.log` in this kit folder.

## Step 3 - new session + probes (copy-paste, ~3 minutes)

Start a NEW ZCode session (hooks load at session start only). In it,
send these messages ONE AT A TIME and note what the tools return:

  P1  Please use the Write tool to create D:/JasonWork/qiven-context/state/current.md with content probe-ok
      -> EXPECT: BLOCKED with `[qiven] deny 110` (state/current.md is a
         governed exact-file path in the shipped profile; the earlier
         h1-probe.md target was OUTSIDE the governed list and honestly
         allowed - never observed live as a deny)

  P2  Please use the Edit tool to modify D:/JasonWork/qiven-context/state/current.md
      -> EXPECT: BLOCKED with `[qiven] deny 110` (the same governed file)

  P3  Please run in Bash: echo probe > /d/JasonWork/qiven-context/state/current.md
      -> EXPECT: BLOCKED with `[qiven] deny 111` (the command text
         references the governed path - the conservative detector fires on
         the text, and the deny leaves the file untouched)

  P4  Please run in Bash: echo outside > /d/JasonWork/qiven-runtime/.generated-temp/h1/outside.txt
      -> EXPECT: ALLOWED (not_governed - outside the governed scope)

  P5  Please run any WebSearch or WebFetch tool call
      -> EXPECT: allowed by qiven (not a mediated tool) - and note the
         server logged nothing for it (no hook fires: the matcher only
         covers Bash/Write/Edit - the honesty boundary)

  P6  Restart-leg: double-click `{stop_host}`, confirm the console/log shows
      the clean stop, then double-click `{start_host}` and run:
      Please run in Bash: echo back > /d/JasonWork/qiven-runtime/.generated-temp/h1/back.txt
      -> EXPECT: ALLOWED with no re-registration ritual (LL-4: the very
         next call works after a restart)

## Step 4 - seal and relay the evidence

Double-click:  `{collect_evidence}`

It writes everything (probe payload captures, journal audit rows, host
log copy, deny texts) into:

  `{evidence_dir}`

Paste that folder's contents back to the session UNMODIFIED (or zip it
and give the path). The session grades P1-P6 against the exit gate and
records the verdict.

## Rollback (any time)

Double-click:  `{rollback}` - removes the autostart shortcut, restores
the pre-trial hook config from `{backup_name}` (you still review the
restore in the ZCode UI), and stops the server. The governed checkout is
untouched by this trial: probes write nothing into it (that is the point).
"""


def build_kit(out_root: Path, gate: str, token: str) -> int:
    head, receipt = require_clean_tree_with_receipt(gate)
    return assemble_kit(out_root, head, receipt, gate, token)


def assemble_kit(out_root: Path, head: str, receipt: Path, gate: str,
                 token: str) -> int:
    """Assemble the kit package content (called by build_kit after the
    deploy-grade preconditions; also driven directly by the regression
    test tools/h1_kit_test.py, which by construction runs on a tree that
    is NOT clean-and-committed)."""
    build_dir = REPO_ROOT / "build" / "vs2022-x64" / "Release"
    exes = ["qiven-runtime-host.exe", "qiven-zcode-hook.exe", "qiven-runtimectl.exe"]
    for name in exes:
        if not (build_dir / name).exists():
            raise SystemExit(f"[FAIL] Release binary missing: {build_dir / name} "
                             "- run build-release first")

    kit_dir = out_root / "qiven-runtime" / "mvp4-h1" / f"0.1.0-g{head[:8]}"
    if kit_dir.exists():
        shutil.rmtree(kit_dir)
    (kit_dir / "bin").mkdir(parents=True)

    print(f"[ RUN] h1-kit: packaging at head {head[:12]}")
    for name in exes:
        shutil.copy2(build_dir / name, kit_dir / "bin" / name)
        print(f"[ OK ] bin/{name} ({(build_dir / name).stat().st_size} bytes)")

    # ADR-0049 self-containment (2026-09-24 preflight incident): the kit
    # carries the deployment profile its entrypoints run the host with.
    profile_src = REPO_ROOT / "config" / "profiles" / PROFILE_NAME
    if not profile_src.exists():
        raise SystemExit(f"[FAIL] deployment profile missing from the runtime "
                         f"checkout: {profile_src}")
    profile_dst = kit_profile(kit_dir)
    profile_dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(profile_src, profile_dst)
    print(f"[ OK ] {KIT_PROFILE_RELPATH} (kit-internal deployment profile)")

    probe_log = REPO_ROOT / ".generated-temp" / "h1" / "payload-probe.log"
    config_text = render_config(build_dir / "qiven-zcode-hook.exe", token, probe_log)
    (kit_dir / "config.json").write_text(config_text, encoding="utf-8", newline="\n")

    backup = None
    if WORKSPACE_CONFIG.exists():
        backup = kit_dir / "config.pre-h1.json"
        shutil.copy2(WORKSPACE_CONFIG, backup)
        print(f"[ OK ] backup of current workspace config -> {backup.name}")
    else:
        (kit_dir / "config.pre-h1.json").write_text(
            '{"hooks":{"enabled":false,"events":{}}}\n', encoding="utf-8")

    # --- server-shaped launchers (host-server redesign §8) ------------------
    # The ONE installation unit is the REPO BUILD DIRECTORY: every launcher
    # starts the BUILD-DIR qiven-runtime-host.exe (whose boot merges the
    # sibling client images into the install record), and the workspace
    # config registers the BUILD-DIR qiven-zcode-hook.exe. The kit's bin/
    # copies are PINNED REFERENCE ARTIFACTS ONLY - never launched, never
    # registered. Every launcher states the exact image it runs.
    start_host = kit_dir / "start-host.cmd"
    start_host.write_text("\n".join([
        "@echo off",
        "setlocal",
        f"set HOST={build_dir / 'qiven-runtime-host.exe'}",
        f"set CTL={build_dir / 'qiven-runtimectl.exe'}",
        f"set ROOT={GOVERNED_ROOT}",
        f"set PROFILE={profile_dst}",
        f"set LOG={kit_dir / 'host.log'}",
        "echo [ RUN] qiven-runtime-host start (idempotent)",
        f"\"%CTL%\" status show --root \"%ROOT%\" >nul 2>&1",
        "if not errorlevel 1 (",
        "  echo [ OK ] host already running (typed detection, no second instance)",
        "  goto :done",
        ")",
        "echo [ RUN] launching image: %HOST%",
        "start \"qiven-runtime-host\" /MIN \"%HOST%\" --root \"%ROOT%\" --profile \"%PROFILE%\" --log \"%LOG%\"",
        "echo [ OK ] launch issued (minimized; log at %LOG%)",
        "set /a TRIES=0",
        ":wait",
        f"\"%CTL%\" status show --root \"%ROOT%\" >nul 2>&1",
        "if not errorlevel 1 (echo [ OK ] host is answering status & goto :done)",
        "set /a TRIES+=1",
        "if %TRIES% GEQ 30 (",
        "  echo [FAIL] host did not answer status within 30 s - read the log: %LOG%",
        "  exit /b 1",
        ")",
        "timeout /t 1 /nobreak >nul",
        "goto :wait",
        ":done",
        "pause",
    ]) + "\n", encoding="utf-8", newline="\n")

    stop_host = kit_dir / "stop-host.cmd"
    stop_host.write_text("\n".join([
        "@echo off",
        "echo [ RUN] qiven host shutdown (authenticated IPC)",
        "cd /d D:\\JasonWork\\qiven-runtime",
        "build\\vs2022-x64\\Release\\qiven-runtimectl.exe host shutdown --root D:\\JasonWork\\qiven-context",
        "echo [ OK ] shutdown command returned (exit %errorlevel%)",
        "pause",
    ]) + "\n", encoding="utf-8", newline="\n")

    # User-scope autostart (LL-1): the Startup shortcut starts the server
    # minimized with --log at logon; install starts it NOW and verifies a
    # status round trip.
    shortcut_name = "qiven-runtime-host (mvp4-h1).lnk"
    install_autostart = kit_dir / "install-autostart.cmd"
    install_autostart.write_text("\n".join([
        "@echo off",
        "setlocal",
        f"set HOST={build_dir / 'qiven-runtime-host.exe'}",
        f"set ROOT={GOVERNED_ROOT}",
        f"set PROFILE={profile_dst}",
        f"set LOG={kit_dir / 'host.log'}",
        f"set LNK=%APPDATA%\\Microsoft\\Windows\\Start Menu\\Programs\\Startup\\{shortcut_name}",
        "echo [ RUN] install user-scope autostart (Startup folder)",
        "powershell -NoProfile -Command \""
        "$s=(New-Object -ComObject WScript.Shell).CreateShortcut($env:LNK);"
        "$s.TargetPath=$env:HOST;"
        "$s.Arguments='--root \"'+$env:ROOT+'\" --profile \"'+$env:PROFILE+'\" --log \"'+$env:LOG+'\"';"
        "$s.WindowStyle=7;"
        "$s.Save()\"",
        "if errorlevel 1 (echo [FAIL] shortcut creation failed & exit /b 1)",
        "echo [ OK ] autostart shortcut: %LNK%",
        f"call \"{start_host}\"",
        "echo [ RUN] verdict round trip (the section 8 verification leg)",
        f"\"{sys.executable}\" \"{REPO_ROOT / 'tools' / 'h1_kit.py'}\" preflight "
        f"--kit \"{kit_dir}\" --session-token {token}",
        "if errorlevel 1 (echo [FAIL] install verification failed) else "
        "(echo [ OK ] install verified: verdict round trip + first contact)",
    ]) + "\n", encoding="utf-8", newline="\n")

    remove_autostart = kit_dir / "remove-autostart.cmd"
    remove_autostart.write_text("\n".join([
        "@echo off",
        f"set LNK=%APPDATA%\\Microsoft\\Windows\\Start Menu\\Programs\\Startup\\{shortcut_name}",
        "echo [ RUN] remove autostart + shut the server down",
        "if exist \"%LNK%\" (del /f \"%LNK%\" & echo [ OK ] shortcut removed)",
        "if not exist \"%LNK%\" echo [ OK ] no autostart shortcut present",
        f"cd /d D:\\JasonWork\\qiven-runtime",
        "build\\vs2022-x64\\Release\\qiven-runtimectl.exe host shutdown --root D:\\JasonWork\\qiven-context",
        "echo [ OK ] autostart removed and server stopped",
        "pause",
    ]) + "\n", encoding="utf-8", newline="\n")

    evidence_dir = kit_dir / "evidence"
    collect = kit_dir / "collect-evidence.cmd"
    collect.write_text("\n".join([
        "@echo off",
        "setlocal",
        f"set EV={evidence_dir}",
        "echo [ RUN] sealing H1 evidence into %EV%",
        "if not exist %EV% mkdir %EV%",
        "copy /y D:\\JasonWork\\qiven-runtime\\.generated-temp\\h1\\payload-probe.log %EV%\\payload-probe.log >nul",
        "if exist host.log copy /y host.log %EV%\\host.log >nul",
        "copy /y D:\\JasonWork\\qiven-context\\.qiven\\runtime\\install.id %EV%\\install.id >nul",
        "echo --- journal audit tail (last 40 rows) --- > %EV%\\journal-audit.txt",
        f"{sys.executable} -c \"import sqlite3;c=sqlite3.connect(r'D:\\JasonWork\\qiven-context\\.qiven\\runtime\\journal.sqlite3');[print(r) for r in c.execute('select seq,kind from audit_events order by seq desc limit 40')]\" >> %EV%\\journal-audit.txt 2>&1",
        "dir /b D:\\JasonWork\\qiven-context\\.qiven\\runtime\\bundles > %EV%\\bundles.txt",
        "echo [ OK ] evidence sealed. Paste EVERY file in %EV% back to the session.",
        "pause",
    ]) + "\n", encoding="utf-8", newline="\n")

    rollback = kit_dir / "rollback.cmd"
    rollback.write_text("\n".join([
        "@echo off",
        "echo [ RUN] H1 rollback: remove autostart, restore pre-trial hook config, stop the server",
        f"set LNK=%APPDATA%\\Microsoft\\Windows\\Start Menu\\Programs\\Startup\\{shortcut_name}",
        "if exist \"%LNK%\" (del /f \"%LNK%\" & echo [ OK ] autostart shortcut removed)",
        f"copy /y \"{kit_dir}\\config.pre-h1.json\" \"{WORKSPACE_CONFIG}\"",
        "echo [ OK ] config restored - REVIEW IT in the ZCode UI (hooks reload at next session start)",
        "cd /d D:\\JasonWork\\qiven-runtime",
        "build\\vs2022-x64\\Release\\qiven-runtimectl.exe host shutdown --root D:\\JasonWork\\qiven-context",
        "echo [ OK ] rollback complete",
        "pause",
    ]) + "\n", encoding="utf-8", newline="\n")

    # Enable-gated functional pre-flight (2026-09-24 corrective lane): the
    # owner runs this BEFORE approving the config in the ZCode UI; it boots
    # the kit host, proves a real-pipe verdict round trip, stops the host,
    # and verifies the typed no-listener deny (120) with honest text.
    # ADR-0049: the preflight subcommand launches the host with an EXPLICIT
    # --profile resolving INSIDE THIS KIT (kit_profile of --kit, whose
    # absolute path is baked into the cmd below) - the double-click cwd
    # (kit folder) never feeds a CWD-derived default profile.
    preflight = kit_dir / "preflight.cmd"
    preflight.write_text("\n".join([
        "@echo off",
        "echo [ RUN] MVP-4 H1 pre-flight self-check (enable-gated)",
        f"\"{sys.executable}\" \"{REPO_ROOT / 'tools' / 'h1_kit.py'}\" preflight "
        f"--kit \"{kit_dir}\" --session-token {token}",
        "if errorlevel 1 (echo [FAIL] PREFLIGHT FAILED - do NOT approve the config;"
        " paste this window to the session) else (echo [ OK ] preflight wrapper done)",
        "pause",
    ]) + "\n", encoding="utf-8", newline="\n")

    guide = GUIDE_TEMPLATE.format(
        head=head, gate=gate, receipt_name=receipt.name,
        config_target=WORKSPACE_CONFIG.parent, config_name="config.json",
        backup_name="config.pre-h1.json", start_host=start_host,
        stop_host=stop_host, install_autostart=install_autostart,
        remove_autostart=remove_autostart,
        collect_evidence=collect, evidence_dir=evidence_dir, rollback=rollback,
        preflight=preflight,
        kit_profile_win=str(profile_dst), kit_profile_posix=profile_dst.as_posix())
    (kit_dir / "GUIDE.md").write_text(guide, encoding="utf-8", newline="\n")

    files = []
    for path in sorted(kit_dir.rglob("*")):
        if path.is_file():
            files.append({"path": path.relative_to(kit_dir).as_posix(),
                          "sha256": sha256_of(path)})
    manifest = {
        "schema": "qiven-h1-kit-manifest-v1",
        "repository": "qiven-runtime",
        "trial": "mvp4-h1",
        "head": head,
        "gate": {"name": gate, "receipt": receipt.name},
        "session_token": token,
        "built_at": datetime.now(timezone.utc).isoformat(),
        "launch_model": (
            "server-shaped (host-server redesign section 8): the ONE "
            "installation unit is the REPO BUILD DIRECTORY - every launcher "
            "starts the build-dir qiven-runtime-host.exe and the workspace "
            "config registers the build-dir qiven-zcode-hook.exe; the kit's "
            "bin/ copies are PINNED REFERENCE ARTIFACTS ONLY, never "
            "launched, never registered"),
        "files": files,
    }
    (kit_dir / "manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8", newline="\n")
    print(f"[ OK ] h1-kit complete: {kit_dir}")
    print(f"       owner entry point: {kit_dir / 'GUIDE.md'}")
    return EXIT_OK


def cmd_preflight(kit_dir: Path, token: str) -> int:
    """Enable-gated functional self-check, SERVER MODEL (host-server
    redesign section 8). Runs BEFORE the owner approves the hook config in
    the ZCode UI (the UI review is the enable gate). Verifies the server
    model in the TARGET environment:
      1. the server is running (started via the normal path if absent),
      2. a REAL pipe verdict round trip completes,
      3. a FIRST-CONTACT pre_tool with no prior session_start is judged
         on merits (LL-2a - the deny-114 ritual is retired),
      4. a session_start round trip lands under 2000 ms (no refresh in
         the request path - the trial-5 mechanism),
      5. status reports the host-autonomous refresh state,
      6./7. authenticated shutdown EXITS the process and a restart
         serves the next verdict with no ritual (LL-4) - these legs run
         ONLY when this preflight started the server (an autostart-owned
         server is never bounced by a verification tool; the honest skip
         label names the owner's manual exercise).
    The preflight LEAVES THE SERVER RUNNING (the long-lived model) and
    says so. Custody: every subprocess is bounded (M4).
    """
    import subprocess as sp
    import time

    # Exercise the exes the TRIAL will actually run: the kit's cmd files and
    # the live config invoke the REPO build-dir binaries (kit bin/ carries
    # pinned reference copies). Admission checks the client IMAGE PATH
    # against the install record — probing with the kit-bin copy would (and
    # did, 2026-09-24) deny 121 by design. Found by the preflight itself.
    repo_bin = REPO_ROOT / "build" / "vs2022-x64" / "Release"
    host_exe = repo_bin / "qiven-runtime-host.exe"
    hook_exe = repo_bin / "qiven-zcode-hook.exe"
    ctl_exe = repo_bin / "qiven-runtimectl.exe"
    kit_bin = kit_dir / "bin"
    for exe in (host_exe, hook_exe, ctl_exe):
        if not exe.exists():
            print(f"[FAIL] release binary missing: {exe} - run build-release first")
            return EXIT_FAIL
    for name in ("qiven-runtime-host.exe", "qiven-zcode-hook.exe", "qiven-runtimectl.exe"):
        if not (kit_bin / name).exists():
            print(f"[FAIL] kit reference copy missing: {kit_bin / name}")
            return EXIT_FAIL
    # ADR-0049 self-containment (2026-09-24 preflight incident): the host
    # below runs the KIT-INTERNAL deployment profile, passed EXPLICITLY --
    # never a CWD-derived default (the owner double-click cwd is the kit
    # folder, where no checkout-relative profile exists).
    profile_file = kit_profile(kit_dir)
    if not profile_file.exists():
        print(f"[FAIL] kit is not self-contained: deployment profile missing: "
              f"{profile_file} - rebuild the kit with the current h1_kit.py")
        return EXIT_FAIL

    probe_payload = json.dumps({
        "tool_name": "Bash",
        "tool_input": {"command": "qiven preflight probe"},
    })
    log_path = kit_dir / "preflight-host.log"

    def run_probe() -> tuple[int, str, str]:
        proc = sp.run(
            [str(hook_exe), "--event", "pre_tool", "--tool", "Bash",
             "--root", str(GOVERNED_ROOT), "--session-handle", token + "-preflight"],
            input=probe_payload, capture_output=True, text=True, timeout=20,
            creationflags=getattr(sp, "CREATE_NO_WINDOW", 0))
        return proc.returncode, proc.stdout.strip(), proc.stderr.strip()

    def run_session_start_probe() -> tuple[int, str, str]:
        # Trial-4 preflight blind spot (2026-09-26 incident): the old
        # preflight drove ONLY a pre_tool round trip, so the session_start
        # registration path -- where the hello/event deadline split lived --
        # was never exercised before the owner approved the config.
        proc = sp.run(
            [str(hook_exe), "--event", "session_start",
             "--root", str(GOVERNED_ROOT), "--session-handle", token + "-preflight"],
            input=probe_payload, capture_output=True, text=True, timeout=20,
            creationflags=getattr(sp, "CREATE_NO_WINDOW", 0))
        return proc.returncode, proc.stdout.strip(), proc.stderr.strip()

    result = EXIT_FAIL
    started_here = False

    def status_show() -> sp.CompletedProcess:
        return sp.run(
            [str(ctl_exe), "status", "show", "--root", str(GOVERNED_ROOT)],
            capture_output=True, text=True, timeout=20,
            creationflags=getattr(sp, "CREATE_NO_WINDOW", 0))

    def start_server() -> bool:
        """The normal start path (the same image/args start-host.cmd uses):
        launches the build-dir host with the KIT profile and --log, then
        waits bounded for a status round trip."""
        with log_path.open("a", encoding="utf-8", newline="\n") as log:
            sp.Popen(
                [str(host_exe), "--root", str(GOVERNED_ROOT),
                 "--profile", str(profile_file), "--log", str(log_path)],
                stdout=log, stderr=sp.STDOUT,
                creationflags=getattr(sp, "CREATE_NO_WINDOW", 0))
        deadline = time.monotonic() + 20.0
        while time.monotonic() < deadline:
            if status_show().returncode == 0:
                return True
            time.sleep(0.5)
        return False

    try:
        # Leg 1 - ensure the server is running (start via the normal path
        # if absent; already-running is the NORMAL state under LL-1).
        print("[ RUN] preflight: ensure the server is running")
        if status_show().returncode == 0:
            print("[ OK ] server already running (the normal state under LL-1)")
        else:
            print("[ RUN] no server answering - starting it via the normal path")
            if not start_server():
                print(f"[FAIL] server did not answer within 20 s; log: {log_path}")
                return EXIT_FAIL
            started_here = True
            print("[ OK ] server started with the kit-internal profile")

        # Leg 2 - verdict round trip (a REAL pipe verdict, not a boot echo).
        print("[ RUN] preflight: verdict round trip")
        code, _out, err = run_probe()
        verdict_ok = code == 0 or "(host verdict)" in err
        print(("[ OK ] " if verdict_ok else "[FAIL] ") +
              "verdict round trip complete" +
              ("" if verdict_ok else f" -- exit {code}: {err}"))
        if not verdict_ok:
            return EXIT_FAIL

        # Leg 3 - FIRST-CONTACT pre_tool with NO prior session_start (the
        # LL-2a live proof: a never-registered handle mints and is judged
        # on its merits - the trial-4 deny-114 ritual is retired).
        print("[ RUN] preflight: first-contact pre_tool (no session_start)")
        proc = sp.run(
            [str(hook_exe), "--event", "pre_tool", "--tool", "Bash",
             "--root", str(GOVERNED_ROOT),
             "--session-handle", token + "-firstcontact"],
            input=probe_payload, capture_output=True, text=True, timeout=20,
            creationflags=getattr(sp, "CREATE_NO_WINDOW", 0))
        first_contact_ok = (proc.returncode == 0
                            and "deny 114" not in proc.stderr)
        print(("[ OK ] " if first_contact_ok else "[FAIL] ") +
              "first contact judged on merits (no registration ritual)" +
              ("" if first_contact_ok else
               f" -- exit {proc.returncode}: {proc.stderr.strip()}"))
        if not first_contact_ok:
            return EXIT_FAIL

        # Leg 4 - session_start round trip asserted FAST (the trial-5
        # mechanism: no refresh work in the request path; wall-clock bound
        # 2000 ms for the FULL round trip, far under the retired 9750 class).
        print("[ RUN] preflight: session_start round trip (< 2000 ms)")
        started = time.monotonic()
        code, _out, err = run_session_start_probe()
        elapsed_ms = (time.monotonic() - started) * 1000.0
        fast_ok = (code == 0 and elapsed_ms < 2000.0
                   and "NOT registered" not in err
                   and "unexpected reply shape" not in err)
        print(("[ OK ] " if fast_ok else "[FAIL] ") +
              f"session_start round trip in {elapsed_ms:.0f} ms (< 2000)" +
              ("" if fast_ok else f" -- exit {code}: {err}"))
        if not fast_ok:
            return EXIT_FAIL

        # Leg 5 - status reports the refresh state (the host-autonomous
        # worker's observable).
        status = status_show()
        refresh_ok = (status.returncode == 0
                      and "refresh_state" in status.stdout)
        print(("[ OK ] " if refresh_ok else "[FAIL] ") +
              "status reports refresh state" +
              ("" if refresh_ok else f" -- exit {status.returncode}: "
                                     f"{status.stdout.strip()[:200]}"))
        if not refresh_ok:
            return EXIT_FAIL

        # Legs 6-7 - the restart pair, ONLY when this preflight started the
        # server (a verification tool does not bounce an autostart-owned
        # server mid-flight; stop-host + start-host remain the owner's
        # manual exercise of that path, GUIDE step P6).
        if started_here:
            print("[ RUN] preflight: authenticated shutdown exits the process")
            stop = sp.run(
                [str(ctl_exe), "host", "shutdown", "--root", str(GOVERNED_ROOT)],
                capture_output=True, text=True, timeout=20,
                creationflags=getattr(sp, "CREATE_NO_WINDOW", 0))
            deadline = time.monotonic() + 10.0
            stopped = False
            while time.monotonic() < deadline:
                if status_show().returncode != 0:
                    stopped = True
                    break
                time.sleep(0.5)
            print(("[ OK ] " if stopped else "[FAIL] ") +
                  f"server exited after the shutdown ack (ctl exit {stop.returncode})")
            if not stopped:
                return EXIT_FAIL
            print("[ RUN] preflight: restart brings it back with no residue")
            if not start_server():
                print(f"[FAIL] restart did not answer within 20 s; log: {log_path}")
                return EXIT_FAIL
            code, _out, err = run_probe()
            residue_ok = code == 0 or "(host verdict)" in err
            print(("[ OK ] " if residue_ok else "[FAIL] ") +
                  "next verdict succeeds with no re-registration ritual (LL-4)" +
                  ("" if residue_ok else f" -- exit {code}: {err}"))
            if not residue_ok:
                return EXIT_FAIL
        else:
            print("[SKIP-labeled] shutdown/restart legs: the server was "
                  "already running (autostart-owned); stop-host.cmd + "
                  "start-host.cmd remain the owner's manual exercise "
                  "(GUIDE step P6)")

        print("[ OK ] PREFLIGHT PASS - safe to approve the hook config in "
              "the ZCode UI")
        result = EXIT_OK
        return result
    finally:
        # §8 honesty on EVERY exit path (not only PASS): a started server
        # stays running on failure exits too, and the owner is told.
        print("[ NOTE ] the preflight LEAVES THE SERVER RUNNING (the "
              "long-lived model); stop-host.cmd / rollback.cmd stop it")


def main(argv=None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    if argv and argv[0] == "preflight":
        parser = argparse.ArgumentParser(prog="h1_kit.py preflight",
                                         description="run the kit pre-flight self-check")
        parser.add_argument("--kit", required=True, help="the kit package directory")
        parser.add_argument("--session-token", default=HOOK_SESSION_TOKEN_DEFAULT)
        pre = parser.parse_args(argv[1:])
        try:
            return cmd_preflight(Path(pre.kit), pre.session_token)
        except Exception as failure:  # bounded custody: no traceback escape (M4)
            print(f"[FAIL] preflight: {type(failure).__name__}: {failure}", file=sys.stderr)
            return EXIT_FAIL

    parser = argparse.ArgumentParser(description="build the H1 acceptance kit package")
    parser.add_argument("--gate", default="local", help="gate name whose receipt is required")
    parser.add_argument("--session-token", default=HOOK_SESSION_TOKEN_DEFAULT)
    parser.add_argument("--out", default=None,
                        help="kit root (default QIVEN_H1_ROOT or <workspace>/h1-kits)")
    args = parser.parse_args(argv)

    out_root = Path(args.out) if args.out else Path(
        os.environ.get("QIVEN_H1_ROOT", str(REPO_ROOT.parent / "h1-kits")))
    try:
        return build_kit(out_root, args.gate, args.session_token)
    except SystemExit as stop:
        print(f"{stop}", file=sys.stderr)
        return EXIT_FAIL


if __name__ == "__main__":
    raise SystemExit(main())
