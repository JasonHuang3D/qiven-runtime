#!/usr/bin/env python3
"""qiven h1_sim_gate.py - the simulated ZCode hook lifecycle gate (ADR-0055,
accepted 2026-09-26; OBL-20260926T234500Z-B4C5D6 item 4). Design:
docs/design/mvp4-simulated-gate.md.

The simulator replaces ONLY the caller and external inputs: it launches the
candidate production hook executable as an external process with the exact
stdin JSON and argument form the harness supplies, and runs the candidate
RuntimeHost behind the real named pipe with the packaged profile, in
isolated scratch roots under .generated-temp/h1-sim/. No host handler is
ever called directly and labeled integration.

Oracles come from the fixture catalogue (tests/fixtures/h1-sim/
catalogue.json: pinned zai-org/ZCode@29628c9 shapes, historical trial-4
captures, contract-derived parameters, incident audits) - never from
current implementation output.

Skip semantics (ADR-0055 decision 5): a skipped host/hook/scenario is
NOT_VALIDATED and fails this gate; there is no green skip. The gate task
therefore rejects absent, skipped, stale and failed receipts by
construction: it always re-runs the whole suite at the invoked tree, and
verify-receipt re-checks a stored receipt's digests against the live tree.

Receipt (decision 7): fixture catalogue digest, candidate exe digests,
profile digest, git HEAD, dependencies manifest digest, platform,
toolchain, Python version, exact command, per-case results, invariant
coverage map, typed states SIMULATED_HOOK_HOST_PASS /
PINNED_SOURCE_CONTRACT_REVIEWED / INSTALLED_DESKTOP_EXECUTION_UNVERIFIED.
The complete-mediation claim stays scoped to the mediated tuple; the
writable-child/delegation bypass is a REQUIRED negative control (decision
8) and is recorded as blocking complete mediation for delegation paths.

Windows-only (named pipes + DPAPI). On any other platform this gate
reports NOT_VALIDATED and exits non-zero.

Standard library only (Devkit python-standard law).

Usage:
  python tools/h1_sim_gate.py run [--dev] [--bin-dir <dir>]
  python tools/h1_sim_gate.py verify-receipt [--receipt <file>]
  python tools/h1_sim_gate.py old-fail-i4 --bin-dir <dir>
"""

from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes as wt
import hashlib
import json
import os
import re
import shutil
import sqlite3
import struct
import subprocess
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
EXIT_OK, EXIT_FAIL = 0, 1

FIXTURES = REPO_ROOT / "tests" / "fixtures" / "h1-sim" / "catalogue.json"
PROFILE_NAME = "zcode-jason-context-record-mvp.yaml"
WORKROOT = REPO_ROOT / ".generated-temp" / "h1-sim"
RECEIPTS = WORKROOT / "receipts"

# Wire constants (src/ipc/framing.hpp; the I-series wire cases fail typed
# when this table drifts from the implementation).
FRAME_MAGIC = 0x51565231
PROTOCOL_VERSION = 1
FRAME_PREFIX = 64  # 32-byte header + 32-byte HMAC
ERR_AUTH, ERR_REPLAY, ERR_FRAME = 62, 63, 64

# Deny codes (include/qiven/runtime/adapter/zcode_hook.hpp).
R_GOVERNED_WRITE, R_BASH_REFERENCE, R_UNKNOWN_TOOL, R_SCOPE_MISMATCH = 110, 111, 112, 113
R_UNKNOWN_SESSION, R_CORRELATION, R_EXPIRED, R_PAYLOAD = 114, 115, 117, 118
R_NO_LISTENER, R_ADMISSION = 120, 121

MAX_HOOK_PAYLOAD = 1024 * 1024
HOOK_TIMEOUT_S = 25          # harness budget is 15 s; headroom for CI noise
SESSION_TX_BOUND_S = 10.0    # INV-10: whole hello+event under one monotonic bound
HOST_BOOT_BOUND_S = 20.0
CREATE_NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)


class GateFailure(Exception):
    """A gate-level failure (environment, precondition, receipt)."""


# ---------------------------------------------------------------------------
# Wire client (fault injection + shutdown only; lifecycle cases go through
# the real hook executable - design section 9)


class DATA_BLOB(ctypes.Structure):
    _fields_ = [("cbData", wt.DWORD), ("pbData", ctypes.POINTER(ctypes.c_char))]


def dpapi_unprotect(data: bytes) -> bytes:
    blob_in = DATA_BLOB(len(data), ctypes.cast(ctypes.create_string_buffer(data, len(data)),
                                               ctypes.POINTER(ctypes.c_char)))
    blob_out = DATA_BLOB()
    if not ctypes.windll.crypt32.CryptUnprotectData(
            ctypes.byref(blob_in), None, None, None, None, 0, ctypes.byref(blob_out)):
        raise GateFailure("CryptUnprotectData failed (same-user secret expected)")
    try:
        return ctypes.string_at(blob_out.pbData, blob_out.cbData)
    finally:
        ctypes.windll.kernel32.LocalFree(blob_out.pbData)


def hmac_sha256_raw(key: bytes, message: bytes) -> bytes:
    import hmac as _hmac
    return _hmac.new(key, message, hashlib.sha256).digest()


def hmac_frame(key: bytes, body: bytes, request_id: int, connection_seq: int,
               magic: int = FRAME_MAGIC, proto: int = PROTOCOL_VERSION,
               corrupt_mac: bool = False) -> bytes:
    header = struct.pack("<IIQQQ", magic & 0xFFFFFFFF, proto & 0xFFFF,
                         len(body), request_id, connection_seq)
    mac = bytearray(hmac_sha256_raw(key, header + body))
    if corrupt_mac:
        mac[0] ^= 0xFF
    return header + bytes(mac) + body


class WireClient:
    """Speaks the authenticated frame protocol directly. Used ONLY for
    fault-injection legs and the authenticated shutdown."""

    def __init__(self, runtime_root: Path):
        secret_file = runtime_root / "client.secret.dpapi"
        install_file = runtime_root / "install.id"
        if not secret_file.exists() or not install_file.exists():
            raise GateFailure(f"installation state incomplete at {runtime_root}")
        self.key = dpapi_unprotect(secret_file.read_bytes())
        self.install_id = install_file.read_text(encoding="utf-8").strip()
        self.pipe = open(rf"\\.\pipe\qiven-runtime-{self.install_id}-v1",
                         "r+b", buffering=0)

    def send(self, frame: bytes) -> None:
        self.pipe.write(frame)

    def _read_exact(self, count: int) -> bytes:
        chunks = []
        remaining = count
        while remaining > 0:
            chunk = self.pipe.read(remaining)
            if not chunk:
                raise ConnectionError("pipe closed by host")
            chunks.append(chunk)
            remaining -= len(chunk)
        return b"".join(chunks)

    def read_reply(self) -> dict:
        prefix = self._read_exact(FRAME_PREFIX)
        _magic, _proto, body_len, _rid, _seq = struct.unpack("<IIQQQ", prefix[:32])
        if body_len > 4 * MAX_HOOK_PAYLOAD:
            raise GateFailure(f"reply body length insane: {body_len}")
        body = self._read_exact(body_len) if body_len else b""
        return json.loads(body.decode("utf-8"))

    def request(self, body_obj: dict, request_id: int, connection_seq: int,
                deadline_ms: int = 3000, **frame_kwargs) -> dict:
        body = json.dumps(body_obj, separators=(",", ":")).encode("utf-8")
        self.send(hmac_frame(self.key, body, request_id, connection_seq, **frame_kwargs))
        return self.read_reply()

    def hello(self, connection_seq: int = 1, deadline_ms: int = 3000, **kw) -> dict:
        return self.request({"kind": "hello", "client_kind": "zcode-hook",
                             "client_build": 1, "request_id": 1,
                             "deadline_ms": deadline_ms},
                            1, connection_seq, **kw)

    def close(self) -> None:
        try:
            self.pipe.close()
        except OSError:
            pass


# ---------------------------------------------------------------------------
# Rig primitives


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def run_hook(hook_exe: Path, event: str, root: Path, payload: bytes,
             tool: str | None = None, session_handle: str | None = None,
             timeout_s: float = HOOK_TIMEOUT_S) -> tuple[int, str, float]:
    argv = [str(hook_exe), "--event", event, "--root", str(root)]
    if tool:
        argv += ["--tool", tool]
    if session_handle:
        argv += ["--session-handle", session_handle]
    started = time.monotonic()
    proc = subprocess.run(argv, input=payload, capture_output=True,
                          timeout=timeout_s, creationflags=CREATE_NO_WINDOW)
    elapsed = time.monotonic() - started
    return proc.returncode, proc.stderr.decode("utf-8", errors="replace").strip(), elapsed


def boot_host(host_exe: Path, root: Path, profile: Path | None, log: Path,
              cwd: Path | None = None):
    argv = [str(host_exe), "--root", str(root)]
    if profile is not None:
        argv += ["--profile", str(profile)]
    handle = log.open("w", encoding="utf-8", newline="\n")
    proc = subprocess.Popen(argv, stdout=handle, stderr=subprocess.STDOUT,
                            creationflags=CREATE_NO_WINDOW,
                            cwd=str(cwd) if cwd else None)
    deadline = time.monotonic() + HOST_BOOT_BOUND_S
    served = False
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            handle.close()
            raise GateFailure(f"host exited during boot (exit {proc.returncode}); "
                              f"log: {log}")
        if log.exists() and "[ OK ] serving" in log.read_text(encoding="utf-8",
                                                              errors="replace"):
            served = True
            break
        time.sleep(0.1)
    if not served:
        proc.kill()
        handle.close()
        raise GateFailure(f"host did not serve within {HOST_BOOT_BOUND_S}s; log: {log}")
    return proc, handle


def stop_host(root: Path, host_proc, log_handle) -> None:
    """Authenticated shutdown via the wire client (INV-16: ack + real exit)."""
    client = WireClient(root / ".qiven" / "runtime")
    try:
        reply = client.request({"kind": "shutdown", "grace_ms": 1500, "request_id": 99,
                                "deadline_ms": 3000}, 99, 1)
        if reply.get("kind") != "shutdown_ack":
            raise GateFailure(f"shutdown reply was {reply.get('kind')}")
    finally:
        client.close()
    deadline = time.monotonic() + 10.0
    while time.monotonic() < deadline:
        if host_proc.poll() is not None:
            log_handle.close()
            return
        time.sleep(0.1)
    host_proc.kill()
    log_handle.close()
    raise GateFailure("host did NOT exit within 10 s of the shutdown ack")


def journal_rows(root: Path, query: str, args: tuple = ()) -> list:
    path = root / ".qiven" / "runtime" / "journal.sqlite3"
    conn = sqlite3.connect(f"file:{path.as_posix()}?mode=ro", uri=True, timeout=5)
    try:
        return list(conn.execute(query, args))
    finally:
        conn.close()


