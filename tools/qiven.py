from __future__ import annotations

"""WR-6 launcher (ADR-0052 doc 02 stage WR-6; supersedes the ADR-0046
shim+pin discovery): this repository's operator entry invokes the
WORKSPACE BOOTSTRAP identity-check BEFORE any Devkit code is imported -
a wrong local Devkit revision (or an unreadable workspace lock) fails
typed here, before operator execution. The launcher identifies the
repository (QIVEN_TARGET_ROOT) and the workspace control (explicit
--devkit-style locator via QIVEN_WORKSPACE_CONTROL, else the sibling
control repository); it contains NO Devkit path fallback and NO
consumer-local Devkit pin. The consumed revision is exactly the lock's
qiven-devkit node."""

import importlib
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
    preflight path) and fail typed on any mismatch. The bootstrap never
    resolves through an unverified Devkit and never selects a sibling
    fallback (qiven-bootstrap.py; WR-1 law). The receipt's notes (e.g.
    a dirty devkit checkout label in shadow mode) are SURFACED - a
    swallowed label would let uncommitted operator edits execute
    silently."""
    control = Path(os.environ.get("QIVEN_WORKSPACE_CONTROL",
                                  TARGET_ROOT.parent / "qiven-workspace")).resolve()
    bootstrap = control / "bootstrap" / "qiven-bootstrap.py"
    if not bootstrap.is_file():
        raise SystemExit(
            f"[FAIL] workspace bootstrap not found at {bootstrap} - NEXT "
            "action: FIX - set QIVEN_WORKSPACE_CONTROL to the control "
            "checkout that contains bootstrap/qiven-bootstrap.py (the Devkit "
            "revision is the lock's qiven-devkit node - WR-6, no local pin), "
            "then retry the launcher"
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
            "wrong local Devkit revision fails before Operator code "
            "executes) - NEXT action: FIX - the preflight output above names "
            "the mismatch; align the local Devkit checkout "
            f"({DEVKIT_CHECKOUT}) to the workspace lock's qiven-devkit node, "
            "then retry the launcher"
        )
    try:
        receipt = json.loads(completed.stdout)
    except json.JSONDecodeError as exc:
        # P0 repair R6b: the receipt IS the WR-6 identity evidence. An
        # unreadable receipt must fail typed BEFORE any Operator import -
        # the former warn-and-continue let an unverified identity ride
        # straight into `importlib.import_module("qiven_operator")`.
        raise SystemExit(
            "[FAIL] preflight receipt unreadable (required WR-6 identity "
            f"evidence); refusing Operator import: {exc} - NEXT action: "
            "DIAGNOSE - run the bootstrap preflight printed above directly "
            "to see its raw output; fix what makes it non-JSON, then retry "
            "the launcher"
        )
    if not isinstance(receipt, dict):
        raise SystemExit(
            "[FAIL] preflight receipt is not an object (required WR-6 "
            f"identity evidence); refusing Operator import: {type(receipt).__name__}"
            " - NEXT action: DIAGNOSE - the bootstrap must print exactly one "
            "JSON receipt on stdout; run it directly to see the stray output, "
            "then retry the launcher"
        )
    for note in receipt.get("bootstrap_notes", []):
        print(f"[wr6] devkit identity note: {note}", file=sys.stderr)


os.environ["QIVEN_TARGET_ROOT"] = str(TARGET_ROOT)
_bootstrap_identity()
sys.path.insert(0, str(DEVKIT_CHECKOUT / "tools"))
main = importlib.import_module("qiven_operator").main


if __name__ == "__main__":
    raise SystemExit(main())
