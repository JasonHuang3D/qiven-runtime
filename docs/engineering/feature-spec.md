# Feature Specification Contract

This document defines how implementation-ready work is specified before implementation begins — for owner-assigned and session-planned work alike (single-session unified engineering, ADR-0044). The purpose is not to micromanage code; it is to state architectural intent and semantic boundaries strongly enough that implementation can proceed with senior engineering judgment without guessing public contracts or expanding scope.

## 1. Required batch header

A task batch should normally establish:

```text
WORK BATCH
Base branch:
Base commit: <optional when branch state is authoritative>
Maximum features:
Usage policy:
Batch-final validation:
Publication: per execution-protocol (owner authorization or standing delegation)

AUTHORIZED FEATURE QUEUE
1. <feature A>
2. <feature B>
3. <feature C>

STOP AFTER <last authorized feature>.
```

Feature dependency order must be explicit. Do not leave it implicit whether later work starts from `main` or from an earlier local feature branch.

## 2. Required feature fields

For each feature, define the relevant subset of:

```text
Feature:
Branch:
Base:
Intent:
Required semantics:
Public API:
Ownership/lifetime:
Failure semantics:
Performance/cost constraints:
Platform constraints:
Expected integration:
Required tests:
Validation profile:
Focused validation:
Out of scope:
Stop conditions:
Documentation:
Architectural barrier:
Notes:
```

Use `N/A` when a contract-sensitive field is deliberately irrelevant instead of leaving it accidentally ambiguous.

## 3. Intent and required semantics

Intent should explain why the feature exists in a short paragraph. Required semantics should state externally meaningful behavior and invariants, not prescribe line-by-line implementation.

A strong specification identifies things such as:

- success and valid empty states;
- failure behavior;
- ownership/lifetime and cleanup responsibility;
- moved-from semantics when ownership moves;
- allocator/backend provenance where relevant;
- whether hidden allocation, synchronization, virtual dispatch, or exceptions are allowed;
- portability and platform exposure boundaries;
- observable compatibility requirements.

Ordinary private implementation choices that preserve these semantics remain the implementer's responsibility.

## 4. Public API

When public naming or API shape is architecturally important, specify it or constrain the allowed shape. If public contract design is deliberately unresolved, mark the feature as requiring an owner/architecture decision before implementation rather than encouraging the implementer to invent it.

Do not leave the failure-channel choice — `assert`, optional/result/error code, exception, termination, or silent fallback — undecided when it changes public failure semantics.

## 5. Ownership and lifetime

For ownership-related work, define what is owned and borrowed, nullable/empty states, what must outlive what, cleanup responsibility, move behavior, backend/allocator provenance, and raw-storage versus typed-object lifetime when applicable.

If these points materially affect the contract and are not decided, the feature is not ready for autonomous implementation.

## 6. Failure semantics

Classify meaningful failure channels. Distinguish caller programming errors, invariant violations, recoverable runtime failure, and environmental failure. State whether failure leaves output/state unchanged when that matters.

Do not leave the implementer to invent a failure category because the happy path is obvious.

## 7. Performance and cost

State only concrete constraints that matter, for example:

```text
- no hidden allocation;
- O(1) move;
- no synchronization;
- no virtual dispatch;
- bounded temporary storage;
```

Avoid vague demands such as "high performance" without naming the controlled cost.

## 8. Platform constraints

State special cross-platform requirements when a feature touches native facilities. Public contracts should remain platform-neutral unless native exposure is intentional and approved.

A platform-sensitive feature is a likely architectural barrier when equivalent semantics or ownership cannot be guaranteed locally.

## 9. Expected integration

List required repository integration when useful, such as public-header registration, implementation-source registration, header self-checks, dedicated tests, generated metadata, or architecture documentation.

A correct implementation file that is not integrated into the repository is not a complete feature.

## 10. Required tests

Specify semantic risks rather than only test filenames. Name success, boundary, failure, ownership/move, overflow/alignment, state-preservation, regression, and portability-sensitive cases when they are part of the contract.

The implementer may add another directly relevant test when it exposes a meaningful risk, but should not inflate test count mechanically.

## 11. Validation profile

Every source feature should explicitly state:

```text
Validation profile:
FULL
```

or:

```text
Validation profile:
FOCUSED

Focused validation:
<exact build targets, tests, selectors, or commands>
```

`FULL` is the default when omitted. `FOCUSED` is appropriate only when the affected surface is known well enough to define exact validation. A batch containing focused feature validation should normally require batch-final `FULL` validation.

## 12. Out of scope

Explicitly name adjacent work that must remain unchanged. This is a primary defense against opportunistic redesign, cleanup, dependency additions, format churn, and speculative abstractions.

A feature scope is a boundary, not a minimum.

## 13. Stop conditions

List conditions that require escalation, such as:

- an upstream/public contract must change;
- a new failure abstraction appears necessary;
- platform behavior cannot be made equivalent;
- ownership/lifetime cannot be expressed under the approved contract;
- a third-party dependency appears necessary;
- compiler/build/CI architecture must change;
- an earlier frozen stack layer proves semantically wrong.

Repository-wide stop conditions from `AGENTS.md` always apply even if not repeated.

## 14. Documentation

Say whether architecture or engineering documentation should change. Do not force documentation churn for trivial private implementation details, but update durable architecture when a public architectural contract changes.

## 15. Architectural barrier

Use:

```text
Architectural barrier:
YES
```

when dependent features should wait for owner/remote review before stacking continues.

Use `NO` only when it has been consciously decided that local stacking is safe. If omitted for an obviously cross-cutting feature, the implementer should conservatively stop after completing it.

## 16. Specification quality check

Before starting a batch, verify:

- feature dependencies and bases are explicit;
- public semantics are decided;
- ownership/lifetime and failure behavior are decided where relevant;
- performance/platform constraints name real boundaries;
- out-of-scope work is explicit;
- high-risk test cases are named;
- validation profile is explicit enough to execute;
- focused validation has exact scope;
- batch-final validation policy is clear;
- architectural barriers are identified;
- the queue is finite and reviewable;
- a senior engineer can tell when to stop without needing trivial implementation instructions.

If missing information affects architecture or externally observable behavior, improve the specification or raise an owner/architecture decision point before spending implementation budget.
