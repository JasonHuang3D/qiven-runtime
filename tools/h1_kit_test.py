#!/usr/bin/env python3
"""qiven h1_kit_test.py - focused regressions for the H1 kit builder and
the runtime host's default-profile resolution (MVP-4 corrective lane,
2026-09-24 preflight incident: the kit shipped without config/profiles,
and runtime_host_main derived its default profile from the CWD instead of
the --root it was given).

Regressions:
  1. Kit self-containment (ADR-0049): a kit assembled by
     h1_kit.assemble_kit contains config/profiles/<profile>, its
     manifest.json lists that file with the matching digest, and
     run-host.cmd launches the host with an EXPLICIT --profile that
     resolves inside the kit (no CWD-derived default on the owner-live
     double-click path).
  2. runtime_host_main default-profile derivation follows --root, never
     the process CWD: spawned with a --root that HAS no config/profiles
     while the CWD HAS one, the fixed exe must fail closed (exit 2)
     naming the ROOT-derived profile path -- the incident mechanism.

Run directly (no python test harness exists in this repo yet; standard
library only per the Devkit python-standard law):

  python tools/h1_kit_test.py
"""

from __future__ import annotations

import hashlib
import json
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import h1_kit  # noqa: E402  (same-directory tool module)

REPO_ROOT = Path(__file__).resolve().parent.parent
EXIT_OK, EXIT_FAIL = 0, 1


def check(condition: bool, label: str) -> bool:
    print(("[ OK ] " if condition else "[FAIL] ") + label)
    return condition


def test_kit_self_containment() -> bool:
    """ADR-0049: the kit package carries its deployment profile and every
    generated host launcher passes it explicitly (assemble_kit is driven
    directly because build_kit's clean-tree precondition can never hold on
    a development tree running this test)."""
    with tempfile.TemporaryDirectory(prefix="qiven-h1-kit-test-") as tmp:
        fake_repo = Path(tmp) / "runtime-checkout"
        release = fake_repo / "build" / "vs2022-x64" / "Release"
        release.mkdir(parents=True)
        for name in ("qiven-runtime-host.exe", "qiven-zcode-hook.exe",
                     "qiven-runtimectl.exe"):
            (release / name).write_bytes(b"dummy-" + name.encode())
        profile_src = fake_repo / "config" / "profiles" / h1_kit.PROFILE_NAME
        profile_src.parent.mkdir(parents=True)
        profile_bytes = b"schema: qiven-deployment-profile-v1\n# kit-test marker\n"
        profile_src.write_bytes(profile_bytes)

        out_root = Path(tmp) / "h1-kits"
        head = "0123456789abcdef" + "0" * 24
        receipt = (fake_repo / ".generated-temp" / "operator" / "receipts"
                   / f"local-{head}.json")
        receipt.parent.mkdir(parents=True)
        receipt.write_text("{}\n")

        saved_root, saved_config = h1_kit.REPO_ROOT, h1_kit.WORKSPACE_CONFIG
        try:
            h1_kit.REPO_ROOT = fake_repo
            h1_kit.WORKSPACE_CONFIG = Path(tmp) / "no-such-workspace-config.json"
            rc = h1_kit.assemble_kit(out_root, head, receipt, "local", "test-token")
        finally:
            h1_kit.REPO_ROOT, h1_kit.WORKSPACE_CONFIG = saved_root, saved_config
        if not check(rc == EXIT_OK, "assemble_kit returns EXIT_OK"):
            return False

        kit_dir = out_root / "qiven-runtime" / "mvp4-h1" / f"0.1.0-g{head[:8]}"
        kit_profile = kit_dir / "config" / "profiles" / h1_kit.PROFILE_NAME
        rel = h1_kit.KIT_PROFILE_RELPATH
        ok = check(kit_profile.is_file(), f"kit contains {rel}")
        if kit_profile.is_file():
            ok &= check(kit_profile.read_bytes() == profile_bytes,
                        "kit profile is byte-identical to the checkout profile")
        manifest = json.loads((kit_dir / "manifest.json").read_text(encoding="utf-8"))
        listed = {entry["path"]: entry["sha256"] for entry in manifest["files"]}
        ok &= check(rel in listed, f"manifest.json lists {rel}")
        if rel in listed and kit_profile.is_file():
            digest = hashlib.sha256(kit_profile.read_bytes()).hexdigest()
            ok &= check(listed[rel] == digest, "manifest digest matches the kit profile")
        run_host = (kit_dir / "run-host.cmd").read_text(encoding="utf-8")
        ok &= check("--profile" in run_host and str(kit_profile) in run_host,
                    "run-host.cmd passes the kit-internal --profile explicitly")
        ok &= check(h1_kit.kit_profile(kit_dir) == kit_profile,
                    "cmd_preflight profile resolution points inside the kit")
        return ok


def test_host_profile_follows_root() -> bool:
    """The 2026-09-24 incident mechanism: --root without --profile. The
    derived default must follow the RESOLVED root; a CWD that happens to
    contain config/profiles must NOT satisfy it."""
    host_exe = REPO_ROOT / "build" / "vs2022-x64" / "Release" / "qiven-runtime-host.exe"
    if not host_exe.exists():
        print(f"[SKIP] {host_exe} not built - build the Release target first "
              "(regression not exercised in this run)")
        return True
    with tempfile.TemporaryDirectory(prefix="qiven-host-profile-test-") as tmp:
        cwd_checkout = Path(tmp) / "cwd-checkout"
        cwd_profile = cwd_checkout / "config" / "profiles" / h1_kit.PROFILE_NAME
        cwd_profile.parent.mkdir(parents=True)
        cwd_profile.write_text("schema: qiven-deployment-profile-v1\n")
        bare_root = Path(tmp) / "governed-root-without-profile"
        bare_root.mkdir()
        proc = subprocess.run(
            [str(host_exe), "--root", str(bare_root)],
            capture_output=True, text=True, timeout=30, cwd=str(cwd_checkout),
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        output = proc.stderr + proc.stdout
        root_derived = str(bare_root / "config" / "profiles" / h1_kit.PROFILE_NAME)
        ok = check(proc.returncode == 2,
                   f"missing derived profile fails closed with exit 2 (got {proc.returncode})")
        ok &= check("default profile not found" in output,
                    "error names the missing derived profile")
        ok &= check(root_derived in output,
                    "the named profile is the ROOT-derived path")
        ok &= check(str(cwd_profile) not in output,
                    "the CWD-derived path is never used")
        return ok


def main() -> int:
    print("[ RUN] h1_kit_test: MVP-4 kit corrective-lane regressions")
    ok = test_kit_self_containment()
    ok = test_host_profile_follows_root() and ok
    if ok:
        print("[ OK ] h1_kit_test")
        return EXIT_OK
    print("[FAIL] h1_kit_test")
    return EXIT_FAIL


if __name__ == "__main__":
    raise SystemExit(main())
