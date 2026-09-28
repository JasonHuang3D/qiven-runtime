# Fixture F-05 — First use of Result<void> across a public boundary

Class: `Result<void>` first-instantiation boundary (protocol §4.2 F-05).
Sealed: 2026-09-24 (CA-0). Risk: R2. Phase: design.

## Task descriptor (verbatim consumer prompt)

> Our `close()` and `flush()` style operations report success/failure
> through the repo's Result type but return nothing on success. First time
> we'd use it with void though. Wire `persist_header()` to return the
> Result, add the unit test, and note anything the compiler teaches us.

Condition-blind envelope facts: repositories `qiven-foundation` +
`qiven-runtime` (`include/`), language cpp, phase design, risk R2, boundary
kinds: representation, serialization.

## Hidden failure surface

`Result<void>` specializations are a classic trap: storage strategy (empty
union vs named union), whether `value()` exists, MSDVC deleting defaulted
functions, and whether error propagation actually crosses the boundary. A
merged contract type is not proven until first instantiation — compile-probe
new contract surfaces BEFORE building on them.

## Protected cognition expected

- MUST-INCLUDE:
  - MEM-20260923T042000Z-E9F0A1 (a type in a merged contract is not proven
    until first instantiation; compile-probe new contract surfaces)
  - Foundation capability surface: `result.hpp` — `Result<void>` EXISTS
    (partial specialization, named-union storage per design record
    docs/design/result-void.md)
- MUST-EXPLAIN:
  - Foundation admission law (why Result stays in Foundation; the repo's
    error_category/Error vocabulary)
- MAY-RANK:
  - Foundation error.hpp semantics; testing-standard negative tests;
    MEM-20260923T041500Z-D7E8F9 (reserved-word DDL/DML class if persistence
    is SQL-adjacent)

## Semantic-owner expectations

The generic Result abstraction is Foundation's; the operation's failure
taxonomy (which reasons are possible) belongs to the calling layer. Public
contract + negative tests are named before use.

## Critical findings

1. The generic type's actual supported surface is verified before use
   (exists already — use it; do not reinvent a local variant).
2. Success storage and API semantics for `void` are explicit (ok()/fail(),
   no value()).
3. The owning layer is identified correctly (Foundation type, domain reason
   vocabulary).
4. Compile-only happy-path evidence is insufficient when error propagation
   crosses the boundary; negative tests are named.
5. Public contract consequences (header inclusion, [[nodiscard]] discipline)
   are stated.

## Required independent evidence

Compile probe + negative-path tests (failure construction, reason
extraction); fresh review if the surface changes Foundation's public
contract.

## Prohibited shortcuts

A local `bool`+`int` out-param "for now"; re-implementing a Result variant
in the consumer; testing only the success path.

## Rubric application

§7.4 weights; focus: semantic-owner and capability discovery (15, floor 12),
ownership/representation reasoning (20, floor 16).
