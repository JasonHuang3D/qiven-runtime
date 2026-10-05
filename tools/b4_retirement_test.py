from __future__ import annotations

"""B4 vendored-operator retirement suite (ADR-0060 D3; P0 repair batch B4,
2026-10-02).

Regression coverage for this repository's adoption of the template
0.1.11 strict-R6b WR-6 launcher and the retirement of the vendored
tools/qiven_operator.py copy:
  B4-R1  the vendored operator copy is gone from this tree
  B4-R2  tools/qiven.py is the byte-identical template render
         (sha256-pinned; drift fails loudly instead of silently)
  B4-R3  .qiven/generated-state.cmake no longer records the vendored
         path, and its recorded tools/qiven.py hash is truthful
  B4-R4  the launcher refuses a rejected preflight BEFORE any operator
         import (identity-check ordering, fixture control)
  B4-R5  the happy path routes .qiven/operator.json through the WR-6
         chain to the fixture devkit's canonical operator copy
  B4-R6  a failing fixture gate surfaces the four-element FAIL carrier
         intact through the adopted launcher (NEXT action + evidence
         selector + QIVEN-RECORD v1 view; the B1-proven carrier bytes)
  B4-R7  a passing fixture gate carries the PASS receipt selector line
  B4-R8  a failing fixture run retains its task-run record under
         .generated-temp/operator/records/ with a DIAGNOSE next action

Each case id rides in the assertion message. Disposable temp fixtures
only (testing law: tests never touch developer repositories); the
fixture devkit carries a byte copy of the canonical devkit operator
(the same file the WR-6 chain serves from the sibling checkout)."""

import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
REPO = TOOLS.parent
CANONICAL_OPERATOR = TOOLS.parent.parent / "qiven-devkit" / "tools" / "qiven_operator.py"
# Template strict-R6b render (P3-21 twin roll: the two tokenless
# refusal sites carry NEXT FIX lines, mirroring the published
# qiven-context twin - the local devkit checkout's template has not
# rolled this render yet; sha256 over the whole file).
TEMPLATE_LAUNCHER_SHA256 = (
    "01d6636b2b1bc255c87652e0c76e5861ab474a911db7aeff7ebf0dfed35f4dd5"
)
CHECKS = 0


def check(condition: bool, label: str, detail: str = "") -> None:
    global CHECKS
    CHECKS += 1
    if not condition:
        raise AssertionError(f"[{label}] {detail}" if detail else f"[{label}] assertion failed")


def run(argv: list[str], *, cwd: Path, expect: int | None = None,
        env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
    completed = subprocess.run(
        argv, cwd=cwd, env=env, text=True, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, check=False,
    )
    if expect is not None and completed.returncode != expect:
        raise AssertionError(
            f"unexpected exit {completed.returncode}, expected {expect}: {argv}\n{completed.stdout}"
        )
    return completed


def git(repo: Path, *args: str) -> subprocess.CompletedProcess[str]:
    return run(["git", "-C", str(repo), *args], cwd=repo)


def write(path: Path, text: str) -> None:
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)


def sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def state_var(path_text: str) -> str:
    """The generated-state.cmake hash variable name for a managed path
    (QIVEN_HASH_<sha256(path)>; QivenRepoCommon.cmake qiven_state_key)."""
    return "QIVEN_HASH_" + hashlib.sha256(path_text.encode("utf-8")).hexdigest()


def tree_cases() -> None:
    # ---------------- R1: vendored copy retired ----------------------
    check(not (TOOLS / "qiven_operator.py").exists(), "B4-R1.vendored-absent",
          "tools/qiven_operator.py must not exist after B4 retirement")

    # ---------------- R2: launcher is the template render ------------
    launcher = TOOLS / "qiven.py"
    check("WR-6 launcher" in launcher.read_text(encoding="utf-8"),
          "B4-R2.launcher-marker", "the strict WR-6 launcher docstring is required")
    check(sha256_file(launcher) == TEMPLATE_LAUNCHER_SHA256, "B4-R2.template-byte-identical",
          f"sha256 {sha256_file(launcher)} != pinned template render "
          f"{TEMPLATE_LAUNCHER_SHA256}")

    # ---------------- R3: generated-state coherent -------------------
    state = (REPO / ".qiven" / "generated-state.cmake").read_text(encoding="utf-8")
    check("tools/qiven_operator.py" not in state, "B4-R3.no-vendored-path",
          "generated-state must not list the retired vendored operator")
    check(state_var("tools/qiven_operator.py") not in state,
          "B4-R3.no-vendored-hash", "generated-state must not carry its hash entry")
    check('"tools/qiven.py"' in state, "B4-R3.launcher-managed",
          "generated-state must still manage tools/qiven.py")
    import re
    recorded = re.search(state_var("tools/qiven.py") + r' "([0-9a-f]{64})"', state)
    check(recorded is not None, "B4-R3.launcher-hash-recorded", state)
    check(recorded is not None and recorded.group(1) == sha256_file(TOOLS / "qiven.py"),
          "B4-R3.launcher-hash-truthful",
          "recorded launcher hash must equal the adopted file bytes")