def audit_events(root: Path) -> list[tuple[str, str]]:
    rows = journal_rows(root, "SELECT kind, payload FROM audit_events ORDER BY seq")
    return [(kind, (payload.decode("utf-8", errors="replace")
                    if isinstance(payload, bytes) else str(payload)))
            for kind, payload in rows]


class Subst:
    """Fixture placeholder substitution (catalogue substitution_contract)."""

    def __init__(self, sim_root: Path, outside_root: Path):
        self.values = {
            "{SIM_ROOT}": sim_root.as_posix(),
            "{SIM_ROOT_BSLASH}": str(sim_root),
            "{OUTSIDE_ROOT}": outside_root.as_posix(),
            "{WORKSPACE_ROOT}": "<workspace-root>",
            "{WORKSPACE_ROOT_POSIX}": "<workspace-root>",
            "{TRANSCRIPT}": "<temp-transcript>",
            "{MODEL}": "<model>",
            "{TS}": "2026-09-26T00:00:00.000Z",
            "{TRACE}": "sim-trace-0001",
            "{TURN}": "sim-turn-0001",
            "{CALL}": "sim-call-0001",
        }

    def payload(self, template: str, session_id: str) -> bytes:
        text = template.replace("{SESSION_ID}", session_id)
        for key, value in self.values.items():
            text = text.replace(key, value)
        return text.encode("utf-8")


# ---------------------------------------------------------------------------
# The rig


