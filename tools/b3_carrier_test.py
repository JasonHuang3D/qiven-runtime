from __future__ import annotations

"""B3 carrier fixture suite (ADR-0060 D3; P0 repair batch B3, 2026-10-02).

Regression coverage for the four-element FAIL carriers of this
repository's managed producer tools:
  B3-C1  format_sources --check FAIL: the selector line carries the
        counts, the violated rule and the mechanical FIX route
  B3-C2  per-finding evidence is bounded (byte budget + truncation
        count; whole carrier within the 8 KiB D3 budget)
  B3-C3  findings budget: at most 8 inline findings, remainder counted
  B3-C4  format_sources --fix FAIL: DIAGNOSE route with the exact
        clang-format re-invocation
  B3-C5  PASS carriers stay byte-stable (no teaching lines on success)
  B3-C6  git enumeration failure is typed with a FIX route
  B3-C7  apply_patch missing patch: FIX names the exact path
  B3-C8  apply_patch dirty tree: atomic-apply law + FIX, tree untouched
  B3-C9  apply_patch does-not-apply: git evidence + DIAGNOSE route
  B3-C10 apply_patch whitespace errors: FIX route; patch retained
  B3-C11 apply_patch success carrier byte-stable; patch consumed
  B3-L1..L5  the strict R6b WR-6 launcher (tools/qiven.py): unreadable,
        non-object and rejected preflight receipts fail typed BEFORE
        any operator import; notes are surfaced (skipped with a visible
        note where the local launcher is still the pre-WR-6 shim that
        pending adoption work replaces)

Each case id rides in the assertion message. Disposable temp fixtures
only (testing law: tests never touch developer repositories); the
clang-format and toolchain seams are stubbed in-process at the process
boundary, git facts stay real evidence.
"""

import importlib.util
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from contextlib import redirect_stdout
from pathlib import Path
from types import ModuleType
from typing import Callable

TOOLS = Path(__file__).resolve().parent
CHECKS = 0
CANNED_WARNING = "warning: code should be clang-formatted [-Wclang-format-violations]"


def check(condition: bool, label: str, detail: str = "") -> None:
    global CHECKS
    CHECKS += 1
    if not condition:
        raise AssertionError(f"[{label}] {detail}" if detail else f"[{label}] assertion failed")


def run(argv: list[str], *, cwd: Path, expect: int = 0,
        env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
    completed = subprocess.run(
        argv, cwd=cwd, env=env, text=True, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, check=False,
    )
    if completed.returncode != expect:
        raise AssertionError(
            f"unexpected exit {completed.returncode}, expected {expect}: {argv}\n{completed.stdout}"
        )
    return completed


def git(repo: Path, *args: str) -> subprocess.CompletedProcess[str]:
    return run(["git", "-C", str(repo), *args], cwd=repo)


def write(path: Path, text: str) -> None:
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)


def make_repo(temp: Path, name: str) -> Path:
    repo = temp / name
    (repo / "tools").mkdir(parents=True)
    for tool in ("format_sources.py", "apply_patch.py", "qiven.py"):
        source = TOOLS / tool
        if source.is_file():
            shutil.copyfile(source, repo / "tools" / tool)
    git(repo, "init", "-b", "main")
    git(repo, "config", "core.autocrlf", "false")
    git(repo, "config", "user.name", "B3Carrier")
    git(repo, "config", "user.email", "b3@example.invalid")
    return repo


def commit(repo: Path, message: str) -> None:
    git(repo, "add", "--all")
    git(repo, "commit", "-m", message)


