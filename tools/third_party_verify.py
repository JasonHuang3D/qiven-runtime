"""third_party_verify.py — consumer spot-verification (gate task).

Law: qiven-devkit docs/engineering/third-party-dependencies.md v2
(section 5.4 — defense in depth; the singleton's own gate is the
authority). Resolves the singleton checkout (QIVEN_THIRD_PARTY_ROOT ->
sibling default), verifies EVERY package provenance there against the
consumer's recorded pin expectation, and fails closed on mismatch.
Zero-dependency by design.
"""

from __future__ import annotations

import hashlib
import os
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
PIN_FILE = ROOT / "CMakeLists.txt"
PIN_MARKER = "QIVEN_THIRD_PARTY_PIN \""
EXPECTED_SCHEMA = "qiven-third-party-provenance-v2"


def resolve_singleton() -> pathlib.Path:
    override = os.environ.get("QIVEN_THIRD_PARTY_ROOT")
    if override:
        candidate = pathlib.Path(override)
        if (candidate / "packages").is_dir():
            return candidate
        raise SystemExit(f"QIVEN_THIRD_PARTY_ROOT={override} has no packages/; refusing")
    sibling = ROOT.parent / "qiven-third-party-win"
    if (sibling / "packages").is_dir():
        return sibling
    raise SystemExit("qiven-third-party-win not found (env QIVEN_THIRD_PARTY_ROOT or sibling layout); refusing")


def recorded_pin() -> str:
    for line in PIN_FILE.read_text(encoding="utf-8").splitlines():
        if line.strip().startswith(PIN_MARKER.replace(" \"", " \"")) or PIN_MARKER in line:
            start = line.index(PIN_MARKER) + len(PIN_MARKER)
            end = line.index('"', start)
            return line[start:end]
    raise SystemExit("no QIVEN_THIRD_PARTY_PIN found in CMakeLists.txt")


def singleton_head(singleton: pathlib.Path) -> str:
    completed = subprocess.run(
        ["git", "-C", str(singleton), "rev-parse", "HEAD"], capture_output=True, text=True, check=False
    )
    if completed.returncode != 0:
        raise SystemExit(f"cannot read singleton HEAD at {singleton}")
    return completed.stdout.strip()


def parse_provenance(text: str) -> dict:
    """Strict subset parser (the workspace's shared provenance grammar)."""
    record: dict = {"files": []}
    section = None
    item = None
    for lineno, raw in enumerate(text.splitlines(), start=1):
        line = raw.rstrip("\n")
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        indent = len(line) - len(line.lstrip(" "))
        stripped = line.strip()
        if indent == 0:
            if stripped == "files:":
                section, item = "files", None
                continue
            if stripped == "archive_digest:":
                section, item = "archive_digest", None
                record["archive_digest"] = {}
                continue
            if ":" in stripped:
                key, _, value = stripped.partition(":")
                record[key.strip()] = value.strip().strip("'\"")
                section, item = None, None
                continue
            raise ValueError(f"line {lineno}: unparseable {stripped!r}")
        if indent == 2 and stripped.startswith("- "):
            if section != "files":
                raise ValueError(f"line {lineno}: list item outside files: {stripped!r}")
            key, _, value = stripped[2:].partition(":")
            if key.strip() != "path":
                raise ValueError(f"line {lineno}: files item must start with path")
            item = {"path": value.strip()}
            record["files"].append(item)
            continue
        if indent >= 2 and ":" in stripped and not stripped.startswith("- "):
            key, _, value = stripped.partition(":")
            key, value = key.strip(), value.strip().strip("'\"")
            if section == "files" and item is not None:
                item[key] = value
            elif section == "archive_digest":
                record["archive_digest"][key] = value
            else:
                raise ValueError(f"line {lineno}: stray field {stripped!r}")
            continue
        raise ValueError(f"line {lineno}: unparseable {stripped!r}")
    return record


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> int:
    singleton = resolve_singleton()
    pin = recorded_pin()
    head = singleton_head(singleton)
    if head != pin:
        print(f"[FAIL] third-party-verify: singleton at {head[:12]} != recorded pin {pin[:12]}; re-pin deliberately")
        return 1

    failures: list[str] = []
    checked = 0
    for provenance_path in sorted((singleton / "packages").glob("*/PROVENANCE.yaml")):
        package = provenance_path.parent
        try:
            record = parse_provenance(provenance_path.read_text(encoding="utf-8"))
        except ValueError as error:
            failures.append(f"{provenance_path}: {error}")
            continue
        if record.get("schema") != EXPECTED_SCHEMA:
            failures.append(f"{package.name}: schema {record.get('schema')!r} != {EXPECTED_SCHEMA!r}")
            continue
        for entry in record.get("files", []):
            target = package / entry["path"]
            if not target.is_file():
                failures.append(f"{target}: listed but missing")
                continue
            if sha256_file(target) != entry["sha256"]:
                failures.append(f"{target}: digest mismatch")
            checked += 1
    if failures:
        for failure in failures:
            print(f"[FAIL] third-party-verify: {failure}")
        return 1
    print(f"[ OK ] third-party-verify: singleton {head[:12]} (== pin); {checked} file digest(s) verified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