class Rig:
    def __init__(self, bin_dir: Path | None = None):
        self.bin = bin_dir or (REPO_ROOT / "build" / "vs2022-x64" / "Release")
        self.host_exe = self.bin / "qiven-runtime-host.exe"
        self.hook_exe = self.bin / "qiven-zcode-hook.exe"
        self.catalogue = json.loads(FIXTURES.read_text(encoding="utf-8"))
        self.templates = self.catalogue["templates"]
        self.run_root = WORKROOT / f"run-{time.strftime('%Y%m%d-%H%M%S')}-{os.getpid()}"
        self.cases: list[dict] = []
        self.incident_map: dict[str, list[str]] = {}
        self.coverage: dict[str, list[str]] = {}
        self.detector_limits: list[str] = []
        self.live_procs: list = []  # leak guard: every booted host, killed at end

    # -- case registry ------------------------------------------------------

    def case(self, case_id: str, invariants: list[str], oracle: str,
             incident: str | None = None):
        def register(fn):
            self.cases.append({"id": case_id, "invariants": invariants,
                               "oracle": oracle, "run": fn})
            for inv in invariants:
                self.coverage.setdefault(inv, []).append(case_id)
            if incident:
                self.incident_map.setdefault(incident, []).append(case_id)
            return fn
        return register

    # -- environment --------------------------------------------------------

    # Minimal cognition fixture files the boot publisher reads from the
    # refs/heads/main tree (profile cognition.source_paths). The policy file
    # is the REAL accepted instance (digest-pinned by the profile); the rest
    # are stubs (no digest pins) so the fixture stays deterministic.
    COGNITION_STUBS = {
        "memory/index.yaml": "schema_version: 1\nrecords: []\n",
        "memory/records/FIXTURE.md": "fixture record\n",
        "obligations/index.yaml": "schema_version: 1\nrecords: []\n",
        "obligations/FIXTURE.md": "fixture obligation\n",
        "decisions/index.yaml": "schema_version: 1\nrecords: []\n",
        "decisions/FIXTURE.md": "fixture decision\n",
        "state/active-work.yaml": "schema_version: 2\nprogram: qiven-sim-fixture\n",
        "state/current.md": "# fixture current state\n",
        "state/roadmap.yaml": "schema_version: 1\nitems: []\n",
        "governance/authority.yaml": "schema_version: 1\n",
    }

    def fresh_root(self, name: str, with_profile: bool = True,
                   with_git: bool = True) -> tuple[Path, Path]:
        """One scratch governed root. RuntimeHost::boot publishes the active
        cognition bundle from the root's refs/heads/main tree (runtime_host
        step 5), so a servable root is a minimal GIT FIXTURE repo carrying
        the pinned invocation-policy.yaml plus stub source paths - the
        boot-real publish path, offline and deterministic. The outside root
        lives under outside/<name> so its path never lexically CONTAINS the
        governed root path (the Bash root-containment detector matches
        text). The client install record is pre-created listing the rig's
        python and the build-dir client images so the wire fault client is
        admitted (same-user DACL remains the boundary; boot merges its own
        images on top)."""
        root = self.run_root / name
        outside = self.run_root / "outside" / name
        outside.mkdir(parents=True)
        if with_profile:
            dst = root / "config" / "profiles" / PROFILE_NAME
            dst.parent.mkdir(parents=True)
            shutil.copy2(REPO_ROOT / "config" / "profiles" / PROFILE_NAME, dst)
        policy = root / "runtime" / "invocation-policy.yaml"
        policy.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(FIXTURES.parent / "invocation-policy.yaml", policy)
        for rel, text in self.COGNITION_STUBS.items():
            path = root / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8", newline="\n")
        for governed_dir in ("memory/records", "obligations", "sessions", "state"):
            (root / governed_dir).mkdir(parents=True, exist_ok=True)
        # Pre-create the client install record (admission): the wire fault
        # client runs from python.exe; the hook/ctl images live in the build
        # dir. Boot merges its own images on top (idempotent ClientRecord).
        runtime_dir = root / ".qiven" / "runtime"
        runtime_dir.mkdir(parents=True, exist_ok=True)
        client_images = [sys.executable,
                         str(self.bin / "qiven-zcode-hook.exe"),
                         str(self.bin / "qiven-runtimectl.exe"),
                         str(self.bin / "qiven-runtime-host.exe")]
        (runtime_dir / "clients.json").write_text(
            json.dumps({"clients": client_images}, indent=2) + "\n",
            encoding="utf-8", newline="\n")
        if with_git:
            git = ["git", "-c", "core.hooksPath=", "-c", "init.defaultBranch=main"]
            for args in (
                ["init", "--quiet", str(root)],
                ["-C", str(root), "add", "-A"],
                ["-C", str(root), "-c", "user.email=sim@qiven.invalid",
                 "-c", "user.name=qiven-sim", "commit", "--quiet", "-m",
                 "sim fixture cognition snapshot"],
            ):
                proc = subprocess.run(git + args, capture_output=True, timeout=30,
                                      creationflags=CREATE_NO_WINDOW)
                if proc.returncode != 0:
                    raise GateFailure(f"fixture git {args[0]} failed: "
                                      f"{proc.stderr.decode(errors='replace')}")
        return root, outside

    def expect(self, condition: bool, message: str) -> None:
        if not condition:
            raise AssertionError(message)

    def expect_deny(self, code: int, stderr: str, exit_code: int, fragment: str = "",
                    source: str = "") -> None:
        self.expect(exit_code == 2, f"expected exit 2, got {exit_code}: {stderr}")
        self.expect(f"deny {code}" in stderr, f"expected deny {code} in: {stderr}")
        if fragment:
            self.expect(fragment in stderr, f"expected '{fragment}' in: {stderr}")
        if source:
            self.expect(source in stderr, f"expected source tag '{source}' in: {stderr}")

    def session_id_of(self, stderr: str) -> str:
        found = re.search(r"session registered \(id ([0-9a-f]+)\)", stderr)
        self.expect(found is not None, f"no registered session id in: {stderr}")
        return found.group(1)

    def hook_payload(self, template_id: str, root: Path, outside: Path,
                     session: str) -> bytes:
        return Subst(root, outside).payload(self.templates[template_id]["payload"], session)

    def registered(self, root: Path, outside: Path, handle: str,
                   session_label: str, require_note: bool = True) -> str | None:
        """Register one session handle; returns the runtime session id when
        the registration note is observable. The REPEAT path may answer
        silently (idempotent branch with a fresh-enough cognition window:
        allow_silent) - identity stability is then proven by the journal's
        exactly-one-row law, not by the text."""
        payload = self.hook_payload("pinned.session-start.startup", root, outside,
                                    session_label)
        code, err, _ = run_hook(self.hook_exe, "session_start", root, payload,
                                session_handle=handle)
        self.expect(code == 0, f"registration for {handle} failed: {err}")
        if require_note:
            self.expect("session registered (id" in err,
                        f"first registration must carry the note: {err}")
            return self.session_id_of(err)
        return self.session_id_of(err) if "session registered (id" in err else None

    def payload_with_target(self, root: Path, outside: Path, session: str,
                            tool: str, file_path: str | None = None,
                            command: str | None = None) -> bytes:
        """A minimal pinned-contract PreToolUse payload with one target."""
        tool_input = {}
        if file_path is not None:
            tool_input["file_path"] = file_path
        if command is not None:
            tool_input["command"] = command
        return json.dumps({
            "cwd": "<workspace-root>", "hookEventName": "PreToolUse", "mode": "yolo",
            "riskLevel": "medium", "sessionId": session,
            "sideEffectScope": "workspace", "timestamp": "2026-09-26T00:00:00.000Z",
            "toolCallId": "sim-call-0001", "toolInput": tool_input,
            "toolName": tool, "traceId": "sim-trace-0001",
        }, separators=(",", ":")).encode("utf-8")

    def clear_outstanding(self, root: Path, handle: str, tool: str) -> None:
        code, _err, _ = run_hook(self.hook_exe, "post_tool", root,
                                 json.dumps({"toolName": tool, "toolInput": {},
                                             "sessionId": "x"}).encode(),
                                 tool=tool, session_handle=handle)
        self.expect(code == 0,
                    f"outstanding cleanup post_tool({tool}) failed: {_err}")

    # -- the scenario table -------------------------------------------------

    def build_scenarios(self) -> None:  # noqa: C901 - the scenario table IS the artifact
        hook = self.hook_exe

        # ===================================================================
        # Group S: SessionStart registration (INV-1, 8, 9, 10, 14)
        # ===================================================================
        s_root, s_outside = self.fresh_root("gS")
        s_proc, s_fh = boot_host(self.host_exe, s_root,
                                 s_root / "config" / "profiles" / PROFILE_NAME,
                                 self.run_root / "gS-host.log")
        self.live_procs.append(s_proc)
        s_ids: dict[str, str] = {}

        @self.case("S1.pinned-startup-registers", ["INV-1", "INV-9", "INV-10", "INV-14"],
                   "pinned.session-start.startup + host registration law")
        def _s1():
            sid = self.registered(s_root, s_outside, "h-s1", "sess-s1")
            s_ids["h-s1"] = sid

        @self.case("S2.idempotent-repeat-stable", ["INV-1"],
                   "host handle_session_start idempotence clause (repeat may "
                   "answer silently; identity stability is proven by S8's "
                   "exactly-one-row law)")
        def _s2():
            code, err, _ = run_hook(hook, "session_start", s_root,
                                    self.hook_payload("pinned.session-start.startup",
                                                      s_root, s_outside, "sess-s1"),
                                    session_handle="h-s1")
            self.expect(code == 0, f"repeat exit {code}: {err}")
            if "session registered (id" in err:
                self.expect(self.session_id_of(err) == s_ids["h-s1"],
                            "repeat registration minted a DIFFERENT runtime identity")

        for source in ("resume", "clear", "compact"):
            @self.case(f"S3.{source}-registers", ["INV-1"],
                       f"contract-derived: SessionStartHookInput source={source}")
            def _s3(payload_source=source):
                self.registered(s_root, s_outside, f"h-{payload_source}",
                                f"sess-{payload_source}")

            @self.case(f"S3.{source}-repeat-idempotent", ["INV-1"],
                       f"idempotence for source={source}")
            def _s3r(payload_source=source):
                self.registered(s_root, s_outside, f"h-{payload_source}",
                                f"sess-{payload_source}", require_note=False)

        @self.case("S4.no-session-field-registers", ["INV-1"], "incident-audit trial 1",
                   incident="I1")
        def _s4():
            payload = self.hook_payload("pinned.session-start.no-session-field",
                                        s_root, s_outside, "unused")
            code, err, _ = run_hook(hook, "session_start", s_root, payload,
                                    session_handle="h-nofield")
            self.expect(code == 0 and "session registered (id" in err,
                        f"trial-1 class regression: {err}")

        @self.case("S5.capture-resume-dualkey-registers", ["INV-1"],
                   "capture.0.session-start.resume.dualkey")
        def _s5():
            payload = self.hook_payload("capture.0.session-start.resume.dualkey",
                                        s_root, s_outside, "sess-cap0")
            code, err, _ = run_hook(hook, "session_start", s_root, payload,
                                    session_handle="h-cap0")
            self.expect(code == 0 and "session registered (id" in err,
                        f"capture row 0: {err}")
            s_ids["h-cap0"] = self.session_id_of(err)

        @self.case("S6.capture-startup-dualkey-registers", ["INV-1"],
                   "capture.1.session-start.startup.dualkey")
        def _s6():
            payload = self.hook_payload("capture.1.session-start.startup.dualkey",
                                        s_root, s_outside, "sess-cap1")
            code, err, _ = run_hook(hook, "session_start", s_root, payload,
                                    session_handle="h-cap1")
            self.expect(code == 0 and "session registered (id" in err,
                        f"capture row 1: {err}")

        @self.case("S7.capture-repeat-idempotent", ["INV-1"], "capture row 0 repeat "
                   "(repeat may answer silently; one-row law proves stability)")
        def _s7():
            payload = self.hook_payload("capture.0.session-start.resume.dualkey",
                                        s_root, s_outside, "sess-cap0")
            code, err, _ = run_hook(hook, "session_start", s_root, payload,
                                    session_handle="h-cap0")
            self.expect(code == 0, f"capture repeat: {err}")
            if "session registered (id" in err:
                self.expect(self.session_id_of(err) == s_ids["h-cap0"],
                            "capture-form repeat minted a new identity")

        @self.case("S8.exactly-one-row-per-handle", ["INV-1", "INV-8"],
                   "journal sessions/audit law")
        def _s8():
            stop_host(s_root, s_proc, s_fh)
            rows = [p for kind, p in audit_events(s_root) if kind == "session_registered"]
            for handle in ("h-s1", "h-resume", "h-clear", "h-compact", "h-nofield",
                           "h-cap0", "h-cap1"):
                count = sum(1 for p in rows if p.endswith("|" + handle))
                self.expect(count == 1,
                            f"handle {handle}: {count} session_registered rows (want 1)")

        # ===================================================================
        # Group B: PreToolUse classification (INV-2, 3, 4, 13, 15)
        # ===================================================================
        b_root, b_outside = self.fresh_root("gB")
        b_proc, b_fh = boot_host(self.host_exe, b_root, None, self.run_root / "gB-host.log")
        self.live_procs.append(b_proc)
        self.registered(b_root, b_outside, "h-b", "sess-b")

        # Capture P1/P2 target state/h1-probe.md: NOT a governed path (the
        # profile governs active-work.yaml/current.md under state/, plus the
        # dir entries) - the honest lexical verdict is not_governed ALLOW.
        # The GUIDE's "EXPECT deny 110" for P1 was a design intent never
        # observed live (every trial-4 probe died at deny 114 before
        # classification). Recorded here as a detector-scope observation.
        governed_writes = [
            ("B1.capture-P1-write-state", "capture.2.pre-tool.write.governed.dualkey",
             "Write"),
            ("B2.pinned-write-current-md", "pinned.pre-tool.write", "Write"),
            ("B3.pinned-edit-memory-records", "pinned.pre-tool.edit", "Edit"),
            ("B4.write-obligations-index", "pinned.pre-tool.write", "Write"),
            ("B5.write-sessions-file", "pinned.pre-tool.write", "Write"),
            ("B6.write-memory-index", "pinned.pre-tool.write", "Write"),
            ("B7.write-active-work", "pinned.pre-tool.write", "Write"),
            ("B8.capture-P2-edit-governed", "capture.12.pre-tool.edit.governed.dualkey",
             "Edit"),
        ]
        # Extra governed matrix: Edit across the remaining governed paths
        # (pinned-contract shapes; the Write legs above already cover them).
        for slug, rel in (("B4b.edit-obligations-index", "obligations/index.yaml"),
                          ("B5b.edit-sessions-file", "sessions/2026-09-26-x.md"),
                          ("B6b.edit-memory-index", "memory/index.yaml"),
                          ("B7b.edit-active-work", "state/active-work.yaml")):
            governed_writes.append((slug, None, "Edit"))
            governed_writes[-1] = (slug, rel, "Edit")  # rel used below

        for case_id, template, tool in governed_writes[:8]:
            is_capture_probe = case_id in ("B1.capture-P1-write-state",
                                           "B8.capture-P2-edit-governed")
            @self.case(case_id, ["INV-3", "INV-15"],
                       f"{template}; " + ("capture P1/P2 target is outside the "
                                          "governed-path LIST - detector-scope "
                                          "observation" if is_capture_probe else
                                          "profile governed_paths"))
            def _deny_captured(template_id=template, tool_name=tool, cid=case_id,
                               probe=is_capture_probe):
                payload = self.hook_payload(template_id, b_root, b_outside, "sess-b")
                target = json.loads(payload)["toolInput"]["file_path"]
                before = Path(target).read_bytes() if Path(target).exists() else None
                code, err, _ = run_hook(hook, "pre_tool", b_root, payload,
                                        tool=tool_name, session_handle="h-b")
                if probe:
                    # state/h1-probe.md is not a governed path: the honest
                    # verdict is allow (the trials' "EXPECT deny 110" was a
                    # design intent never observed live - every trial-4 probe
                    # died at deny 114 before classification).
                    self.expect(code == 0 and err == "",
                                f"capture P1/P2 target must allow lexically: {err}")
                    self.detector_limits.append(cid)
                    self.clear_outstanding(b_root, "h-b", tool_name)
                    return
                self.expect_deny(R_GOVERNED_WRITE, err, code, source="(host verdict)")
                after = Path(target).read_bytes() if Path(target).exists() else None
                self.expect(after == before,
                            "DENY executed an effect (target changed)")

        for slug, rel, _tool in governed_writes[8:]:
            @self.case(slug, ["INV-3", "INV-15"], f"pinned-contract Edit -> {rel}")
            def _deny_edit(rel_path=rel):
                payload = self.payload_with_target(
                    b_root, b_outside, "sess-b", "Edit",
                    file_path=f"{b_root.as_posix()}/{rel_path}")
                code, err, _ = run_hook(hook, "pre_tool", b_root, payload,
                                        tool="Edit", session_handle="h-b")
                self.expect_deny(R_GOVERNED_WRITE, err, code)

        bash_governed = [
            ("B9.capture-P3-bash-governed", "capture.13.pre-tool.bash.governed.dualkey",
             None),
            ("B10.pinned-bash-root-reference", "pinned.pre-tool.bash", None),
            ("B11.capture-P6-bash-governed", "capture.17.pre-tool.bash.governed-late.dualkey",
             None),
            ("B11b.bash-posix-root-reference", None,
             lambda r, o: f"echo x > {r.as_posix()}/memory/records/via-bash.md"),
            ("B11c.bash-backslash-root-lexical-gap", None,
             lambda r, o: f"echo x > {str(r)}\\memory\\records\\via-bash.md"),
            ("B11d.bash-governed-text-reference", None,
             lambda r, o: "cat obligations/index.yaml somewhere"),
            ("B11e.bash-memory-text-reference", None,
             lambda r, o: "grep x memory/records/MEM-1.md"),
        ]
        for case_id, template, command_fn in bash_governed:
            @self.case(case_id, ["INV-3", "INV-13", "INV-15"],
                       "conservative_text_reference detector + repo-root containment")
            def _deny_bash(template_id=template, cmd_fn=command_fn, cid=case_id):
                if template_id:
                    payload = self.hook_payload(template_id, b_root, b_outside, "sess-b")
                    code, err, _ = run_hook(hook, "pre_tool", b_root, payload,
                                            tool="Bash", session_handle="h-b")
                    self.expect_deny(R_BASH_REFERENCE, err, code)
                else:
                    command = cmd_fn(b_root, b_outside)
                    payload = self.payload_with_target(b_root, b_outside, "sess-b",
                                                       "Bash", command=command)
                    code, err, _ = run_hook(hook, "pre_tool", b_root, payload,
                                            tool="Bash", session_handle="h-b")
                    if cid.endswith("lexical-gap"):
                        # The conservative detector matches TEXT: a backslash
                        # root form never contains the slash-form root, so the
                        # action is honestly outside the detector - recorded as
                        # a detector-scope limit, never claimed as authorization.
                        self.expect(code == 0 and err == "",
                                    f"backslash root form is outside the TEXT "
                                    f"detector (expected allow): {err}")
                        self.detector_limits.append(cid)
                        self.clear_outstanding(b_root, "h-b", "Bash")
                    else:
                        self.expect_deny(R_BASH_REFERENCE, err, code)

        @self.case("B12.traversal-form-denied", ["INV-3"],
                   "host traversal-form rejection clause")
        def _b12():
            payload = self.hook_payload("pinned.pre-tool.write", b_root, b_outside,
                                        "sess-b").replace(b"state/current.md",
                                                          b"../../escape.md")
            code, err, _ = run_hook(hook, "pre_tool", b_root, payload,
                                    tool="Write", session_handle="h-b")
            self.expect_deny(R_GOVERNED_WRITE, err, code, fragment="traversal")

        @self.case("B13.unregistered-write-denied-114", ["INV-2"],
                   "trial-4 audit: deny 114 fires BEFORE classification", incident="I4")
        def _b13():
            payload = self.hook_payload("pinned.pre-tool.write", b_root, b_outside,
                                        "sess-ghost")
            code, err, _ = run_hook(hook, "pre_tool", b_root, payload,
                                    tool="Write", session_handle="h-ghost")
            self.expect_deny(R_UNKNOWN_SESSION, err, code,
                             fragment="SessionStart must fire first")

        @self.case("B14.unregistered-outside-denied-114", ["INV-2"],
                   "trial-4 audit: even the outside-scope probe denied 114",
                   incident="I4")
        def _b14():
            payload = self.hook_payload("capture.14.pre-tool.bash.outside.dualkey",
                                        b_root, b_outside, "sess-ghost")
            code, err, _ = run_hook(hook, "pre_tool", b_root, payload,
                                    tool="Bash", session_handle="h-ghost")
            self.expect_deny(R_UNKNOWN_SESSION, err, code)

        @self.case("B15.unknown-tool-denied-112", ["INV-2"],
                   "tool-inventory fail-closed law")
        def _b15():
            payload = self.hook_payload("pinned.pre-tool.write", b_root, b_outside,
                                        "sess-b").replace(b"Write", b"WebSearch")
            code, err, _ = run_hook(hook, "pre_tool", b_root, payload,
                                    tool="WebSearch", session_handle="h-b")
            self.expect_deny(R_UNKNOWN_TOOL, err, code, fragment="tool inventory")

        @self.case("B16.tool-contradiction-denied-118", ["INV-2"],
                   "deny-118 audit: payload contradicts the registration template",
                   incident="I1")
        def _b16():
            payload = self.hook_payload("pinned.pre-tool.write", b_root, b_outside,
                                        "sess-b")
            code, err, _ = run_hook(hook, "pre_tool", b_root, payload,
                                    tool="Bash", session_handle="h-b")
            self.expect_deny(R_PAYLOAD, err, code, fragment="misregistration",
                             source="hook-client")

        @self.case("B17.oversize-payload-denied-118", ["INV-2"],
                   "max_hook_payload_bytes law")
        def _b17():
            blob = b'{"x":"' + b"A" * (MAX_HOOK_PAYLOAD + 1) + b'"}'
            code, err, _ = run_hook(hook, "pre_tool", b_root, blob,
                                    tool="Write", session_handle="h-b")
            self.expect_deny(R_PAYLOAD, err, code, source="hook-client")

        @self.case("B18.unreadable-payload-denied-118", ["INV-2"],
                   "trial-2 heap-residue class: garbage bytes")
        def _b18():
            code, err, _ = run_hook(hook, "pre_tool", b_root, b"\x00\xff\xfe garbage \x93",
                                    tool="Write", session_handle="h-b")
            self.expect_deny(R_PAYLOAD, err, code, source="hook-client")

        @self.case("B19.write-without-file-path-denied-118", ["INV-2"],
                   "host no-target fail-closed clause")
        def _b19():
            payload = b'{"tool_name":"Write","tool_input":{"content":"x"}}'
            code, err, _ = run_hook(hook, "pre_tool", b_root, payload,
                                    tool="Write", session_handle="h-b")
            self.expect_deny(R_PAYLOAD, err, code, fragment="no file_path",
                             source="(host verdict)")

        @self.case("B20.bash-without-command-denied-118", ["INV-2"],
                   "host no-target fail-closed clause")
        def _b20():
            payload = b'{"tool_name":"Bash","tool_input":{"description":"empty"}}'
            code, err, _ = run_hook(hook, "pre_tool", b_root, payload,
                                    tool="Bash", session_handle="h-b")
            self.expect_deny(R_PAYLOAD, err, code, fragment="no command",
                             source="(host verdict)")

        @self.case("B21.degraded-session-denies-113", ["INV-4"],
                   "capability-handshake mismatch law (fault profile: undeclared tool)")
        def _b21():
            fault_root, fault_outside = self.fresh_root("gB21")
            profile = fault_root / "config" / "profiles" / PROFILE_NAME
            text = profile.read_text(encoding="utf-8").replace(
                "tool_inventory:",
                "tool_inventory:\n  - tool: NotebookEdit\n    capability: 4\n"
                "    extraction: file_path\n    detector: exact_path")
            profile.write_text(text, encoding="utf-8")
            proc, fh = boot_host(self.host_exe, fault_root, profile,
                                 self.run_root / "gB21-host.log")
            self.live_procs.append(proc)
            try:
                self.registered(fault_root, fault_outside, "h-d", "sess-d")
                payload = self.hook_payload("pinned.pre-tool.write", fault_root,
                                            fault_outside, "sess-d")
                code, err, _ = run_hook(hook, "pre_tool", fault_root, payload,
                                        tool="Write", session_handle="h-d")
                self.expect_deny(R_SCOPE_MISMATCH, err, code,
                                 fragment="manifest lacks declared tool 'NotebookEdit'")
            finally:
                stop_host(fault_root, proc, fh)

        @self.case("B22.expired-cognition-denies-117", ["INV-4"],
                   "cognition freshness window law (fault: stale refresh meta)")
        def _b22():
            fault_root, fault_outside = self.fresh_root("gB22")
            proc, fh = boot_host(self.host_exe, fault_root, None,
                                 self.run_root / "gB22-host.log")
            self.live_procs.append(proc)
            try:
                self.registered(fault_root, fault_outside, "h-e0", "sess-e0")
            finally:
                stop_host(fault_root, proc, fh)
            journal = fault_root / ".qiven" / "runtime" / "journal.sqlite3"
            conn = sqlite3.connect(journal, timeout=5)
            stale = str(int(time.time() * 1000) - 61 * 24 * 3600 * 1000)
            conn.execute("UPDATE runtime_meta SET value=? WHERE key='last_refresh_ok_ms'",
                         (stale,))
            conn.commit()
            conn.close()
            proc, fh = boot_host(self.host_exe, fault_root, None,
                                 self.run_root / "gB22-host2.log")
            self.live_procs.append(proc)
            try:
                self.registered(fault_root, fault_outside, "h-e1", "sess-e1")
                payload = self.hook_payload("pinned.pre-tool.write", fault_root,
                                            fault_outside, "sess-e1")
                code, err, _ = run_hook(hook, "pre_tool", fault_root, payload,
                                        tool="Write", session_handle="h-e1")
                self.expect_deny(R_EXPIRED, err, code)
            finally:
                stop_host(fault_root, proc, fh)

        @self.case("B23.no-host-deny-120-honest", ["INV-5"],
                   "2026-09-24 taxonomy split (typed no-listener)")
        def _b23():
            cold_root, cold_outside = self.fresh_root("gB23")
            payload = self.hook_payload("capture.17.pre-tool.bash.governed-late.dualkey",
                                        cold_root, cold_outside, "sess-cold")
            code, err, _ = run_hook(hook, "pre_tool", cold_root, payload,
                                    tool="Bash", session_handle="h-cold")
            self.expect_deny(R_NO_LISTENER, err, code, source="hook-client")
            self.expect("fail-closed" in err, f"no honest fail-closed text: {err}")

        @self.case("B24.no-host-session-start-advisory", ["INV-5", "INV-14"],
                   "trial-4 audit: registration failure is an ADVISORY (why the "
                   "preflight missed it)", incident="I4")
        def _b24():
            cold_root, cold_outside = self.fresh_root("gB24")
            payload = self.hook_payload("pinned.session-start.startup", cold_root,
                                        cold_outside, "sess-cold2")
            code, err, _ = run_hook(hook, "session_start", cold_root, payload,
                                    session_handle="h-cold2")
            self.expect(code == 0, f"advisory event must exit 0, got {code}: {err}")
            self.expect("session NOT registered" in err,
                        "the trial-4 invisible symptom must at least be VISIBLE text")

        @self.case("B25.deny-rows-journaled", ["INV-8"], "journal audit law")
        def _b25():
            stop_host(b_root, b_proc, b_fh)
            kinds = [kind for kind, _ in audit_events(b_root)]
            self.expect(kinds.count("hook_deny_governed") >= 12,
                        f"hook_deny_governed rows: {kinds.count('hook_deny_governed')}")
            self.expect("hook_deny_unknown_tool" in kinds,
                        "unknown-tool deny not journaled")

        # ===================================================================
        # Group A: allows, correlation, digest binding (INV-4, 6, 7)
        # ===================================================================
        a_root, a_outside = self.fresh_root("gA")
        a_proc, a_fh = boot_host(self.host_exe, a_root, None, self.run_root / "gA-host.log")
        self.live_procs.append(a_proc)
        self.registered(a_root, a_outside, "h-a", "sess-a")
        self.registered(a_root, a_outside, "h-a2", "sess-a2")
        a_sim, a_out = a_root.as_posix(), a_outside.as_posix()

        def outside_write_payload(handle_session: str, mutate=lambda b: b) -> bytes:
            base = self.hook_payload("pinned.pre-tool.write", a_root, a_outside,
                                     handle_session)
            base = base.replace(a_sim.encode(), a_out.encode())
            return mutate(base)

        @self.case("A1.capture-P4-bash-outside-allows", ["INV-4"],
                   "capture.14: the P4 probe must ALLOW (trial-4 denied it via 114)",
                   incident="I4")
        def _a1():
            payload = self.hook_payload("capture.14.pre-tool.bash.outside.dualkey",
                                        a_root, a_outside, "sess-a")
            code, err, _ = run_hook(hook, "pre_tool", a_root, payload,
                                    tool="Bash", session_handle="h-a")
            self.expect(code == 0 and err == "",
                        f"outside-scope Bash must allow silently, got {code}: {err}")
            self.clear_outstanding(a_root, "h-a", "Bash")

        @self.case("A2.pinned-write-outside-allows", ["INV-4"], "profile scope law")
        def _a2():
            code, err, _ = run_hook(hook, "pre_tool", a_root, outside_write_payload("sess-a"),
                                    tool="Write", session_handle="h-a")
            self.expect(code == 0 and err == "", f"outside Write must allow: {err}")

        @self.case("A3.inert-bash-allows", ["INV-4", "INV-13"],
                   "capture.4 (ls listing): no governed reference -> not_governed")
        def _a3():
            payload = self.hook_payload("capture.4.pre-tool.bash.inert.dualkey",
                                        a_root, a_outside, "sess-a2")
            code, err, _ = run_hook(hook, "pre_tool", a_root, payload,
                                    tool="Bash", session_handle="h-a2")
            self.expect(code == 0 and err == "", f"inert bash must allow: {err}")

        @self.case("A4.outstanding-repeat-denied-115", ["INV-7"],
                   "one outstanding pre per tool (correlation law)")
        def _a4():
            code, err, _ = run_hook(hook, "pre_tool", a_root, outside_write_payload("sess-a"),
                                    tool="Write", session_handle="h-a")
            self.expect_deny(R_CORRELATION, err, code, fragment="awaits its PostToolUse")

        @self.case("A5.post-tool-clears-outstanding", ["INV-7", "INV-8"],
                   "PostToolUseHookInput contract; host outcome correlation")
        def _a5():
            payload = self.hook_payload("pinned.post-tool.write", a_root, a_outside,
                                        "sess-a")
            code, err, _ = run_hook(hook, "post_tool", a_root, payload,
                                    tool="Write", session_handle="h-a")
            self.expect(code == 0, f"post_tool exit {code}: {err}")
            code, err, _ = run_hook(hook, "pre_tool", a_root, outside_write_payload("sess-a"),
                                    tool="Write", session_handle="h-a")
            self.expect(code == 0, f"outstanding must clear after post_tool: {err}")
            self.clear_outstanding(a_root, "h-a", "Write")

        @self.case("A6.unmatched-post-indeterminate", ["INV-7", "INV-8"],
                   "unmatched post is Indeterminate, never guessed; the advisory "
                   "exit contract is 0 (a degraded HookAck prints no text - the "
                   "journal row is the observable)")
        def _a6():
            payload = self.hook_payload("pinned.post-tool.write", a_root, a_outside,
                                        "sess-a").replace(b"Write", b"Edit")
            before = [p for kind, p in audit_events(a_root)
                      if kind == "hook_outcome_unmatched"]
            code, err, _ = run_hook(hook, "post_tool", a_root, payload,
                                    tool="Edit", session_handle="h-a")
            self.expect(code == 0, f"advisory event exits 0: {err}")
            after = [p for kind, p in audit_events(a_root)
                     if kind == "hook_outcome_unmatched"]
            self.expect(len(after) == len(before) + 1,
                        "unmatched post must leave its Indeterminate journal row")

        @self.case("A7.unregistered-post-indeterminate", ["INV-7", "INV-14"],
                   "unregistered post: degraded 114 verdict (advisory exit 0; "
                   "the host records Indeterminate without guessing)")
        def _a7():
            payload = self.hook_payload("pinned.post-tool.write", a_root, a_outside,
                                        "sess-ghost")
            code, _err, _ = run_hook(hook, "post_tool", a_root, payload,
                                     tool="Write", session_handle="h-ghost2")
            self.expect(code == 0, "unregistered post is advisory: exit 0")

        @self.case("A8.cross-session-outstanding-isolated", ["INV-7"],
                   "outstanding state is per-session (isolation contract)")
        def _a8():
            code, err, _ = run_hook(hook, "pre_tool", a_root,
                                    outside_write_payload("sess-a2"),
                                    tool="Write", session_handle="h-a2")
            self.expect(code == 0,
                        f"session a2 has its own outstanding budget: {err}")
            self.clear_outstanding(a_root, "h-a2", "Write")

        @self.case("A13.bash-outside-allow-then-post", ["INV-4", "INV-7"],
                   "outside Bash allow + correlated outcome")
        def _a13():
            payload = self.payload_with_target(a_root, a_outside, "sess-a", "Bash",
                                               command=f"echo x > {a_out}/a13.txt")
            code, err, _ = run_hook(hook, "pre_tool", a_root, payload,
                                    tool="Bash", session_handle="h-a")
            self.expect(code == 0 and err == "", f"outside bash must allow: {err}")
            self.clear_outstanding(a_root, "h-a", "Bash")

        @self.case("A14.edit-outside-allow-then-post", ["INV-4", "INV-7"],
                   "outside Edit allow + correlated outcome")
        def _a14():
            payload = self.payload_with_target(a_root, a_outside, "sess-a2", "Edit",
                                               file_path=f"{a_out}/a14.md")
            code, err, _ = run_hook(hook, "pre_tool", a_root, payload,
                                    tool="Edit", session_handle="h-a2")
            self.expect(code == 0 and err == "", f"outside edit must allow: {err}")
            self.clear_outstanding(a_root, "h-a2", "Edit")

        # I2: the digest binds EXACT bytes (trial-2 class) with re-encoding-
        # sensitive payloads; oracle = the transaction row's request_digest.
        def digest_case(case_id: str, mutate) -> None:
            @self.case(case_id, ["INV-6"], "trial-2 audit + journal request_digest law",
                       incident="I2")
            def _digest(cid=case_id, mut=mutate):
                self.clear_outstanding(a_root, "h-a2", "Write")
                payload = outside_write_payload("sess-a2", mut)
                tag = cid.split(".")[0].encode()
                payload = payload.replace(b"probe\\n", b"probe\\n" + tag)
                code, err, _ = run_hook(hook, "pre_tool", a_root, payload,
                                        tool="Write", session_handle="h-a2")
                self.expect(code == 0, f"digest case pre_tool failed: {err}")
                self.clear_outstanding(a_root, "h-a2", "Write")
                payload_sha = hashlib.sha256(payload).hexdigest()
                file_path = json.loads(payload)["toolInput"]["file_path"]
                expected = hashlib.sha256(
                    f"{payload_sha}|Write||{file_path}".encode("utf-8")).digest()
                rows = journal_rows(a_root,
                                    "SELECT request_digest FROM transactions "
                                    "ORDER BY id DESC LIMIT 6")
                self.expect(any(bytes(row[0]) == expected for row in rows),
                            "no transaction row binds the EXACT stdin bytes "
                            "(a re-serialized digest would mismatch)")

        digest_case("A9.I2-digest-exact-bytes", lambda b: b)
        digest_case("A10.I2-digest-whitespace-form",
                    lambda b: b.replace(b"{\"cwd\"", b"{ \"cwd\"  ", 1))
        digest_case("A11.I2-digest-unicode-escape-form",
                    lambda b: b[:-1] + b',"note":"\\u4e2d\\u6587"}')

        @self.case("A12.admit-rows-journaled", ["INV-8"], "journal audit law")
        def _a12():
            stop_host(a_root, a_proc, a_fh)
            kinds = [kind for kind, _ in audit_events(a_root)]
            self.expect("hook_admit" in kinds, "allowed action not journaled")
            self.expect("hook_outcome" in kinds, "correlated outcome not journaled")

        # ===================================================================
        # Group C: shutdown / restart / duplicate boot (INV-16, 17)
        # ===================================================================
        c_root, c_outside = self.fresh_root("gC")
        c_proc, c_fh = boot_host(self.host_exe, c_root, None, self.run_root / "gC-host.log")
        self.live_procs.append(c_proc)
        self.registered(c_root, c_outside, "h-c", "sess-c")
        c_state: dict = {}

        @self.case("C1.duplicate-boot-fails-closed", ["INV-16"],
                   "singleton mutex law (one host per installation)")
        def _c1():
            proc = subprocess.run([str(self.host_exe), "--root", str(c_root)],
                                  capture_output=True, timeout=HOST_BOOT_BOUND_S,
                                  creationflags=CREATE_NO_WINDOW)
            self.expect(proc.returncode == 1,
                        f"second host on the same root must fail boot "
                        f"(got {proc.returncode})")

        @self.case("C2.authenticated-shutdown-exits", ["INV-16"],
                   "shutdown ack precedes drain; the PROCESS exits (M3 law)")
        def _c2():
            c_state["pre_rows"] = len(audit_events(c_root))
            stop_host(c_root, c_proc, c_fh)  # raises unless ack + real exit

        @self.case("C3.old-session-dead-after-restart", ["INV-17"],
                   "in-memory sessions die with the host; journal survives")
        def _c3():
            proc, fh = boot_host(self.host_exe, c_root, None,
                                 self.run_root / "gC-host2.log")
            self.live_procs.append(proc)
            try:
                payload = self.hook_payload("pinned.pre-tool.write", c_root,
                                            c_outside, "sess-c")
                code, err, _ = run_hook(hook, "pre_tool", c_root, payload,
                                        tool="Write", session_handle="h-c")
                self.expect_deny(R_UNKNOWN_SESSION, err, code)
                rows = len(audit_events(c_root))
                self.expect(rows >= c_state["pre_rows"],
                            "journal rows must survive the restart")
            finally:
                stop_host(c_root, proc, fh)

        @self.case("C4.re-register-after-restart-idempotent", ["INV-1", "INV-17"],
                   "re-registration after restart; idempotent repeat (the repeat "
                   "may answer silently - the idempotent branch computes a "
                   "window-current refresh; identity stability rides the "
                   "exactly-one-row law)")
        def _c4():
            proc, fh = boot_host(self.host_exe, c_root, None,
                                 self.run_root / "gC-host3.log")
            self.live_procs.append(proc)
            try:
                self.registered(c_root, c_outside, "h-c2", "sess-c2")
                self.registered(c_root, c_outside, "h-c2", "sess-c2",
                                require_note=False)
                rows = [p for kind, p in audit_events(c_root)
                        if kind == "session_registered" and p.endswith("|h-c2")]
                self.expect(len(rows) == 1,
                            f"h-c2 registered {len(rows)} times (want exactly 1)")
            finally:
                stop_host(c_root, proc, fh)

        # ===================================================================
        # Group D/I5: packaging (INV-11)
        # ===================================================================
        @self.case("D1.kit-entrypoint-boots-and-registers", ["INV-11"],
                   "ADR-0049 kit self-containment (assemble_kit + kit-internal profile)")
        def _d1():
            sys.path.insert(0, str(REPO_ROOT / "tools"))
            import h1_kit
            kit_root = self.run_root / "kits"
            stub_receipt = self.run_root / "stub-receipt.json"
            stub_receipt.write_text("{}\n", encoding="utf-8")
            saved_root, saved_config = h1_kit.REPO_ROOT, h1_kit.WORKSPACE_CONFIG
            try:
                h1_kit.REPO_ROOT = REPO_ROOT
                h1_kit.WORKSPACE_CONFIG = self.run_root / "no-workspace-config.json"
                rc = h1_kit.assemble_kit(kit_root, "0" * 40, stub_receipt, "local",
                                         "sim-token")
            finally:
                h1_kit.REPO_ROOT, h1_kit.WORKSPACE_CONFIG = saved_root, saved_config
            self.expect(rc == EXIT_OK, "assemble_kit failed")
            kit_dir = kit_root / "qiven-runtime" / "mvp4-h1" / "0.1.0-g00000000"
            kit_profile = h1_kit.kit_profile(kit_dir)
            self.expect(kit_profile.is_file(), "kit profile missing")
            d_root, d_outside = self.fresh_root("gD1", with_profile=False)
            proc, fh = boot_host(self.host_exe, d_root, kit_profile,
                                 self.run_root / "gD1-host.log", cwd=kit_dir)
            self.live_procs.append(proc)
            try:
                code, err, _ = run_hook(hook, "session_start", d_root,
                                        self.hook_payload("pinned.session-start.startup",
                                                          d_root, d_outside, "sess-d1"),
                                        session_handle="h-d1")
                self.expect(code == 0 and "session registered (id" in err,
                            f"kit-form boot must register: {err}")
            finally:
                stop_host(d_root, proc, fh)

        @self.case("I5a.stripped-kit-fails-closed-root-named", ["INV-11"],
                   "2026-09-24 kit-preflight incident (old-fail fault injection)",
                   incident="I5")
        def _i5a():
            bare_root, _ = self.fresh_root("gI5a", with_profile=False)
            cwd_with_profile = REPO_ROOT  # this checkout HAS config/profiles
            proc = subprocess.run([str(self.host_exe), "--root", str(bare_root)],
                                  capture_output=True, timeout=HOST_BOOT_BOUND_S,
                                  creationflags=CREATE_NO_WINDOW,
                                  cwd=str(cwd_with_profile))
            out = (proc.stdout + proc.stderr).decode("utf-8", errors="replace")
            self.expect(proc.returncode == 2,
                        f"bare root must fail closed (got {proc.returncode})")
            self.expect("default profile not found" in out,
                        f"must name the missing profile: {out}")
            self.expect(str(bare_root) in out,
                        f"must name the ROOT-derived path: {out}")
            self.expect(str(cwd_with_profile / "config" / "profiles") not in out,
                        "a CWD profile must NEVER satisfy the root-derived default")

        @self.case("I5b.explicit-kit-profile-cwd-independent", ["INV-11"],
                   "the corrective form: --profile overrides CWD everywhere",
                   incident="I5")
        def _i5b():
            kit_profile = self.run_root / "kits" / "qiven-runtime" / "mvp4-h1" / \
                "0.1.0-g00000000" / "config" / "profiles" / PROFILE_NAME
            self.expect(kit_profile.is_file(), "kit profile must exist (D1 built it)")
            d_root, d_outside = self.fresh_root("gI5b", with_profile=False)
            trap_cwd = self.run_root / "gS"  # a directory that HAS its own profile
            proc, fh = boot_host(self.host_exe, d_root, kit_profile,
                                 self.run_root / "gI5b-host.log", cwd=trap_cwd)
            self.live_procs.append(proc)
            try:
                code, err, _ = run_hook(hook, "session_start", d_root,
                                        self.hook_payload("pinned.session-start.startup",
                                                          d_root, d_outside, "sess-i5b"),
                                        session_handle="h-i5b")
                self.expect(code == 0 and "session registered (id" in err,
                            f"explicit kit profile must be CWD-independent: {err}")
            finally:
                stop_host(d_root, proc, fh)

        # ===================================================================
        # Group P: path forms (INV-3, 4, 13) - pinned-contract shapes
        # ===================================================================
        p_root, p_outside = self.fresh_root("gP")
        p_proc, p_fh = boot_host(self.host_exe, p_root, None,
                                 self.run_root / "gP-host.log")
        self.live_procs.append(p_proc)
        self.registered(p_root, p_outside, "h-p", "sess-p")
        p_sim, p_out = p_root.as_posix(), p_outside.as_posix()

        path_cases = [
            ("P1.backslash-governed", "Write", "{SIM_ROOT_BSLASH}\\state\\current.md",
             "deny110", None),
            ("P2.case-variant-governed", "Write", "{SIM_ROOT}/STATE/CURRENT.MD",
             "deny110", None),
            ("P3.relative-governed-form", "Write", "state/current.md", "deny110", None),
            ("P4.trailing-slash-dir-not-governed", "Write", "{SIM_ROOT}/state/",
             "allow", "dir-form is not a governed path (lexical policy)"),
            ("P5.unicode-inside-governed", "Write",
             "{SIM_ROOT}/memory/records/\u8bb0\u5f55.md", "deny110", None),
            ("P6.unicode-outside", "Write",
             "{OUTSIDE_ROOT}/\u65e5\u672c\u8a9e/\u30d5\u30a1\u30a4\u30eb.md", "allow", None),
            ("P7.mixed-separator-governed", "Edit",
             "{SIM_ROOT}/obligations\\index.yaml", "deny110", None),
            ("P8.dot-segment-lexical-gap", "Write",
             "{SIM_ROOT}/memory/./records/x.md", "allow",
             "lexical dot-segment does not match the governed path - DETECTOR LIMIT recorded"),
            ("P9.backslash-edit-governed", "Edit",
             "{SIM_ROOT_BSLASH}\\sessions\\2026.md", "deny110", None),
            ("P10.case-variant-edit", "Edit", "{SIM_ROOT}/OBLIGATIONS/x.md",
             "deny110", None),
            ("P11.relative-edit-form", "Edit", "sessions/x.md", "deny110", None),
            ("P12.unicode-inside-edit", "Edit",
             "{SIM_ROOT}/obligations/\u4e2d\u6587.md", "deny110", None),
            ("P13.trailing-slash-edit-dir", "Edit", "{SIM_ROOT}/sessions/",
             "deny110", "the governed DIRECTORY itself is inside the scope "
             "(conservative: trailing slash trims to the dir entry)"),
            ("P14.forward-mix-outside-edit", "Edit", "{OUTSIDE_ROOT}/sub/x.md",
             "allow", None),
            ("P18.double-backslash-lexical-gap", "Write",
             "{SIM_ROOT_BSLASH}\\\\state\\\\current.md", "allow",
             "double separators normalize to empty segments - no governed substring "
             "- DETECTOR LIMIT recorded"),
            ("P19.unicode-outside-edit", "Edit",
             "{OUTSIDE_ROOT}/\u56fd/\u4e2d.md", "allow", None),
        ]
        for case_id, tool, target_form, expectation, note in path_cases:
            @self.case(case_id, ["INV-3", "INV-4", "INV-13"],
                       f"path-form policy ({target_form}); {note or 'lexical policy'}")
            def _path(tool_name=tool, form=target_form, want=expectation, cid=case_id,
                      case_note=note):
                target = form.replace("{SIM_ROOT_BSLASH}", str(p_root)) \
                             .replace("{SIM_ROOT}", p_sim) \
                             .replace("{OUTSIDE_ROOT}", p_out)
                payload = self.payload_with_target(p_root, p_outside, "sess-p",
                                                   tool_name, file_path=target)
                target_path = Path(target)
                before = (target_path.read_bytes()
                          if target_path.is_file() else None)
                code, err, _ = run_hook(hook, "pre_tool", p_root, payload,
                                        tool=tool_name, session_handle="h-p")
                if want == "deny110":
                    self.expect_deny(R_GOVERNED_WRITE, err, code)
                    after = (target_path.read_bytes()
                             if target_path.is_file() else None)
                    self.expect(after == before, "deny executed an effect")
                else:
                    self.expect(code == 0 and err == "",
                                f"expected lexical allow, got {code}: {err}")
                    if case_note and "DETECTOR LIMIT" in case_note:
                        self.detector_limits.append(cid)
                    self.clear_outstanding(p_root, "h-p", tool_name)

        @self.case("P15.junction-reparse-lexical-gap", ["INV-3", "INV-4"],
                   "reparse indirection outside lexical scope - DETECTOR LIMIT recorded "
                   "(production architecture demands OS-backed checks; not claimed here)")
        def _p15():
            junction = p_outside / "jg"
            if junction.exists():
                shutil.rmtree(junction, ignore_errors=True)
            proc = subprocess.run(
                ["cmd", "/c", "mklink", "/J", str(junction),
                 str(p_root / "memory" / "records")],
                capture_output=True, timeout=15, creationflags=CREATE_NO_WINDOW)
            self.expect(proc.returncode == 0,
                        f"junction creation failed: "
                        f"{proc.stderr.decode(errors='replace')}")
            target = (junction / "via-junction.md").as_posix()
            payload = self.payload_with_target(p_root, p_outside, "sess-p", "Write",
                                               file_path=target)
            code, err, _ = run_hook(hook, "pre_tool", p_root, payload,
                                    tool="Write", session_handle="h-p")
            self.expect(code == 0 and err == "",
                        f"junction form is outside the LEXICAL detector "
                        f"(expected allow): {err}")
            self.detector_limits.append("P15.junction-reparse-lexical-gap")
            self.clear_outstanding(p_root, "h-p", "Write")

        @self.case("P16.bash-unicode-command-governed", ["INV-13"],
                   "conservative detector over-approximates unicode text too")
        def _p16():
            command = f"echo \u4e2d > {p_sim}/sessions/\u4e2d.md"
            payload = self.payload_with_target(p_root, p_outside, "sess-p", "Bash",
                                               command=command)
            code, err, _ = run_hook(hook, "pre_tool", p_root, payload,
                                    tool="Bash", session_handle="h-p")
            self.expect_deny(R_BASH_REFERENCE, err, code)

        @self.case("P17.bash-backslash-name-still-caught", ["INV-13"],
                   "the Bash detector matches the bare governed NAME anywhere in "
                   "the text (contains_ci on the path token itself), so a "
                   "backslash form still denies - conservative over-approximation "
                   "as documented in the profile honesty note")
        def _p17():
            command = f"echo x > {str(p_root)}\\sessions\\\\via-bs.md"
            payload = self.payload_with_target(p_root, p_outside, "sess-p", "Bash",
                                               command=command)
            code, err, _ = run_hook(hook, "pre_tool", p_root, payload,
                                    tool="Bash", session_handle="h-p")
            self.expect_deny(R_BASH_REFERENCE, err, code,
                             fragment="references governed scope")

        @self.case("P21.astral-character-payload-denied-118", ["INV-2"],
                   "extractor law: \\uXXXX surrogate halves are unreadable at this "
                   "grade (bounded first-party parser) - an emoji-bearing file_path "
                   "fails closed client-side")
        def _p21():
            payload = self.payload_with_target(p_root, p_outside, "sess-p", "Write",
                                               file_path=f"{p_out}/\U0001F600/x.md")
            code, err, _ = run_hook(hook, "pre_tool", p_root, payload,
                                    tool="Write", session_handle="h-p")
            self.expect_deny(R_PAYLOAD, err, code, source="hook-client")

        @self.case("P20.path-series-teardown", ["INV-8"], "journal integrity after P")
        def _p20():
            stop_host(p_root, p_proc, p_fh)
            kinds = [kind for kind, _ in audit_events(p_root)]
            self.expect("session_registered" in kinds, "registration row missing")

        # ===================================================================
        # Group W: wire faults (I3/I4) + admission + bypass controls
        # ===================================================================
        w_root, w_outside = self.fresh_root("gW")
        w_proc, w_fh = boot_host(self.host_exe, w_root, None,
                                 self.run_root / "gW-host.log")
        self.live_procs.append(w_proc)
        self.registered(w_root, w_outside, "h-w", "sess-w")

        @self.case("I3a.admission-fault-denied-121", ["INV-12"],
                   "deny-116 audit addendum: install-record admission (fault: hook "
                   "image outside the host's install directory at boot)",
                   incident="I3")
        def _i3a():
            elsewhere = self.run_root / "elsewhere"
            elsewhere.mkdir(exist_ok=True)
            stray = elsewhere / "qiven-zcode-hook.exe"
            shutil.copy2(hook, stray)
            payload = self.hook_payload("pinned.pre-tool.write", w_root, w_outside,
                                        "sess-w")
            code, err, _ = run_hook(stray, "pre_tool", w_root, payload,
                                    tool="Write", session_handle="h-w")
            self.expect_deny(R_ADMISSION, err, code, fragment="admission rejected")

        @self.case("I3b.wire-seq-regression-typed-63", ["INV-9"],
                   "per-connection sequence law", incident="I3")
        def _i3b():
            client = WireClient(w_root / ".qiven" / "runtime")
            try:
                reply = client.hello(connection_seq=5)
                self.expect(reply.get("kind") == "hello_ack",
                            f"hello rejected: {reply}")
                body = {"kind": "hook_event", "event": "pre_tool",
                        "session_handle": "h-w", "tool_name": "Write",
                        "payload_sha256": "00" * 32, "payload_bytes": 2,
                        "command": "", "file_path": "", "mediated_tools": "",
                        "request_id": 2, "deadline_ms": 3000}
                client.send(hmac_frame(client.key,
                                       json.dumps(body,
                                                  separators=(",", ":")).encode(),
                                       2, 5))  # seq regression: 5, not > 5
                reply = client.read_reply()
                self.expect(reply.get("kind") == "error"
                            and reply["error"]["code"] == ERR_REPLAY,
                            f"expected typed {ERR_REPLAY}, got: {reply}")
                self.expect("sequence regression" in reply["error"]["detail"],
                            f"detail must name the class: {reply}")
            finally:
                client.close()

        @self.case("I3c.wire-bad-mac-typed-62", ["INV-9"], "HMAC verification law")
        def _i3c():
            client = WireClient(w_root / ".qiven" / "runtime")
            try:
                client.send(hmac_frame(
                    client.key,
                    b'{"kind":"status","request_id":1,"deadline_ms":1000}',
                    1, 1, corrupt_mac=True))
                reply = client.read_reply()
                self.expect(reply.get("kind") == "error"
                            and reply["error"]["code"] == ERR_AUTH
                            and "HMAC" in reply["error"]["detail"],
                            f"expected typed {ERR_AUTH} HMAC failure, got: {reply}")
            finally:
                client.close()

        @self.case("I3d.wire-bad-magic-typed-64", ["INV-9"], "frame magic law")
        def _i3d():
            client = WireClient(w_root / ".qiven" / "runtime")
            try:
                client.send(hmac_frame(
                    client.key,
                    b'{"kind":"status","request_id":1,"deadline_ms":1000}',
                    1, 1, magic=0xDEADBEEF))
                reply = client.read_reply()
                self.expect(reply.get("kind") == "error"
                            and reply["error"]["code"] == ERR_FRAME
                            and "magic" in reply["error"]["detail"],
                            f"expected typed {ERR_FRAME} magic failure, got: {reply}")
            finally:
                client.close()

        @self.case("I3e.wire-unknown-field-typed-64", ["INV-9"],
                   "closed vocabulary law (protocol decode_request)")
        def _i3e():
            client = WireClient(w_root / ".qiven" / "runtime")
            try:
                client.send(hmac_frame(
                    client.key,
                    b'{"kind":"status","request_id":1,"deadline_ms":1000,'
                    b'"surprise":"x"}', 1, 1))
                reply = client.read_reply()
                self.expect(reply.get("kind") == "error"
                            and "unknown request field" in reply["error"]["detail"],
                            f"closed vocabulary must reject unknown fields: {reply}")
            finally:
                client.close()

        @self.case("I3g.wire-version-skew-typed-64", ["INV-9"],
                   "protocol version law")
        def _i3g():
            client = WireClient(w_root / ".qiven" / "runtime")
            try:
                client.send(hmac_frame(
                    client.key,
                    b'{"kind":"status","request_id":1,"deadline_ms":1000}',
                    1, 1, proto=99))
                reply = client.read_reply()
                self.expect(reply.get("kind") == "error"
                            and "unsupported protocol version" in
                            reply["error"]["detail"],
                            f"version skew must be typed: {reply}")
            finally:
                client.close()

        @self.case("I4a.wire-hello-refresh-deadline-typed-64", ["INV-9", "INV-10"],
                   "trial-4 mechanism, wire level: hello ceiling is 5000 even when "
                   "the session_start event budget is 9750", incident="I4")
        def _i4a():
            client = WireClient(w_root / ".qiven" / "runtime")
            try:
                reply = client.hello(deadline_ms=9750)
                self.expect(reply.get("kind") == "error"
                            and reply["error"]["code"] == ERR_FRAME
                            and "deadline_ms must be in (0, 5000]" in
                            reply["error"]["detail"],
                            f"the hello frame must reject a refresh-grade "
                            f"deadline: {reply}")
            finally:
                client.close()

        @self.case("I3f.wire-frame-budget-typed-64", ["INV-9"],
                   "per-connection frame budget (DoS bound)")
        def _i3f():
            client = WireClient(w_root / ".qiven" / "runtime")
            try:
                reply = client.hello(connection_seq=1)
                self.expect(reply.get("kind") == "hello_ack",
                            f"hello rejected: {reply}")
                exceeded = False
                for seq in range(2, 80):
                    client.send(hmac_frame(
                        client.key,
                        b'{"kind":"status","request_id":' + str(seq).encode() +
                        b',"deadline_ms":1000}', seq, seq))
                    reply = client.read_reply()
                    if reply.get("kind") == "error":
                        self.expect("frame budget" in reply["error"]["detail"],
                                    f"expected budget denial, got: {reply}")
                        exceeded = True
                        break
                self.expect(exceeded, "frame budget (64) was never enforced")
            finally:
                client.close()

        # W: the writable-child/delegation bypass negative controls.
        @self.case("W1.unmediated-direct-write-bypass", ["INV-18"],
                   "negative control: a non-mediated process writes inside the "
                   "governed root and NOTHING stops it (pinned source: subagents "
                   "carry no hook runner)")
        def _w1():
            target = w_root / "memory" / "records" / "BYPASS-DIRECT.md"
            target.write_text("bypass\n", encoding="utf-8")
            self.expect(target.exists(),
                        "the direct write must succeed - this gap is REAL")
            mentions = [p for kind, p in audit_events(w_root) if "BYPASS-DIRECT" in p]
            self.expect(not mentions,
                        "the journal must show NO mediation for the bypass write")

        @self.case("W2.unmediated-child-process-bypass", ["INV-18"],
                   "negative control: a spawned child process (subagent shape) "
                   "writes inside the governed root with no hook in between")
        def _w2():
            target = w_root / "obligations" / "BYPASS-CHILD.md"
            proc = subprocess.run(
                [sys.executable, "-c",
                 "import pathlib,sys;"
                 "pathlib.Path(sys.argv[1]).write_text('child bypass\\n')",
                 str(target)],
                capture_output=True, timeout=20, creationflags=CREATE_NO_WINDOW)
            self.expect(proc.returncode == 0 and target.exists(),
                        f"child write failed unexpectedly: "
                        f"{proc.stderr.decode(errors='replace')}")
            mentions = [p for kind, p in audit_events(w_root) if "BYPASS-CHILD" in p]
            self.expect(not mentions,
                        "the journal must show NO mediation for the child write")

        @self.case("W3.wire-group-teardown", ["INV-16"], "shutdown after wire faults")
        def _w3():
            stop_host(w_root, w_proc, w_fh)

        # ===================================================================
        # Group X: independent-root breadth + no-host forms
        # ===================================================================
        x_root, x_outside = self.fresh_root("gX")
        x_proc, x_fh = boot_host(self.host_exe, x_root, None,
                                 self.run_root / "gX-host.log")
        self.live_procs.append(x_proc)
        for idx, template_id in enumerate(
                ("capture.0.session-start.resume.dualkey",
                 "capture.1.session-start.startup.dualkey")):
            row_token = template_id.split(".")[1]
            @self.case(f"X{idx + 1}.capture-row{row_token}-fresh-root",
                       ["INV-1"], f"{template_id} on an independent root")
            def _x(template=template_id, handle=f"h-x{idx}"):
                payload = self.hook_payload(template, x_root, x_outside,
                                            f"sess-{handle}")
                code, err, _ = run_hook(hook, "session_start", x_root, payload,
                                        session_handle=handle)
                self.expect(code == 0 and "session registered (id" in err, err)

        @self.case("X3.capture-inert-bash-outside-root", ["INV-4", "INV-13"],
                   "capture.4 with substitutions: no governed reference -> not_governed")
        def _x3():
            payload = self.hook_payload("capture.4.pre-tool.bash.inert.dualkey",
                                        x_root, x_outside, "sess-x0")
            code, err, _ = run_hook(hook, "pre_tool", x_root, payload,
                                    tool="Bash", session_handle="h-x0")
            self.expect(code == 0 and err == "",
                        f"inert listing must allow (placeholder cwd is not the "
                        f"root): {err}")
            self.clear_outstanding(x_root, "h-x0", "Bash")

        @self.case("X4.capture-P4-outside-allow-fresh-root", ["INV-4"],
                   "capture.14 on an independent root: the P4 allow")
        def _x4():
            self.registered(x_root, x_outside, "h-x1b", "sess-x1b")
            payload = self.hook_payload("capture.14.pre-tool.bash.outside.dualkey",
                                        x_root, x_outside, "sess-x1b")
            code, err, _ = run_hook(hook, "pre_tool", x_root, payload,
                                    tool="Bash", session_handle="h-x1b")
            self.expect(code == 0 and err == "",
                        f"outside bash must allow on a fresh root: {err}")
            self.clear_outstanding(x_root, "h-x1b", "Bash")

        @self.case("X5.x-group-teardown", ["INV-16"], "shutdown after X series")
        def _x5():
            stop_host(x_root, x_proc, x_fh)

    # -- execution ----------------------------------------------------------

    def execute(self) -> dict:
        self.build_scenarios()
        self.run_root.mkdir(parents=True, exist_ok=True)
        results = []
        failed = 0
        print(f"[ RUN] h1-sim: {len(self.cases)} scenarios, run root {self.run_root}",
              flush=True)
        started = time.monotonic()
        try:
            for entry in self.cases:
                case_id = entry["id"]
                print(f"[ RUN] {case_id}", flush=True)
                t0 = time.monotonic()
                try:
                    entry["run"]()
                    results.append({"id": case_id, "result": "pass",
                                    "invariants": entry["invariants"],
                                    "oracle": entry["oracle"],
                                    "seconds": round(time.monotonic() - t0, 3)})
                    print(f"[ OK ] {case_id}", flush=True)
                except (AssertionError, GateFailure, OSError, ConnectionError,
                        subprocess.SubprocessError, sqlite3.Error,
                        json.JSONDecodeError, struct.error) as failure:
                    detail = f"{type(failure).__name__}: {failure}"
                    results.append({"id": case_id, "result": "FAIL", "detail": detail,
                                    "invariants": entry["invariants"],
                                    "oracle": entry["oracle"],
                                    "seconds": round(time.monotonic() - t0, 3)})
                    failed += 1
                    print(f"[FAIL] {case_id}: {detail}", flush=True)
        finally:
            # Leak guard: every booted host dies with the rig, pass or fail.
            for proc in self.live_procs:
                if proc.poll() is None:
                    proc.kill()
        elapsed = time.monotonic() - started
        verdict = "SIMULATED_HOOK_HOST_PASS" if failed == 0 else "SIMULATED_HOOK_HOST_FAIL"
        if len(results) != len(self.cases):
            verdict = "NOT_VALIDATED"
        return {"verdict": verdict, "results": results, "failed": failed,
                "skipped": 0, "elapsed_s": round(elapsed, 1),
                "total": len(self.cases)}

    # -- receipt -------------------------------------------------------------

    def source_graph(self) -> dict:
        return {
            "runtime_head": subprocess.run(
                ["git", "-C", str(REPO_ROOT), "rev-parse", "HEAD"],
                capture_output=True, text=True, check=False).stdout.strip(),
            "tree_clean": not subprocess.run(
                ["git", "-C", str(REPO_ROOT), "status", "--short"],
                capture_output=True, text=True, check=False).stdout.strip(),
            "exe_digests": {name: sha256_file(self.bin / name) for name in (
                "qiven-runtime-host.exe", "qiven-zcode-hook.exe",
                "qiven-runtimectl.exe")},
            "profile_digest": sha256_file(
                REPO_ROOT / "config" / "profiles" / PROFILE_NAME),
            "fixture_catalogue_digest": sha256_file(FIXTURES),
            "dependencies_manifest_digest": sha256_file(
                REPO_ROOT / ".qiven" / "dependencies.json"),
            "python": sys.version.split()[0],
            "platform": sys.platform,
        }

    def write_receipt(self, outcome: dict, argv: list[str], dev: bool) -> Path:
        RECEIPTS.mkdir(parents=True, exist_ok=True)
        graph = self.source_graph()
        head12 = graph["runtime_head"][:12] or "unknown"
        receipt = {
            "schema": "qiven-h1-sim-receipt-v1",
            "command": " ".join(argv),
            "development": dev,
            "head": graph["runtime_head"],
            "tree_clean": graph["tree_clean"],
            "generated_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "source_graph": graph,
            "case_count": outcome["total"],
            "case_results": outcome["results"],
            "failed": outcome["failed"],
            "skipped": outcome["skipped"],
            "elapsed_s": outcome["elapsed_s"],
            "invariant_coverage": self.coverage,
            "incident_coverage": self.incident_map,
            "detector_limits_recorded": sorted(set(self.detector_limits)),
            "typed_states": {
                "SIMULATED_HOOK_HOST_PASS":
                    outcome["verdict"] == "SIMULATED_HOOK_HOST_PASS",
                "PINNED_SOURCE_CONTRACT_REVIEWED": True,
                "INSTALLED_DESKTOP_EXECUTION_UNVERIFIED": True,
            },
            "complete_mediation_scope": (
                "claim scoped to the mediated tuple (Bash/Write/Edit through the "
                "hook); delegation paths are NOT mediated (W1/W2 negative "
                "controls; pinned zai-org/ZCode@29628c9 subagent construction "
                "wires no hooks)"),
            "verdict": outcome["verdict"],
        }
        path = RECEIPTS / (
            f"sim-{head12}{'-dev' if dev else ''}-"
            f"{time.strftime('%Y%m%dT%H%M%SZ', time.gmtime())}.json")
        path.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n",
                        encoding="utf-8", newline="\n")
        return path

    def verify_receipt(self, path: Path) -> int:
        receipt = json.loads(path.read_text(encoding="utf-8"))
        graph = self.source_graph()
        problems = []
        if receipt.get("development"):
            problems.append("receipt is a DEVELOPMENT receipt (dirty tree) - "
                            "not gate-grade")
        if not receipt.get("tree_clean"):
            problems.append("receipt was taken on a dirty tree")
        if receipt.get("verdict") != "SIMULATED_HOOK_HOST_PASS":
            problems.append(f"receipt verdict is {receipt.get('verdict')}")
        if receipt.get("skipped"):
            problems.append("receipt records skipped scenarios (acceptance-fatal)")
        if receipt.get("failed"):
            problems.append(f"receipt records {receipt['failed']} failed cases")
        for key in ("exe_digests", "profile_digest", "fixture_catalogue_digest",
                    "dependencies_manifest_digest"):
            if receipt.get("source_graph", {}).get(key) != graph.get(key):
                problems.append(f"STALE receipt: {key} differs from the live tree")
        if receipt.get("head") != graph["runtime_head"]:
            problems.append("STALE receipt: head differs from the live HEAD")
        if problems:
            for problem in problems:
                print(f"[FAIL] {problem}")
            print("[FAIL] receipt NOT_VALIDATED")
            return EXIT_FAIL
        print(f"[ OK ] receipt {path.name} is gate-grade at head "
              f"{graph['runtime_head'][:12]} ({receipt['case_count']} cases, "
              f"0 failed, 0 skipped)")
        return EXIT_OK


