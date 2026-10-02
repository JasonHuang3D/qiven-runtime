"""B3 deploy-launcher fixture suite (ADR-0060 D3; P0 repair batch B3, 2026-10-02).

Regression coverage for the strict R6b receipt law of the runtime deploy
launcher (tools/deploy.py; register row runtime-deploy-py):
  B3-D1  an unreadable preflight receipt fails typed BEFORE any deploy
        code runs (the former warn-and-continue drift)
  B3-D2  a non-object receipt fails typed with the observed type named
  B3-D3  a rejected bootstrap relays its evidence and fails typed
  B3-D4  a valid receipt surfaces its notes and reaches deploy_bundle

Each case id rides in the assertion message. Disposable temp fixtures
only (testing law: tests never touch developer repositories); the
bootstrap and the deploy tool are stubbed at their process boundaries
while the launcher under test runs for real.
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
from pathlib import Path

LAUNCHER = Path(__file__).resolve().parent / "deploy.py"
CHECKS = 0


def check(condition: bool, label: str, detail: str = "") -> None:
    global CHECKS
    CHECKS += 1
    if not condition:
        raise AssertionError(f"[{label}] {detail}" if detail else f"[{label}] assertion failed")


def write(path: Path, text: str) -> None:
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)


def launch(control_dir: Path, devkit_dir: Path) -> subprocess.CompletedProcess[str]:
    env = {**os.environ, "QIVEN_WORKSPACE_CONTROL": str(control_dir),
           "QIVEN_DEVKIT_CHECKOUT": str(devkit_dir)}
    return subprocess.run(
        [sys.executable, str(LAUNCHER)], env=env, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False,
    )


def main() -> int:
    global CHECKS
    with tempfile.TemporaryDirectory(prefix="qiven-b3-deploy-") as temp_name:
        temp = Path(temp_name)
        control = temp / "control"
        (control / "bootstrap").mkdir(parents=True)
        devkit = temp / "devkit"
        (devkit / "tools").mkdir(parents=True)
        write(devkit / "tools" / "deploy_bundle.py",
              "print('DEPLOY-REACHED')\nraise SystemExit(0)\n")
        bootstrap = control / "bootstrap" / "qiven-bootstrap.py"

        # ---------------- D1: unreadable receipt fails typed -----------
        write(bootstrap, "print('this is not json')\n")
        done = launch(control, devkit)
        check(done.returncode != 0, "B3-D1.exit", done.stdout)
        check("preflight receipt unreadable" in done.stdout
              and "refusing deploy execution" in done.stdout,
              "B3-D1.typed-refusal", done.stdout)
        check("DEPLOY-REACHED" not in done.stdout, "B3-D1.no-deploy-run", done.stdout)

        # ---------------- D2: non-object receipt fails typed -----------
        write(bootstrap, "print('[1, 2, 3]')\n")
        done = launch(control, devkit)
        check(done.returncode != 0, "B3-D2.exit", done.stdout)
        check("preflight receipt is not an object" in done.stdout and "list" in done.stdout,
              "B3-D2.typed-refusal", done.stdout)
        check("DEPLOY-REACHED" not in done.stdout, "B3-D2.no-deploy-run", done.stdout)

        # ---------------- D3: rejected bootstrap relays + fails --------
        write(bootstrap, "import sys\nprint('fixture rejection detail')\nsys.exit(3)\n")
        done = launch(control, devkit)
        check(done.returncode != 0, "B3-D3.exit", done.stdout)
        check("fixture rejection detail" in done.stdout, "B3-D3.relayed-evidence", done.stdout)
        check("[FAIL] workspace bootstrap rejected the local Devkit" in done.stdout,
              "B3-D3.typed-refusal", done.stdout)

        # ---------------- D4: valid receipt surfaces notes; runs --------
        write(bootstrap, "import json\nprint(json.dumps({'bootstrap_notes': "
                         "['fixture identity note']}))\n")
        done = launch(control, devkit)
        check(done.returncode == 0, "B3-D4.exit", done.stdout)
        check("[wr6] devkit identity note: fixture identity note" in done.stdout,
              "B3-D4.notes-surfaced", done.stdout)
        check("DEPLOY-REACHED" in done.stdout, "B3-D4.deploy-runs", done.stdout)

    print(f"[ OK ] b3-deploy: {CHECKS} checks")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