def make_fixture(temp: Path, name: str) -> tuple[Path, Path, Path]:
    """A fixture repository with the ADOPTED launcher, a fixture control
    (stub bootstrap) and a fixture devkit carrying the canonical
    operator bytes."""
    repo = temp / name
    (repo / ".qiven").mkdir(parents=True)
    (repo / "tools").mkdir(parents=True)
    shutil.copyfile(TOOLS / "qiven.py", repo / "tools" / "qiven.py")
    control = temp / (name + "-control")
    (control / "bootstrap").mkdir(parents=True)
    devkit = temp / (name + "-devkit")
    (devkit / "tools").mkdir(parents=True)
    shutil.copyfile(CANONICAL_OPERATOR, devkit / "tools" / "qiven_operator.py")
    # the canonical operator imports its devkit siblings (common_record,
    # record_projection - the B1 record machinery); carry them too
    for sibling in ("common_record.py", "record_projection.py"):
        source = CANONICAL_OPERATOR.parent / sibling
        if source.is_file():
            shutil.copyfile(source, devkit / "tools" / sibling)
    # the canonical operator reports the devkit checkout's HEAD; give
    # the fixture devkit a real committed HEAD
    git(devkit, "init", "-b", "main")
    git(devkit, "config", "core.autocrlf", "false")
    git(devkit, "config", "user.name", "B4Retirement")
    git(devkit, "config", "user.email", "b4@example.invalid")
    git(devkit, "add", "--all")
    git(devkit, "commit", "-m", "fixture devkit")
    return repo, control, devkit


def launch(repo: Path, control: Path, devkit: Path, *args: str,
           expect: int | None = None) -> subprocess.CompletedProcess[str]:
    env = {**os.environ,
           "QIVEN_WORKSPACE_CONTROL": str(control),
           "QIVEN_DEVKIT_CHECKOUT": str(devkit)}
    return run([sys.executable, str(repo / "tools" / "qiven.py"), *args],
               cwd=repo, env=env, expect=expect)


def write_config(repo: Path, tasks: dict, gates: dict) -> None:
    config = {
        "schema_version": 1,
        "repository_name": "b4-" + repo.name,
        "default_gate": next(iter(gates)),
        "tasks": tasks,
        "gates": gates,
        "ci": {},
    }
    write(repo / ".qiven" / "operator.json", json.dumps(config, indent=2) + "\n")


FAIL_TASK = {"argv": [sys.executable, "-c",
                      "import sys; sys.stderr.write('b4-boom\\n'); raise SystemExit(3)"]}
PASS_TASK = {"argv": [sys.executable, "-c", "print('b4-pass')"]}