# ---------------------------------------------------------------------------


def stop_host_quiet(proc, fh, root: Path) -> None:
    try:
        client = WireClient(root / ".qiven" / "runtime")
        client.request({"kind": "shutdown", "grace_ms": 1000, "request_id": 99,
                        "deadline_ms": 3000}, 99, 1)
        client.close()
    except Exception:  # noqa: BLE001 - best-effort cleanup on an old-fail path
        pass
    deadline = time.monotonic() + 8.0
    while time.monotonic() < deadline and proc.poll() is None:
        time.sleep(0.1)
    if proc.poll() is None:
        proc.kill()
    fh.close()


def cmd_old_fail_i4(bin_dir: Path) -> int:
    """The I4 old-fail leg against the historical defective binaries:
    session_start registration must fail with the recorded mechanism
    (hello 9750 > 5000 ceiling -> advisory -> no session -> deny 114)."""
    rig = Rig(bin_dir=bin_dir)
    rig.run_root = WORKROOT / f"oldfail-{time.strftime('%Y%m%d-%H%M%S')}"
    root, outside = rig.fresh_root("old-i4")
    log = rig.run_root / "host.log"
    proc, fh = boot_host(rig.host_exe, root, None, log)
    try:
        payload = rig.hook_payload("pinned.session-start.startup", root, outside,
                                   "sess-old")
        code, err, elapsed = run_hook(rig.hook_exe, "session_start", root, payload,
                                      session_handle="h-old")
        registered = code == 0 and "session registered (id" in err
        p2 = rig.hook_payload("pinned.pre-tool.write", root, outside, "sess-old")
        code2, err2, _ = run_hook(rig.hook_exe, "pre_tool", root, p2,
                                  tool="Write", session_handle="h-old")
        denied_114 = code2 == 2 and "deny 114" in err2
        if registered or not denied_114:
            print(f"[FAIL] old-fail expectation NOT met: the binaries registered "
                  f"(registered={registered}, later deny 114={denied_114}). Either "
                  f"the binaries are not the defective revision or the defect was "
                  f"fixed: {err} | {err2}")
            return EXIT_FAIL
        print(f"[ OK ] old-fail reproduced on the pre-fix revision: registration "
              f"failed as recorded (advisory: {err[:120]}...; later probe: "
              f"{err2[:100]}...; {elapsed:.2f}s)")
        return EXIT_OK
    finally:
        stop_host_quiet(proc, fh, root)


