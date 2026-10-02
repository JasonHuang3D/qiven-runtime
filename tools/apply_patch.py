from __future__ import annotations

"""Apply a remote-authored candidate patch to the local working copy.

History: this task was previously named `apply-jason-brother.cmd` and encoded an
older workflow name. The semantics are unchanged: the remote authoring agent
produces one patch file, the human applies it locally with git apply.

The old script chained formatting and solution regeneration; that chaining now
belongs to the Operator's declared `local` gate. This task does one thing:
verify the tree is clean, apply `candidate.patch` atomically, and remove it.

FAIL carriers obey the P0 four-element law (ADR-0060 D3; repair batch B3):
each refusal states WHAT was refused, WHY (the atomic-apply law: a patch
lands on exactly a clean HEAD, so every refusal leaves the tree untouched),
bounded git evidence with an explicit omission count, and a mechanical NEXT
action (FIX for invocation-shaped refusals; DIAGNOSE when the patch content
does not match HEAD). The success carrier is byte-stable.
"""

import subprocess
import sys
from pathlib import Path

PATCH_NAME = "candidate.patch"
EVIDENCE_LINE_CAP = 40


def run(argv: list[str], *, check: bool = True) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(argv, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if check and result.returncode != 0:
        print(result.stdout or "")
        raise SystemExit(
            f"[FAIL] command failed: {' '.join(argv)} - DIAGNOSE: the evidence "
            "above is the command's own output")
    return result


def _bounded_evidence(text: str) -> str:
    """Newline-aligned evidence prefix with an explicit omission count
    (ADR-0060 D3: a bounded excerpt, never the unbounded body)."""
    lines = text.splitlines()
    if len(lines) <= EVIDENCE_LINE_CAP:
        return text.rstrip("\n")
    kept = lines[:EVIDENCE_LINE_CAP]
    omitted = len(lines) - len(kept)
    return "\n".join(kept) + f"\n[... {omitted} evidence line(s) omitted ...]"


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    patch = root / PATCH_NAME

    if not patch.exists():
        print(f"[FAIL] Patch not found: {patch}")
        print("[FAIL] apply-patch: nothing to apply (the candidate patch is "
              "this tool's only input) - FIX: place it at the repository root "
              "as candidate.patch, then re-run python tools/apply_patch.py")
        return 1

    def git(*args: str, check: bool = True) -> subprocess.CompletedProcess[str]:
        return run(["git", "-C", str(root), *args], check=check)

    dirty = git("diff", "--quiet", check=False)
    staged = git("diff", "--cached", "--quiet", check=False)
    if dirty.returncode != 0 or staged.returncode != 0:
        print("[FAIL] Working tree has tracked changes. Commit or revert them first.")
        print("[FAIL] apply-patch: atomic-apply law - a patch must land on exactly "
              "a clean HEAD, so this refusal leaves the tree untouched - FIX: "
              "git status, commit or revert the tracked changes, then re-run "
              "python tools/apply_patch.py")
        return 1

    print(f"[ RUN] checking {PATCH_NAME}")
    check = git("apply", "--ignore-space-change", "--check", patch.name, check=False)
    if check.returncode != 0:
        print(_bounded_evidence(check.stdout or ""))
        print(f"[FAIL] {PATCH_NAME} does not apply cleanly.")
        print("[FAIL] apply-patch: the patch context does not match the current "
              "HEAD (evidence above is git apply --check) - DIAGNOSE: compare "
              f"the failed hunks with the current files (git apply --check "
              f"{PATCH_NAME} reproduces this), then regenerate the patch from "
              "its source revision")
        return 1

    applied = git("apply", "--ignore-space-change", patch.name, check=False)
    if applied.returncode != 0:
        print(_bounded_evidence(applied.stdout or ""))
        print(f"[FAIL] {PATCH_NAME} was not applied.")
        print("[FAIL] apply-patch: git apply refused the write (evidence above) - "
              f"DIAGNOSE: re-run git apply --ignore-space-change {PATCH_NAME} "
              "directly and read its refusal; the tree is unchanged")
        return 1

    whitespace = git("diff", "--check", check=False)
    if whitespace.returncode != 0:
        print(_bounded_evidence(whitespace.stdout or ""))
        print("[FAIL] Applied patch introduced whitespace errors; inspect git diff.")
        print("[FAIL] apply-patch: the applied change violates the whitespace rules "
              "(evidence above is git diff --check) - FIX: correct the flagged "
              f"lines and commit; {PATCH_NAME} was applied but kept for reference "
              "- remove it once the change is committed")
        return 1

    patch.unlink()
    print(f"[ OK ] Patch applied successfully to {root.name}; {PATCH_NAME} removed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
