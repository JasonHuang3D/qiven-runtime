from __future__ import annotations

"""Delete every local and origin branch except main, all-or-nothing.

Refuses on a dirty tree, a missing origin, or any candidate not merged into
main (local) / origin/main (remote). Idempotent when there are no candidates.
Was tools/delete-all-branches-but-main.cmd.
"""

import os
import os
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent

def git(*args: str, check: bool = True) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(["git", *args], cwd=str(ROOT), text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, check=False)
    if check and result.returncode != 0:
        print(result.stdout or "")
        raise SystemExit(f"[FAIL] git {' '.join(args)}")
    return result


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    work_tree = git("rev-parse", "--show-toplevel", check=False).stdout.strip()
    if not work_tree or os.path.normcase(Path(work_tree).resolve()) != os.path.normcase(root.resolve()):
        print("[FAIL] script repository is not the work tree root:")
        print(f"       script: {root}")
        print(f"       git:    {work_tree}")
        return 1

    dirty = git("diff", "--quiet", check=False)
    staged = git("diff", "--cached", "--quiet", check=False)
    if dirty.returncode != 0 or staged.returncode != 0:
        print("[FAIL] Working tree has uncommitted changes. Commit or revert first.")
        return 1

    git("remote", "get-url", "origin")
    git("switch", "main")
    git("fetch", "origin", "--prune")
    git("pull", "--ff-only", "origin", "main")
    git("fetch", "origin", "--prune")

    local = [line.removeprefix("refs/heads/")
             for line in git("for-each-ref", "--format=%(refname)",
                             "refs/heads/").stdout.splitlines()
             if line.strip() and line.strip() != "refs/heads/main"]
    remote = []
    for line in git("for-each-ref", "--format=%(refname)",
                    "refs/remotes/origin/").stdout.splitlines():
        name = line.strip().removeprefix("refs/remotes/origin/")
        if name and name != "HEAD" and name != "main":
            remote.append(name)

    print("[ OK ] local candidates: " + (", ".join(local) or "(none)"))
    print("[ OK ] remote candidates: " + (", ".join(remote) or "(none)"))

    blocked = []
    for branch in local:
        if git("merge-base", "--is-ancestor", f"refs/heads/{branch}",
               "refs/heads/main", check=False).returncode != 0:
            blocked.append(f"local {branch} is not merged into main")
    for branch in remote:
        if git("merge-base", "--is-ancestor", f"refs/remotes/origin/{branch}",
               "refs/remotes/origin/main", check=False).returncode != 0:
            blocked.append(f"remote {branch} is not merged into origin/main")
    if blocked:
        for reason in blocked:
            print(f"[FAIL] {reason}")
        print("[FAIL] Safety preflight failed. No branches were deleted.")
        return 1

    for branch in local:
        git("branch", "-D", branch)
    for branch in remote:
        git("push", "origin", "--delete", branch)
    print(f"[ OK ] deleted local={len(local)} remote={len(remote)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
