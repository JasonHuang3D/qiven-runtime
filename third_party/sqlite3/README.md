# third_party/sqlite3 — vendored SQLite 3 amalgamation (3.53.4)

Slot TP-1 of the runtime detailed C++ design (§17): the durable control
journal substrate for the Runtime Production MVP (ADR-0047; MVP-1).

- Mode M1 (vendored source) per the Devkit standard
  `docs/engineering/third-party-dependencies.md` (qiven-devkit).
- Consumed ONLY as the `qiven::tp::sqlite3` target
  (`third_party/sqlite3/CMakeLists.txt`); compile-flag adaptation is
  scoped to that target and never leaks into first-party targets.
- Files: sqlite3.c, sqlite3.h, sqlite3ext.h (amalgamation, pristine).
- Provenance + digests: PROVENANCE.yaml (machine-verified by the
  `third-party-verify` gate task). Upstream archive digest:
  SHA3-256 628a44cf...934e (sqlite.org download page, 2026-09-23).
- License: Public Domain (LICENSE).
