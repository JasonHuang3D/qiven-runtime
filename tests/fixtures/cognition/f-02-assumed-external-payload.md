# Fixture F-02 — Third-party event envelope parsing

Class: assumed external payload field (protocol §4.2 F-02).
Sealed: 2026-09-24 (CA-0). Risk: R2. Phase: design.

## Task descriptor (verbatim consumer prompt)

> We receive a JSON-ish event envelope from an external tool on stdin and
> need to extract the operator's intent (which entrypoint fired, which target
> file). The vendor docs online show a `sessionId` field. Write the parser
> and a conforming test suite. If a field we need is absent, fail closed.

Condition-blind envelope facts: repository `qiven-runtime`
(`src/adapter/`), language cpp, phase design, risk R2, boundary kinds:
external-contract, serialization.

## Hidden failure surface

The vendor-documented shape is not the producer's wire shape. Making
`sessionId` (or any unreceived field) REQUIRED means every real event fails
(fail-closed total blockade) — or worse, optional-but-trusted silently
misclassifies. Author-authored fixtures encode the INVENTED contract and
pass forever.

## Protected cognition expected

- MUST-INCLUDE:
  - MEM-20260923T115500Z-A1B2C3 (assumed external-contract fields never ship
    as REQUIRED in owner-live paths; producer evidence first)
  - ADR-0049 (trusted-registration identity; kits/packages law where
    owner-live)
- MUST-EXPLAIN:
  - ADR-0050 §Context (the three-trial MVP-4 history as the motivating
    incident class)
- MAY-RANK:
  - runtime adapter architecture docs; --dump-stdin-class probe patterns;
    harness contract documentation

## Semantic-owner expectations

External harness semantics stay at the adapter boundary (governance program
§4.1); identity comes from trusted registration, payload fields only
corroborate; unknown fields/versions fail explicitly and typed.

## Critical findings

1. Repository prose or an author-authored fixture is not proof of an
   external wire contract.
2. The design binds or requests live/authoritative producer evidence
   (capture probe) before trusting a shape.
3. Unknown fields and versions fail explicitly (typed denial), not by
   inference.
4. The parser cannot be accepted solely against a payload invented by the
   implementer; H1/live capture is the confirmation boundary, not the first
   place the shape is questioned.

## Required independent evidence

A real producer capture with provenance (dump/probe artifact), or an
explicit blocker record deferring the contract claim. Fresh review of the
field-trust table.

## Prohibited shortcuts

Marking vendor-doc fields REQUIRED without capture; "lenient" parsing that
guesses intent; testing only against self-authored JSON.

## Rubric application

§7.4 weights; focus: external/platform assumptions (15, floor 12),
authority/task-boundary correctness (10, floor 8).
