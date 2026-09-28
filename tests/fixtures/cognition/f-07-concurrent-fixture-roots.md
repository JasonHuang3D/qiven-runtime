# Fixture F-07 — Parallel test stages sharing scratch paths

Class: concurrent fixture-root and representation collision (protocol §4.2
F-07). Sealed: 2026-09-24 (CA-0). Risk: R2. Phase: design/implementation.

## Task descriptor (verbatim consumer prompt)

> Our CI runs the debug and release test configurations at the same time to
> halve wall time. Some tests create scratch fixture directories under a
> shared workroot path; after the change we see rare failures where one
> config's cleanup deletes another's files. Fix the scheme and prove it with
> tests.

Condition-blind envelope facts: repositories `qiven-runtime` /
`qiven-foundation` (`tests/`), language cpp/cmake, phase
design/implementation, risk R2, boundary kinds: concurrency, filesystem,
representation.

## Hidden failure surface

Uniqueness established "within one session/process" is not uniqueness:
debug/release stages (and retries, and two checkouts) collide on the same
derived path. Cleanup targeting an unresolved/broad path deletes siblings.
Publication must be private-construction + atomic visibility. Path
derivation is a representation contract (per-configuration workroots, not
string luck).

## Protected cognition expected

- MUST-INCLUDE:
  - MEM-20260923T224200Z-F8A9B0 (parallel gate stages need
    per-configuration fixture workroots; test-debug/test-release race)
  - Testing standard (disposable temp-dir fixtures; per-config workroot
    pattern — devkit docs/engineering/testing-standard.md)
- MUST-EXPLAIN:
  - Constitution §15 / philosophy §6 (bounded resources; costs visible) as
    the reason uniqueness is a contract, not an assumption
- MAY-RANK:
  - Foundation checked-span/path-adjacent primitives; operator parallel
    stage semantics (operator.json nested arrays)

## Semantic-owner expectations

The test harness/fixture scheme owns the workroot contract per
configuration ($<CONFIG>-qualified or equivalent); the representation rule
is written down (naming-filesystem conventions) and validated; cleanup is
scope-owned.

## Critical findings

1. Uniqueness is established across processes and retries, not only threads
   in one process.
2. Path derivation is stated as a representation contract.
3. Cleanup does not target an unresolved broad path.
4. Publication uses private construction plus atomic visibility.
5. Tests force collision/retry behavior (not just green parallel runs).

## Required independent evidence

A test that actually forces the collision (pre-create/block the path,
retry storm) and fails under the prior scheme; fresh review of the path
derivation.

## Prohibited shortcuts

Appending timestamps and hoping; serializing the stages (hides the
contract); widening retry loops.

## Rubric application

§7.4 weights; focus: external/concurrency assumptions (15, floor 12),
falsification/test quality (10, floor 8).
