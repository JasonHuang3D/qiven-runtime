from __future__ import annotations

"""Apply a remote-authored candidate patch to the local working copy.

History: this task was previously named `apply-jason-brother.cmd` and encoded an
older workflow name. The semantics are unchanged: the remote authoring agent
produces one patch file, the human applies it locally with git apply.

The old script chained formatting and solution regeneration; that chaining now
belongs to the Operator's declared `local` gate. This task does one thing:
verify the tree is clean, apply `candidate.patch` atomically, and remove it.
"""

import subprocess
import sys
from pathlib import Path

PATCH_NAME = "candidate.patch"


def run(argv: list[str], *, check: bool = True) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(argv, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if check and result.returncode != 0:
        print(result.stdout or "")
        raise SystemExit(f"[FAIL] command failed: {' '.join(argv)}")
    return result


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    patch = root / PATCH_NAME

    if not patch.exists():
        print(f"[FAIL] Patch not found: {patch}")
        return 1

    def git(*args: str, check: bool = True) -> subprocess.CompletedProcess[str]:
        return run(["git", "-C", str(root), *args], check=check)

    dirty = git("diff", "--quiet", check=False)
    staged = git("diff", "--cached", "--quiet", check=False)
    if dirty.returncode != 0 or staged.returncode != 0:
        print("[FAIL] Working tree has tracked changes. Commit or revert them first.")
        return 1

    print(f"[ RUN] checking {PATCH_NAME}")
    check = git("apply", "--ignore-space-change", "--check", patch.name, check=False)
    if check.returncode != 0:
        print(check.stdout or "")
        print(f"[FAIL] {PATCH_NAME} does not apply cleanly.")
        return 1

    applied = git("apply", "--ignore-space-change", patch.name, check=False)
    if applied.returncode != 0:
        print(applied.stdout or "")
        print(f"[FAIL] {PATCH_NAME} was not applied.")
        return 1

    whitespace = git("diff", "--check", check=False)
    if whitespace.returncode != 0:
        print(whitespace.stdout or "")
        print("[FAIL] Applied patch introduced whitespace errors; inspect git diff.")
        return 1

    patch.unlink()
    print(f"[ OK ] Patch applied successfully to {root.name}; {PATCH_NAME} removed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
