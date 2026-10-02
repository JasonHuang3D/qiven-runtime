from __future__ import annotations

"""WR-6 deploy launcher: the deploy transport resolves the Devkit checkout
through the WORKSPACE BOOTSTRAP identity-check BEFORE any Devkit code runs -
no QIVEN_DEVKIT_ROOT variable and no consumer-local pin; the lock's
qiven-devkit node is the only admitted source (deployment law: qiven-devkit
docs/engineering/deployment.md; launcher shape per the strict R6b WR-6
launcher, converged in repair batch B3). A wrong local Devkit revision (or
an unreadable workspace lock) fails typed here, before deploy_bundle.py
executes."""

import json
import os
import subprocess
import sys
from pathlib import Path

sys.dont_write_bytecode = True

TARGET_ROOT = Path(__file__).resolve().parents[1]
DEVKIT_CHECKOUT = Path(os.environ.get("QIVEN_DEVKIT_CHECKOUT", TARGET_ROOT.parent / "qiven-devkit"))


def _bootstrap_identity() -> None:
    """Run the workspace bootstrap's Devkit identity-check (its own
    preflight path) and fail typed on any mismatch. The receipt's notes
    (e.g. a dirty devkit checkout label in shadow mode) are SURFACED - a
    swallowed label would let uncommitted deploy code execute silently."""
    control = Path(os.environ.get("QIVEN_WORKSPACE_CONTROL",
                                  TARGET_ROOT.parent / "qiven-workspace")).resolve()
    bootstrap = control / "bootstrap" / "qiven-bootstrap.py"
    if not bootstrap.is_file():
        raise SystemExit(
            f"[FAIL] workspace bootstrap not found at {bootstrap}; set "
            "QIVEN_WORKSPACE_CONTROL to the control checkout (the Devkit "
            "revision is the lock's qiven-devkit node - WR-6, no local pin)."
        )
    completed = subprocess.run(
        [sys.executable, str(bootstrap), "preflight",
         "--control", str(control), "--devkit", str(DEVKIT_CHECKOUT)],
        capture_output=True, text=True, timeout=180,
    )
    if completed.returncode != 0:
        sys.stdout.write(completed.stdout)
        sys.stderr.write(completed.stderr)
        raise SystemExit(
            "[FAIL] workspace bootstrap rejected the local Devkit (WR-6: "
            "wrong local Devkit revision fails before deploy executes)"
        )
    try:
        receipt = json.loads(completed.stdout)
    except json.JSONDecodeError as exc:
        # P0 repair R6b (B3): the receipt IS the WR-6 identity evidence. An
        # unreadable receipt must fail typed BEFORE any deploy code runs -
        # the former warn-and-continue let an unverified identity ride
        # straight into deploy_bundle.py.
        raise SystemExit(
            "[FAIL] preflight receipt unreadable (required WR-6 identity "
            f"evidence); refusing deploy execution: {exc}"
        )
    if not isinstance(receipt, dict):
        raise SystemExit(
            "[FAIL] preflight receipt is not an object (required WR-6 "
            f"identity evidence); refusing deploy execution: {type(receipt).__name__}"
        )
    for note in receipt.get("bootstrap_notes", []):
        print(f"[wr6] devkit identity note: {note}", file=sys.stderr)


if __name__ == "__main__":
    _bootstrap_identity()
    script = DEVKIT_CHECKOUT / "tools" / "deploy_bundle.py"
    if not script.is_file():
        raise SystemExit(
            f"[FAIL] deploy_bundle.py not found at {script} - the bootstrap "
            "preflight passed but the Devkit checkout lacks the deploy tool"
        )
    raise SystemExit(subprocess.call(
        [sys.executable, str(script), "--repo", str(TARGET_ROOT), *sys.argv[1:]]
    ))
