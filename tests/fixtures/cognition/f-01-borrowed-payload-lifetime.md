# Fixture F-01 — Deferred command buffer lifetime

Class: borrowed payload lifetime across execution (protocol §4.2 F-01).
Sealed: 2026-09-24 (CA-0). Risk: R2. Phase: design.

## Task descriptor (verbatim consumer prompt)

> In our Windows service prototype (C++20) we queue little command records
> for a worker thread: producers call `submit(std::span<const std::byte> body)`
> and the record is executed later on the worker. Implement `CommandQueue::
> submit` plus the worker drain loop, with tests. The body arrives from a
> caller that builds it into a local buffer.

Condition-blind envelope facts: repository `qiven-runtime` (path prefix
`src/`), language cpp, phase design→implementation, risk R2, boundary kinds
mechanically derivable: ownership, lifetime, concurrency.

## Hidden failure surface

A `submit` that stores the span (or a `string_view`/`span` member) is
storage-by-rental: the caller's local buffer dies at the end of the calling
statement; the worker reads freed memory. An immediate-drain test cannot see
it (the memory is still warm); only the deferred path under reuse pressure
(or ASan) exposes it.

## Protected cognition expected

- MUST-INCLUDE:
  - MEM-20260924T032100Z-D4E5F6 (owning storage across delayed boundaries;
    delayed path is the tested path)
  - Software-engineering philosophy §1/§9 (semantic ownership; first
    implementation quality bar: explicit ownership/lifetime)
  - Foundation capability surface: owning-buffer primitives status
    (capability-surface inventory) for the make-vs-reuse decision
- MUST-EXPLAIN:
  - ADR-0024 admission criteria (why CommandQueue's owning buffer is the
    domain owner's job, not a Foundation wrapper by default)
- MAY-RANK:
  - qiven-foundation memory/ownership primitives (OwnedArray, LinearArena)
    as implementation candidates; runtime architecture queue/journal notes

## Semantic-owner expectations

The Runtime/domain type that stores work across the boundary owns the bytes
(`std::vector<std::byte>` / owning buffer). Foundation owns only generic
owning/bounded mechanics if admitted; it never owns "a command record".

## Critical findings (binary; a miss is a critical miss)

1. A view is not storage; the outlives relation is explicit and proven.
2. A temporary container cannot back a later-executed payload.
3. The owning type is the queueing domain type, not an arbitrary lower-layer
   wrapper.
4. The test exercises the deferred path under a lifetime-sensitive detector
   or adversarial reuse (not just immediate drain).

## Required independent evidence

ASan (or adversarial heap-reuse) run of the deferred drain path; fresh review
of the ownership contract. Author-authored happy-path drain alone is
corroboration, not falsification.

## Prohibited shortcuts

Copying the span into ANOTHER temporary captured by a lambda; documenting
"caller must keep alive" without a type-level contract; testing only
submit-then-immediately-drain.

## Rubric application

Standard §7.4 weights; dimension focus: ownership/lifetime/representation
reasoning (20) and falsification/test quality (10); critical floor 16/8.
