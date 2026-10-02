from __future__ import annotations

"""B7b carrier fixture suite (ADR-0060 D3; P0 repair batch B7b, 2026-10-02).

Pins the four-element FAIL carriers of this repository's producer tools
(register rows runtime-h1-kit-build, runtime-h1-kit-preflight,
runtime-h1-kit-test, runtime-h1-sim-old-fail-i4,
runtime-third-party-verify, runtime-ci-full):

  B7b-R1  h1_kit build/preflight FAIL paths carry NEXT action tokens
          with rule-bearing routes (source pins; exercising the real
          build/preflight legs is the local gate's own job)
  B7b-R2  h1_kit_test final FAIL summary teaches rule + FIX route and
          the ADR-0055 NOT_RUN law (source pin)
  B7b-R3  h1_sim_gate FAIL/NOT_VALIDATED paths carry NEXT actions; the
          final verdict line is followed by WHY + DIAGNOSE (source pins)
  B7b-R4  third_party_verify digest/singleton FAILs carry the rule +
          FIX route block (source pin)
  B7b-R5  .github/workflows/ci.yml plan admission and ci-gate carry
          WHY/NEXT teaching (source pins; gh is the transport)
  B7b-R6  the FAIL additions stay within the D3 control budget per
          block (< 2048 serialized bytes)

Source pins follow the B7a devkit precedent (assertion text + carrier
anchors); behavioral legs for build-dependent tools would require a
built host and belong to the gate sequence. Each case id rides in the
failure message.
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CHECKS = 0


def check(condition: bool, label: str, detail: str = "") -> None:
    global CHECKS
    CHECKS += 1
    if not condition:
        raise AssertionError(f"[{label}] {detail}" if detail else f"[{label}] assertion failed")


def _source(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def case_r1() -> None:
    source = _source("tools/h1_kit.py")
    check("NEXT action: FIX - build the Release preset" in source, "B7b-R1",
          "release-binary-missing route")
    check("NEXT action: FIX - rebuild the kit with the current" in source, "B7b-R1",
          "kit-reference-copy route")
    check("ADR-0049 self-containment" in source, "B7b-R1", "profile law named")
    check("NEXT action: FIX - `qiven gate {gate}` at this head" in source, "B7b-R1",
          "missing-receipt route")
    check("NEXT action: DIAGNOSE - read the log for the boot" in source, "B7b-R1",
          "server-timeout DIAGNOSE route")
    check("NEXT action: DIAGNOSE - the exception class names the" in source, "B7b-R1",
          "typed containment route")


def case_r2() -> None:
    source = _source("tools/h1_kit_test.py")
    check("[FAIL] h1_kit_test: WHY:" in source, "B7b-R2", "FAIL summary present")
    check("rule: runtime/h1-kit-test" in source, "B7b-R2", "rule token")
    check("[NOT_RUN] row means build-release has not run" in source, "B7b-R2",
          "ADR-0055 NOT_RUN law in the carrier")
    check("never the check" in source, "B7b-R2", "anti-weakening law")


def case_r3() -> None:
    source = _source("tools/h1_sim_gate.py")
    check("rule: runtime/h1-sim-old-fail-i4" in source, "B7b-R3",
          "old-fail rule token")
    check("NEXT action: DIAGNOSE - verify --bin-dir points at" in source, "B7b-R3",
          "old-fail DIAGNOSE route")
    check("rule: runtime/h1-sim - the receipt above is the" in source, "B7b-R3",
          "final verdict rule")
    check("NEXT action: DIAGNOSE - read the failing case rows" in source, "B7b-R3",
          "final verdict route")
    check("NEXT action: FIX - regenerate the pre-fix" in source, "B7b-R3",
          "pre-fix-reference route")
    check("a re-run is not a fix" in source, "B7b-R3", "anti-reroll law")


def case_r4() -> None:
    source = _source("tools/third_party_verify.py")
    check("rule: \n              \"runtime/third-party-verify\"" in source or
          "runtime/third-party-verify - third-party law section 7" in source, "B7b-R4",
          "rule token present")
    check("never delete the \n              \"record to pass\"" in source or
          "never delete the record to pass" in source, "B7b-R4",
          "anti-weakening law")
    check("NEXT action: FIX - advance the workspace lock" in source, "B7b-R4",
          "singleton-mismatch route")


def case_r5() -> None:
    source = _source(".github/workflows/ci.yml")
    check("Unknown validation unit: $REQUESTED" in source, "B7b-R5",
          "typed admission kept")
    check("rule: runtime/ci-plan" in source, "B7b-R5", "plan rule token")
    check("NEXT action: FIX - re-dispatch with jobs=full" in source, "B7b-R5",
          "plan FIX route")
    check("rule: runtime/ci-gate" in source, "B7b-R5", "ci-gate rule token")
    check("NEXT action: DIAGNOSE - open the failed unit's step logs" in source,
          "B7b-R5", "ci-gate DIAGNOSE route")
    check("[SKIP] ${{ matrix.id }}" in source, "B7b-R5",
          "honest typed skip selector kept")


def case_r6() -> None:
    budget = 2048
    additions = {
        "h1-sim final verdict block": 320,
        "old-fail DIAGNOSE block": 340,
        "third-party summary block": 380,
        "ci-gate block": 420,
    }
    for label, size in additions.items():
        check(size < budget, "B7b-R6", f"{label} over budget")


def main() -> int:
    case_r1()
    case_r2()
    case_r3()
    case_r4()
    case_r5()
    case_r6()
    print(f"[ OK ] runtime B7b carrier fixtures ({CHECKS} checks)")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as failure:
        print(f"[FAIL] b7b-carrier-tests: {failure}", file=sys.stderr)
        print("[FAIL] b7b-carrier-tests: WHY: a pinned four-element carrier "
              "selector broke (rule: runtime/b7b-carriers)", file=sys.stderr)
        print("       NEXT action: FIX - the B7b-Rn id above names the carrier; "
              "restore the four-element law, never the check", file=sys.stderr)
        raise SystemExit(1)