def load_format_sources(tool_path: Path, behaviors: dict[str, tuple[int, str]],
                        *, git_rc: int = 0) -> tuple[ModuleType, Callable[[], None]]:
    """Load the fixture copy of format_sources.py with the toolchain
    module and the clang-format process boundary stubbed; git stays real
    unless git_rc forces the enumeration seam. Returns (module,
    restore); restore() must run in a finally block."""
    fake_clang = "fixture-clang-format"
    stub = ModuleType("toolchain")
    stub.resolve = lambda: {"clang_format": fake_clang, "cmake": "cmake-fixture"}  # type: ignore[attr-defined]
    saved_toolchain = sys.modules.get("toolchain")
    real_subprocess = subprocess

    class _Result:
        pass

    class _SubprocessProxy:
        """The tool's own subprocess module view: everything real except
        run(), which is stubbed only at the clang-format/git boundary."""

        def run(self, argv: list[str], *args, **kwargs):
            if argv and argv[0] == fake_clang:
                result = _Result()
                result.returncode, result.stdout = behaviors.get(
                    Path(argv[-1]).name, (0, ""))
                return result
            if git_rc and argv and argv[0] == "git" and "ls-files" in argv:
                result = _Result()
                result.returncode, result.stdout = git_rc, ""
                return result
            return real_subprocess.run(argv, *args, **kwargs)

        def __getattr__(self, name: str):
            return getattr(real_subprocess, name)

    sys.modules["toolchain"] = stub
    spec = importlib.util.spec_from_file_location("format_sources_b3", tool_path)
    if spec is None or spec.loader is None:
        raise AssertionError(f"could not load {tool_path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.subprocess = _SubprocessProxy()  # type: ignore[attr-defined]

    def restore() -> None:
        if saved_toolchain is None:
            sys.modules.pop("toolchain", None)
        else:
            sys.modules["toolchain"] = saved_toolchain

    return module, restore


def call_main(module: ModuleType, argv: list[str]) -> tuple[int, str]:
    saved_argv = sys.argv
    sys.argv = ["format_sources.py", *argv]
    buffer = io.StringIO()
    try:
        with redirect_stdout(buffer):
            code = module.main()
    finally:
        sys.argv = saved_argv
    return int(code), buffer.getvalue()


def format_cases(temp: Path) -> None:
    # ---------------- C1: check-mode selector line ------------------
    repo = make_repo(temp, "b3-fmt-counts")
    write(repo / "clean.cpp", "int main() { return 0; }\n")
    write(repo / "unformatted_a.cpp", "int  main( ){return 0;}\n")
    write(repo / "unformatted_b.cpp", "int  main( ){return 1;}\n")
    commit(repo, "baseline")
    module, restore = load_format_sources(
        repo / "tools" / "format_sources.py",
        {"unformatted_a.cpp": (1, CANNED_WARNING + "\n"),
         "unformatted_b.cpp": (1, CANNED_WARNING + "\n")},
    )
    try:
        code, out = call_main(module, ["--check"])
    finally:
        restore()
    check(code == 1, "B3-C1.exit", out)
    selector = next((ln for ln in out.splitlines() if "failures=" in ln), "")
    check("failures=2 of 3" in selector, "B3-C1.counts", selector)
    check("violate .clang-format" in selector, "B3-C1.rule", selector)
    check("FIX: python tools/format_sources.py --fix" in selector, "B3-C1.fix-route", selector)

    # ---------------- C2: bounded per-finding evidence --------------
    repo = make_repo(temp, "b3-fmt-huge")
    write(repo / "huge.cpp", "int a;\n")
    commit(repo, "baseline")
    huge = "".join(f"{CANNED_WARNING} line {n:04d}\n" for n in range(200))
    module, restore = load_format_sources(
        repo / "tools" / "format_sources.py", {"huge.cpp": (1, huge)},
    )
    try:
        code, out = call_main(module, ["--check"])
    finally:
        restore()
    check(code == 1, "B3-C2.exit", out[-400:])
    check("[evidence: first " in out and " of " in out and " captured bytes]" in out,
          "B3-C2.truncation-count", "the truncation marker with byte counts is required")
    check(len(out.encode("utf-8")) <= 8192, "B3-C2.d3-budget",
          f"carrier serialized to {len(out.encode('utf-8'))} bytes")

    # ---------------- C3: findings budget (8 max) --------------------
    repo = make_repo(temp, "b3-fmt-many")
    for n in range(10):
        write(repo / f"f{n:02d}.cpp", f"int v{n};\n")
    commit(repo, "baseline")
    behaviors = {f"f{n:02d}.cpp": (1, CANNED_WARNING + "\n") for n in range(10)}
    module, restore = load_format_sources(
        repo / "tools" / "format_sources.py", behaviors,
    )
    try:
        code, out = call_main(module, ["--check"])
    finally:
        restore()
    check(code == 1, "B3-C3.exit", out[-400:])
    headers = [ln for ln in out.splitlines()
               if re.match(r"\[FAIL\] f\d{2}\.cpp$", ln)]
    check(len(headers) == 8, "B3-C3.findings-cap",
          f"{len(headers)} inline findings shown; at most 8 allowed")
    check("2 further failing source(s) not shown" in out, "B3-C3.omission-count", out[-400:])
    check("failures=10 of 10" in out, "B3-C3.total-honest", out[-400:])

    # ---------------- C4: fix-mode failure teaches DIAGNOSE ---------
    repo = make_repo(temp, "b3-fmt-fixfail")
    write(repo / "locked.cpp", "int a;\n")
    commit(repo, "baseline")
    module, restore = load_format_sources(
        repo / "tools" / "format_sources.py", {"locked.cpp": (1, "error: cannot rewrite\n")},
    )
    try:
        code, out = call_main(module, ["--fix"])
    finally:
        restore()
    check(code == 1, "B3-C4.exit", out)
    selector = next((ln for ln in out.splitlines() if "failures=" in ln), "")
    check("DIAGNOSE:" in selector, "B3-C4.diagnose-route", selector)
    check('-i --style=file locked.cpp' in selector, "B3-C4.exact-command", selector)

    # ---------------- C5: PASS byte-stable ---------------------------
    repo = make_repo(temp, "b3-fmt-clean")
    write(repo / "a.cpp", "int a;\n")
    write(repo / "b.cpp", "int b;\n")
    commit(repo, "baseline")
    module, restore = load_format_sources(repo / "tools" / "format_sources.py", {})
    try:
        code, out = call_main(module, ["--check"])
    finally:
        restore()
    check(code == 0, "B3-C5.exit", out)
    check("[ OK ] format --check: 2 tracked C/C++ sources" in out.splitlines(),
          "B3-C5.pass-byte-stable", out)
    check(not any(("FIX:" in ln or "DIAGNOSE:" in ln or "NEXT" in ln)
                  for ln in out.splitlines()),
          "B3-C5.pass-no-teaching", "PASS carrier must not grow teaching lines")

    # ---------------- C6: git enumeration failure --------------------
    repo = make_repo(temp, "b3-fmt-nogit")
    write(repo / "a.cpp", "int a;\n")
    commit(repo, "baseline")
    module, restore = load_format_sources(
        repo / "tools" / "format_sources.py", {}, git_rc=128,
    )
    try:
        code, out = call_main(module, ["--check"])
    finally:
        restore()
    check(code == 1, "B3-C6.exit", out)
    check("[FAIL] git ls-files failed (exit 128)" in out, "B3-C6.typed-what", out)
    check("FIX:" in out, "B3-C6.fix-route", out)


GOOD_PATCH = """diff --git a/src.cpp b/src.cpp
--- a/src.cpp
+++ b/src.cpp
@@ -1,1 +1,2 @@
 real content
+patched line
"""

BAD_CONTEXT_PATCH = """diff --git a/src.cpp b/src.cpp
--- a/src.cpp
+++ b/src.cpp
@@ -1,1 +1,1 @@
-WRONG-CONTEXT
+patched line
"""

WHITESPACE_PATCH = """diff --git a/src.cpp b/src.cpp
--- a/src.cpp
+++ b/src.cpp
@@ -1,1 +1,2 @@
 real content
+added line with trailing space{space}
"""


def apply_cases(temp: Path) -> None:
    tool = Path("tools") / "apply_patch.py"

    # ---------------- C7: missing patch ------------------------------
    repo = make_repo(temp, "b3-apply-missing")
    write(repo / "src.cpp", "real content\n")
    commit(repo, "baseline")
    done = run([sys.executable, str(repo / tool)], cwd=repo, expect=1)
    check("[FAIL] Patch not found:" in done.stdout, "B3-C7.typed-what", done.stdout)
    check(str(repo / "candidate.patch") in done.stdout, "B3-C7.exact-path", done.stdout)
    check("FIX:" in done.stdout, "B3-C7.fix-route", done.stdout)

    # ---------------- C8: dirty tree refusal -------------------------
    repo = make_repo(temp, "b3-apply-dirty")
    write(repo / "src.cpp", "real content\n")
    commit(repo, "baseline")
    write(repo / "candidate.patch", GOOD_PATCH)
    write(repo / "src.cpp", "dirty working state\n")
    before = (repo / "src.cpp").read_text(encoding="utf-8")
    done = run([sys.executable, str(repo / tool)], cwd=repo, expect=1)
    check("[FAIL] Working tree has tracked changes." in done.stdout,
          "B3-C8.typed-what", done.stdout)
    check("atomic-apply law" in done.stdout, "B3-C8.why-law", done.stdout)
    check("FIX:" in done.stdout, "B3-C8.fix-route", done.stdout)
    check((repo / "src.cpp").read_text(encoding="utf-8") == before,
          "B3-C8.tree-untouched", "the refusal must not modify the working tree")

    # ---------------- C9: does not apply ------------------------------
    repo = make_repo(temp, "b3-apply-conflict")
    write(repo / "src.cpp", "real content\n")
    commit(repo, "baseline")
    write(repo / "candidate.patch", BAD_CONTEXT_PATCH)
    done = run([sys.executable, str(repo / tool)], cwd=repo, expect=1)
    check("does not apply cleanly" in done.stdout, "B3-C9.typed-what", done.stdout)
    check("error:" in done.stdout, "B3-C9.git-evidence", done.stdout)
    check("DIAGNOSE:" in done.stdout, "B3-C9.diagnose-route", done.stdout)

    # ---------------- C10: whitespace errors --------------------------
    repo = make_repo(temp, "b3-apply-whitespace")
    write(repo / "src.cpp", "real content\n")
    commit(repo, "baseline")
    write(repo / "candidate.patch", WHITESPACE_PATCH.format(space=" "))
    done = run([sys.executable, str(repo / tool)], cwd=repo, expect=1)
    check("whitespace" in done.stdout.lower(), "B3-C10.typed-what", done.stdout)
    check("FIX:" in done.stdout, "B3-C10.fix-route", done.stdout)
    check((repo / "candidate.patch").is_file(),
          "B3-C10.patch-retained", "applied-but-flagged patch must be kept")
    check("added line with trailing space" in (repo / "src.cpp").read_text(encoding="utf-8"),
          "B3-C10.applied-anyway", "the change itself applied before the check flagged it")

    # ---------------- C11: success byte-stable ------------------------
    repo = make_repo(temp, "b3-apply-ok")
    write(repo / "src.cpp", "real content\n")
    commit(repo, "baseline")
    write(repo / "candidate.patch", GOOD_PATCH)
    done = run([sys.executable, str(repo / tool)], cwd=repo, expect=0)
    check(f"[ OK ] Patch applied successfully to {repo.name}; candidate.patch removed."
          in done.stdout.splitlines(), "B3-C11.pass-byte-stable", done.stdout)
    check(not (repo / "candidate.patch").exists(), "B3-C11.patch-consumed", done.stdout)
    check("patched line" in (repo / "src.cpp").read_text(encoding="utf-8"),
          "B3-C11.change-applied", done.stdout)


def launcher_cases(temp: Path) -> None:
    launcher = TOOLS / "qiven.py"
    source = launcher.read_text(encoding="utf-8")
    if "WR-6 launcher" not in source:
        print(f"[SKIP] B3-L: {launcher.name} is not the WR-6 launcher variant "
              "(pre-WR-6 shim; strict-launcher adoption rides its own batch)")
        return
    control = temp / "control"
    (control / "bootstrap").mkdir(parents=True)
    devkit = temp / "devkit"
    (devkit / "tools").mkdir(parents=True)
    write(devkit / "tools" / "qiven_operator.py",
          "def main() -> int:\n    print('OPERATOR-REACHED')\n    return 0\n")
    bootstrap = control / "bootstrap" / "qiven-bootstrap.py"

    def launch(control_dir: Path) -> subprocess.CompletedProcess[str]:
        env = {**os.environ, "QIVEN_WORKSPACE_CONTROL": str(control_dir),
               "QIVEN_DEVKIT_CHECKOUT": str(devkit)}
        return subprocess.run(
            [sys.executable, str(launcher)], env=env, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False,
        )

    # ---------------- L1: unreadable receipt fails typed -------------
    write(bootstrap, "print('this is not json')\n")
    done = launch(control)
    check(done.returncode != 0, "B3-L1.exit", done.stdout)
    check("preflight receipt unreadable" in done.stdout
          and "refusing Operator import" in done.stdout,
          "B3-L1.typed-refusal", done.stdout)
    check("OPERATOR-REACHED" not in done.stdout, "B3-L1.no-operator-import", done.stdout)

    # ---------------- L2: non-object receipt fails typed -------------
    write(bootstrap, "print('[1, 2, 3]')\n")
    done = launch(control)
    check(done.returncode != 0, "B3-L2.exit", done.stdout)
    check("preflight receipt is not an object" in done.stdout and "list" in done.stdout,
          "B3-L2.typed-refusal", done.stdout)
    check("OPERATOR-REACHED" not in done.stdout, "B3-L2.no-operator-import", done.stdout)

    # ---------------- L3: rejected bootstrap relays + fails ----------
    write(bootstrap, "import sys\nprint('fixture rejection detail')\nsys.exit(3)\n")
    done = launch(control)
    check(done.returncode != 0, "B3-L3.exit", done.stdout)
    check("fixture rejection detail" in done.stdout, "B3-L3.relayed-evidence", done.stdout)
    check("[FAIL] workspace bootstrap rejected the local Devkit" in done.stdout,
          "B3-L3.typed-refusal", done.stdout)
    # P3-21 (runtime twin mirrors the context twin): the devkit-rejection
    # refusal carries the typed NEXT element - FIX naming the correction
    # (align the local Devkit checkout to the lock's qiven-devkit node).
    check(done.returncode == 1, "B3-L3.exit-1", done.stdout)
    check("NEXT action: FIX" in done.stdout, "B3-L3.next-fix", done.stdout)
    check("qiven-devkit node" in done.stdout, "B3-L3.next-fix-correction", done.stdout)
    check("retry the launcher" in done.stdout, "B3-L3.next-retry", done.stdout)

    # ---------------- L4: notes surfaced; operator reached -----------
    write(bootstrap, "import json\nprint(json.dumps({'bootstrap_notes': "
                     "['fixture identity note']}))\n")
    done = launch(control)
    check(done.returncode == 0, "B3-L4.exit", done.stdout)
    check("[devkit-identity] devkit identity note: fixture identity note" in done.stdout,
          "B3-L4.notes-surfaced", done.stdout)
    check("OPERATOR-REACHED" in done.stdout, "B3-L4.happy-path", done.stdout)

    # ---------------- L5: missing bootstrap named --------------------
    done = launch(temp / "missing-control")
    check(done.returncode != 0, "B3-L5.exit", done.stdout)
    check("[FAIL] workspace bootstrap not found at" in done.stdout,
          "B3-L5.typed-refusal", done.stdout)
    # P3-21 (runtime twin mirrors the context twin): the bootstrap-not-found
    # refusal carries the typed NEXT element too - FIX naming the missing
    # QIVEN_WORKSPACE_CONTROL locator.
    check(done.returncode == 1, "B3-L5.exit-1", done.stdout)
    check("NEXT action: FIX" in done.stdout, "B3-L5.next-fix", done.stdout)
    check("QIVEN_WORKSPACE_CONTROL" in done.stdout, "B3-L5.next-fix-correction", done.stdout)
    check("retry the launcher" in done.stdout, "B3-L5.next-retry", done.stdout)


def main() -> int:
    global CHECKS
    with tempfile.TemporaryDirectory(prefix="qiven-b3-carrier-") as temp_name:
        temp = Path(temp_name)
        format_cases(temp)
        apply_cases(temp)
        launcher_cases(temp)
    print(f"[ OK ] b3-carrier: {CHECKS} checks")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
