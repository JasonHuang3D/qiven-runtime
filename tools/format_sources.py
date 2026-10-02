from __future__ import annotations

"""Format (--fix) or check (--check) tracked AND new (untracked, unignored) C/C++ sources.

Was tools/format.cmd and tools/format-check.cmd (one script, two modes).

FAIL carriers obey the P0 four-element law (ADR-0060 D3; repair batch B3):
WHAT happened (counts + the violated rule), WHY (the check that failed),
bounded evidence (per-source clang-format excerpts with truncation and
omission counts), and a mechanical NEXT action - FIX for --check (the
correction is --fix), DIAGNOSE for --fix failures (a rewrite failure is a
tool condition, not a style verdict). The serialized FAIL carrier stays
within the 8 KiB D3 model-view budget; the PASS carrier is byte-stable.
"""

import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from toolchain import resolve

GLOBS = ("*.c", "*.cc", "*.cpp", "*.cxx", "*.h", "*.hh", "*.hpp", "*.hxx",
         "*.inl", "*.ipp", "*.tpp")

# ADR-0060 D3 budgets for the FAIL carrier: at most 8 inline findings,
# bounded per-finding evidence, whole serialized result under 8 KiB
# (control lines are reserved ahead of diagnostic excerpts).
MAX_FINDINGS_SHOWN = 8
PER_FINDING_EVIDENCE_BYTES = 640
FAIL_CARRIER_BUDGET_BYTES = 8192


def _evidence_excerpt(text: str) -> tuple[str, int, int]:
    """(excerpt, shown_bytes, total_bytes): a UTF-8-safe, newline-aligned
    prefix of one finding's captured clang-format output, bounded by
    PER_FINDING_EVIDENCE_BYTES."""
    raw = text.encode("utf-8", errors="replace")
    if len(raw) <= PER_FINDING_EVIDENCE_BYTES:
        return text.rstrip("\n"), len(raw), len(raw)
    cut = raw[:PER_FINDING_EVIDENCE_BYTES]
    newline = cut.rfind(b"\n")
    if newline > 0:
        cut = cut[:newline]
    return cut.decode("utf-8", errors="replace"), len(cut), len(raw)


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
        print(f"[FAIL] git ls-files failed (exit {listing.returncode}): tracked C/C++ "
              "sources cannot be enumerated outside a readable git work tree")
        print(f"[FAIL] format {mode}: aborted - FIX: run from the repository work "
              "tree (git status reports its state), then re-run")
        return 1
    files = [line for line in listing.stdout.splitlines() if line.strip()]
    # Third-party law (qiven-devkit docs/engineering/
    # third-party-dependencies.md — the Devkit is the law's home; this
    # repository carries no copy, ADR-0046):
    # vendored trees are pristine upstream content under provenance
    # digests — never formatted.
    files = [name for name in files if not name.startswith("third_party/")]
    mode_args = ["--dry-run", "--Werror"] if mode == "--check" else ["-i"]
    failing: list[tuple[str, str]] = []
    for name in files:
        result = subprocess.run([tools["clang_format"], *mode_args, "--style=file", name],
                                cwd=str(root), text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, check=False)
        if result.returncode != 0:
            failing.append((name, result.stdout or ""))
    if not failing:
        print(f"[ OK ] format {mode}: {len(files)} tracked C/C++ sources")
        return 0
    emitted = 0
    shown = 0
    for name, output in failing:
        if shown == MAX_FINDINGS_SHOWN or emitted + 256 > FAIL_CARRIER_BUDGET_BYTES:
            break
        excerpt, shown_bytes, total_bytes = _evidence_excerpt(output)
        lines = [f"[FAIL] {name}"]
        lines.extend(f"  {line}" for line in excerpt.splitlines())
        if shown_bytes < total_bytes:
            lines.append(f"  [evidence: first {shown_bytes} of {total_bytes} captured bytes]")
        block = "\n".join(lines)
        emitted += len(block.encode("utf-8")) + 1
        print(block)
        shown += 1
    omitted = len(failing) - shown
    if omitted:
        print(f"[FAIL] format {mode}: {omitted} further failing source(s) not shown "
              f"(budget: at most {MAX_FINDINGS_SHOWN} findings, "
              f"{FAIL_CARRIER_BUDGET_BYTES} serialized bytes)")
    if mode == "--check":
        print(f"[FAIL] format {mode}: failures={len(failing)} of {len(files)} tracked "
              "C/C++ sources violate .clang-format (rule: clang-format --dry-run "
              "--Werror, style=file) - FIX: python tools/format_sources.py --fix, "
              "then re-run --check")
    else:
        print(f"[FAIL] format {mode}: failures={len(failing)} of {len(files)} sources "
              "could not be rewritten (clang-format -i failed - a tool condition, "
              f"not a style verdict) - DIAGNOSE: \"{tools['clang_format']}\" -i "
              f"--style=file {failing[0][0]}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