def identity_cases(temp: Path) -> None:
    repo, control, devkit = make_fixture(temp, "b4-identity")
    git(repo, "init", "-b", "main")
    git(repo, "config", "core.autocrlf", "false")
    git(repo, "config", "user.name", "B4Retirement")
    git(repo, "config", "user.email", "b4@example.invalid")
    bootstrap = control / "bootstrap" / "qiven-bootstrap.py"

    # ---------------- R4: rejected preflight precedes import ---------
    write(bootstrap, "import sys\nprint('fixture rejection detail')\nsys.exit(3)\n")
    done = launch(repo, control, devkit)
    check(done.returncode != 0, "B4-R4.exit", done.stdout)
    check("fixture rejection detail" in done.stdout, "B4-R4.relayed-evidence", done.stdout)
    check("[FAIL] workspace bootstrap rejected the local Devkit" in done.stdout,
          "B4-R4.typed-refusal", done.stdout)

    # ---------------- R5: happy path routes to the operator ----------
    write(bootstrap, "import json\nprint(json.dumps({'bootstrap_notes': []}))\n")
    write_config(repo, {"b4-pass": PASS_TASK}, {"b4-pass-gate": ["b4-pass"]})
    git(repo, "add", "--all")
    git(repo, "commit", "-m", "fixture baseline")
    done = launch(repo, control, devkit, "--json", "info", expect=0)
    payload = json.loads(done.stdout)
    check(payload.get("repository") == "b4-" + repo.name, "B4-R5.routed-to-operator",
          f"info payload must come from the fixture devkit's canonical operator: {done.stdout}")
    check(payload.get("default_gate") == "b4-pass-gate", "B4-R5.config-read",
          "the operator must read this fixture's .qiven/operator.json")


def carrier_cases(temp: Path) -> None:
    repo, control, devkit = make_fixture(temp, "b4-carrier")
    bootstrap = control / "bootstrap" / "qiven-bootstrap.py"
    write(bootstrap, "import json\nprint(json.dumps({'bootstrap_notes': []}))\n")
    git(repo, "init", "-b", "main")
    git(repo, "config", "core.autocrlf", "false")
    git(repo, "config", "user.name", "B4Retirement")
    git(repo, "config", "user.email", "b4@example.invalid")
    write_config(repo, {"b4-fail": FAIL_TASK, "b4-pass": PASS_TASK},
                 {"b4-fail-gate": ["b4-fail"], "b4-pass-gate": ["b4-pass"]})
    git(repo, "add", "--all")
    git(repo, "commit", "-m", "baseline")
    records = repo / ".generated-temp" / "operator" / "records"

    # ---------------- R6: gate FAIL carrier through the launcher -----
    done = launch(repo, control, devkit, "--no-color", "gate", "b4-fail-gate", expect=1)
    summary = next((ln for ln in done.stdout.splitlines()
                    if "gate:b4-fail-gate: FAIL" in ln), "")
    check("NEXT action:" in summary, "B4-R6.next-action", summary or done.stdout[-400:])
    check(" - evidence: " in summary, "B4-R6.evidence-selector", summary)
    check("QIVEN-RECORD v1" in done.stdout, "B4-R6.projection-view", done.stdout[-400:])

    # ---------------- R7: gate PASS receipt selector -----------------
    done = launch(repo, control, devkit, "--no-color", "gate", "b4-pass-gate", expect=0)
    check(any("gate:b4-pass-gate: PASS - receipt: " in ln
              for ln in done.stdout.splitlines()), "B4-R7.pass-selector", done.stdout)

    # ---------------- R8: run FAIL retains the task-run record -------
    done = launch(repo, control, devkit, "--no-color", "run", "b4-fail", expect=1)
    run_line = next((ln for ln in done.stdout.splitlines() if "run: FAIL" in ln), "")
    check("NEXT action: DIAGNOSE" in run_line, "B4-R8.next-action", run_line or done.stdout[-400:])
    check(" - evidence: " in run_line, "B4-R8.evidence-selector", run_line)
    kept = sorted(records.glob("run-*.json"))
    check(bool(kept), "B4-R8.record-retained", f"no run-* record under {records}")
    record = json.loads(kept[-1].read_text(encoding="utf-8"))
    check(record.get("record_kind") == "task-run", "B4-R8.record-kind", str(record)[:200])
    check(record.get("next_action", {}).get("action") == "DIAGNOSE", "B4-R8.record-next")


def main() -> int:
    global CHECKS
    if not CANONICAL_OPERATOR.is_file():
        raise AssertionError(
            f"[B4-fixture] canonical devkit operator not found at {CANONICAL_OPERATOR}; "
            "the WR-6 chain requires the sibling qiven-devkit checkout"
        )
    tree_cases()
    with tempfile.TemporaryDirectory(prefix="qiven-b4-retirement-") as temp_name:
        temp = Path(temp_name)
        identity_cases(temp)
        carrier_cases(temp)
    print(f"[ OK ] b4-retirement: {CHECKS} checks")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
