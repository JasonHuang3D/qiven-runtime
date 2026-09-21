from __future__ import annotations

"""Format (--fix) or check (--check) tracked AND new (untracked, unignored) C/C++ sources.

Was tools/format.cmd and tools/format-check.cmd (one script, two modes).
"""

import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from toolchain import resolve

GLOBS = ("*.c", "*.cc", "*.cpp", "*.cxx", "*.h", "*.hh", "*.hpp", "*.hxx",
         "*.inl", "*.ipp", "*.tpp")


def main() -> int:
    mode = sys.argv[1] if len(sys.argv) > 1 else ""
    if mode not in ("--check", "--fix"):
        print("usage: format_sources.py --check|--fix")
        return 2
    tools = resolve()
    root = Path(__file__).resolve().parent.parent
    listing = subprocess.run(["git", "-C", str(root), "ls-files", "--cached", "--others", "--exclude-standard", "--", *GLOBS],
                             text=True, stdout=subprocess.PIPE, check=False)
    if listing.returncode != 0:
        print("[FAIL] git ls-files failed")
        return 1
    files = [line for line in listing.stdout.splitlines() if line.strip()]
    mode_args = ["--dry-run", "--Werror"] if mode == "--check" else ["-i"]
    failures = 0
    for name in files:
        result = subprocess.run([tools["clang_format"], *mode_args, "--style=file", name],
                                cwd=str(root), text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, check=False)
        if result.returncode != 0:
            failures += 1
            print(f"[FAIL] {name}")
            print(result.stdout or "")
    if failures:
        print(f"[FAIL] format {mode}: failures={failures}")
        return 1
    print(f"[ OK ] format {mode}: {len(files)} tracked C/C++ sources")
    return 0


if __name__ == "__main__":
    sys.exit(main())