def main(argv=None) -> int:
    argv = list(sys.argv if argv is None else argv)
    if sys.platform != "win32":
        print("[NOT_VALIDATED] platform leg skipped: this gate is Windows-only "
              "(named pipes + DPAPI); ADR-0055 decision 5 makes the skip "
              "acceptance-fatal, never green")
        return EXIT_FAIL

    parser = argparse.ArgumentParser(prog="h1_sim_gate.py")
    sub = parser.add_subparsers(dest="mode", required=True)
    run_p = sub.add_parser("run")
    run_p.add_argument("--dev", action="store_true",
                       help="allow a dirty tree; the receipt is marked development "
                            "and verify-receipt rejects it")
    run_p.add_argument("--bin-dir", default=None)
    verify_p = sub.add_parser("verify-receipt")
    verify_p.add_argument("--receipt", default=None)
    old_p = sub.add_parser("old-fail-i4")
    old_p.add_argument("--bin-dir", required=True,
                       help="pre-fix binary directory (the historical defective "
                            "revision): registration MUST FAIL for the recorded "
                            "reason")
    args = parser.parse_args(argv[1:])

    if args.mode == "verify-receipt":
        rig = Rig()
        if args.receipt:
            path = Path(args.receipt)
        elif RECEIPTS.exists():
            candidates = sorted(RECEIPTS.glob("sim-*.json"),
                                key=lambda p: p.stat().st_mtime)
            path = candidates[-1] if candidates else None
        else:
            path = None
        if path is None or not path.exists():
            print("[FAIL] no receipt found - absent receipts are rejected")
            return EXIT_FAIL
        return rig.verify_receipt(path)

    if args.mode == "old-fail-i4":
        return cmd_old_fail_i4(Path(args.bin_dir))

    # run mode
    rig = Rig(bin_dir=Path(args.bin_dir) if args.bin_dir else None)
    if not args.dev and not rig.source_graph()["tree_clean"]:
        print("[FAIL] working tree is not clean - commit first (the receipt binds "
              "an exact validated head) or pass --dev for a development receipt")
        return EXIT_FAIL
    for name in ("qiven-runtime-host.exe", "qiven-zcode-hook.exe"):
        if not (rig.bin / name).exists():
            print(f"[NOT_VALIDATED] candidate executable missing: {rig.bin / name} "
                  "(build first; a missing binary is acceptance-fatal, never a "
                  "skip)")
            return EXIT_FAIL
    outcome = rig.execute()
    receipt_path = rig.write_receipt(outcome, argv, dev=args.dev)
    if outcome["verdict"] == "SIMULATED_HOOK_HOST_PASS":
        print(f"[ OK ] h1-sim PASS: {outcome['total']} cases, 0 failed, 0 skipped "
              f"({outcome['elapsed_s']}s) - receipt {receipt_path}")
        return rig.verify_receipt(receipt_path)
    print(f"[FAIL] h1-sim {outcome['verdict']}: {outcome['failed']} failed, "
          f"{outcome['skipped']} skipped of {outcome['total']} - receipt "
          f"{receipt_path}")
    return EXIT_FAIL


if __name__ == "__main__":
    raise SystemExit(main())
