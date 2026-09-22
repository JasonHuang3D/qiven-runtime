"""third_party_verify.py — provenance digest verifier (gate task).

Law: qiven-devkit docs/engineering/third-party-dependencies.md section 4.
Recomputes the SHA-256 of every file listed in each
third_party/*/PROVENANCE.yaml and fails closed on any mismatch or
missing record. Exits non-zero naming the offending file.

Zero-dependency by design: PROVENANCE.yaml is this workspace's own
strict subset; parsing it needs no YAML library (and the gate's python
must not grow one for this).
"""

from __future__ import annotations

import hashlib
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
THIRD_PARTY = ROOT / "third_party"

EXPECTED_SCHEMA = "qiven-third-party-provenance-v1"


def parse_provenance(text: str) -> dict:
    """Parse the strict PROVENANCE subset: flat `key: value` lines, one
    nested `archive_digest:` block (algorithm/value), and `files:` as
    `- path:` / `sha256:` item lines. Anything else fails closed."""
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
            body = stripped[2:]
            key, _, value = body.partition(":")
            if key.strip() != "path":
                raise ValueError(f"line {lineno}: files item must start with path: {body!r}")
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
    if not THIRD_PARTY.is_dir():
        print("[ OK ] third-party-verify: no third_party/ directory (nothing vendored)")
        return 0

    failures: list[str] = []
    checked = 0
    for provenance_path in sorted(THIRD_PARTY.glob("*/PROVENANCE.yaml")):
        try:
            record = parse_provenance(provenance_path.read_text(encoding="utf-8"))
        except ValueError as error:
            failures.append(f"{provenance_path}: {error}")
            continue
        if record.get("schema") != EXPECTED_SCHEMA:
            failures.append(f"{provenance_path}: schema {record.get('schema')!r} != {EXPECTED_SCHEMA!r}")
            continue
        base = provenance_path.parent
        for entry in record.get("files", []):
            target = base / entry["path"]
            if not target.is_file():
                failures.append(f"{target}: listed in provenance but missing")
                continue
            actual = sha256_file(target)
            if actual != entry["sha256"]:
                failures.append(f"{target}: sha256 {actual} != recorded {entry['sha256']}")
            checked += 1
        # untracked files inside a vendored tree are a defect too
        listed = {entry["path"] for entry in record.get("files", [])}
        for present in sorted(base.rglob("*")):
            if not present.is_file():
                continue
            rel = present.relative_to(base).as_posix()
            if rel in ("PROVENANCE.yaml", "CMakeLists.txt") or rel.startswith("patches/"):
                continue
            if rel not in listed:
                failures.append(f"{present}: present on disk but absent from PROVENANCE.yaml")

    if failures:
        for failure in failures:
            print(f"[FAIL] third-party-verify: {failure}")
        return 1
    print(f"[ OK ] third-party-verify: {checked} file digest(s) verified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
