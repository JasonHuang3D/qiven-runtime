from __future__ import annotations

"""WR-6 deploy launcher: the deploy transport resolves the Devkit checkout
through the WORKSPACE BOOTSTRAP identity-check BEFORE any Devkit code runs -
no QIVEN_DEVKIT_ROOT variable and no consumer-local pin; the lock's
qiven-devkit node is the only admitted source (deployment law: qiven-devkit
docs/engineering/deployment.md; launcher shape per qiven-math tools/qiven.py).
A wrong local Devkit revision (or an unreadable workspace lock) fails typed
here, before deploy_bundle.py executes."""

import json
import os
import subprocess
import sys
from pathlib import Path

sys.dont_write_bytecode = True

TARGET_ROOT = Path(__file__).resolve().parents[1]
DEVKIT_CHECKOUT = Path(os.environ.get("QIVEN_DEVKIT_CHECKOUT", TARGET_ROOT.parent / "qiven-devkit"))


def _bootstrap_identity() -> None:
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
        for note in receipt.get("bootstrap_notes", []):
            print(f"[wr6] devkit identity note: {note}", file=sys.stderr)
    except json.JSONDecodeError:
        print("[wr6] preflight receipt unreadable (notes not surfaced)",
              file=sys.stderr)


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
