from __future__ import annotations

"""Shared toolchain resolution and pinned-version validation.

WR-5 (ADR-0052 doc 02): the Toolchain root is obtained from the
WorkspaceGeneration - the root LOCATOR is env/sibling, but the selected
REVISION is the locked qiven-toolchain-win node, identity-checked here.
The qiven-toolchain-win manifest (toolchain.json) remains the single
inventory of executables and versions inside the selected node; this
module proves the installed tools actually report the manifest-declared
versions. Standard library only, Python 3.9+.
"""

import json
import os
import subprocess
from pathlib import Path


def _locked_toolchain_commit() -> str:
    control = Path(os.environ.get(
        "QIVEN_WORKSPACE_CONTROL",
        str(Path(__file__).resolve().parent.parent.parent / "qiven-workspace"),
    )).resolve()
    lock_path = control / "workspace.lock.json"
    try:
        lock = json.loads(lock_path.read_text(encoding="utf-8-sig"))
    except (OSError, json.JSONDecodeError) as exc:
        raise SystemExit(f"[FAIL] workspace lock unreadable at {lock_path}: {exc}")
    node = lock.get("nodes", {}).get("qiven-toolchain-win")
    commit = node.get("commit") if isinstance(node, dict) else None
    if not isinstance(commit, str) or len(commit) != 40:
        raise SystemExit("[FAIL] workspace lock has no qiven-toolchain-win node commit")
    return commit


def toolchain_root() -> Path:
    candidate = os.environ.get("QIVEN_TOOLCHAIN_ROOT")
    if not candidate:
        candidate = Path(__file__).resolve().parent.parent.parent / "qiven-toolchain-win"
    root = Path(candidate).resolve()
    locked = _locked_toolchain_commit()
    head = subprocess.run(["git", "-C", str(root), "rev-parse", "HEAD"],
                          capture_output=True, text=True, check=False).stdout.strip()
    if head != locked:
        found = head[:12] if head else "<unreadable>"
        raise SystemExit(f"[FAIL] toolchain checkout at {found} != locked node {locked[:12]}; "
                         "advance the workspace lock deliberately")
    return root


def resolve() -> dict:
    base = toolchain_root()
    manifest = base / "toolchain.json"
    if not manifest.is_file():
        raise SystemExit(f"[FAIL] toolchain manifest not found: {manifest}")
    try:
        data = json.loads(manifest.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise SystemExit(f"[FAIL] invalid toolchain manifest: {exc}") from exc
    tools = data.get("tools", {})
    resolved: dict = {"toolchain_root": str(base)}
    for logical, key in (("cmake", "cmake"), ("clang_format", "clang-format")):
        entry = tools.get(key)
        if not isinstance(entry, dict) or not entry.get("path"):
            raise SystemExit(f"[FAIL] toolchain manifest lacks {key}")
        path = base / entry["path"]
        if not path.is_file():
            raise SystemExit(f"[FAIL] tool not found: {path}")
        resolved[logical] = str(path)
        resolved[logical + "_version"] = str(entry.get("version", ""))
    ctest = Path(resolved["cmake"]).with_name("ctest.exe" if os.name == "nt" else "ctest")
    if not ctest.is_file():
        raise SystemExit(f"[FAIL] ctest not found beside CMake: {ctest}")
    resolved["ctest"] = str(ctest)
    return resolved


def validate() -> dict:
    tools = resolve()
    for logical, flag in (("clang_format", "--version"), ("cmake", "--version")):
        out = subprocess.run([tools[logical], flag], text=True, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, check=False).stdout or ""
        expected = tools[logical + "_version"]
        if expected and expected not in out:
            first = out.splitlines()[0] if out else ""
            raise SystemExit(f"[FAIL] {logical} does not report pinned version "
                             f"{expected}: {first}")
    return tools
