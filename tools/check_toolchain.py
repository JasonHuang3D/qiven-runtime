from __future__ import annotations

"""Gate task: prove the pinned toolchain is actually installed.

Was tools/resolve-toolchain.cmd; the version pins now come from the
toolchain.json manifest instead of being hardcoded here.
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from toolchain import validate


def main() -> int:
    tools = validate()
    print("[ OK ] toolchain validated: "
          f"cmake={tools['cmake_version']} clang-format={tools['clang_format_version']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
