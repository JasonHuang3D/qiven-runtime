# qiven-runtime Engineering Protocol

This directory contains the shared repository-level engineering protocol.
It applies to every implementer — human or AI session — unchanged
(single-session unified engineering, ADR-0044 in `JasonHuang3D/qiven-context`).

Repository architecture remains repository-owned. These documents define **how** implementation work is performed safely and consistently; they do not replace domain or architectural contracts.

## Documents

- `implementation-standard.md` — implementation quality, scope discipline, C++ design, dependency, portability, and review rules.
- `testing-standard.md` — semantic test design, validation profiles, adversarial/concurrency posture, and local evidence requirements.
- `execution-protocol.md` — branch, batch, commit, validation, publication, stopping, blocker, and escalation procedure.
- `feature-spec.md` — the specification contract used to define implementation-ready work before implementation begins (owner-authored or session-authored alike).

## The single-session engineering loop

```text
authorize task (owner)
    -> specify (feature-spec discipline: semantics, scope, validation profile)
    -> implement (implementation-standard) and design tests (testing-standard)
    -> validate at the exact head (execution-protocol profiles)
    -> review of the exact delta (owner H2, or delegated reviewer)
    -> publish (push / PR / merge) and reconcile
    -> record durable cognition (context transaction when material)
```

One session performs the whole loop. There are no role-stage handoffs
inside it; publication authority separates at the owner boundary (typed
handoffs H1-H4), not between agent roles. Cross-session continuity is
owned by the canonical qiven-context cold boot plus session checkpoints,
never by role-to-role handoffs.

## Standard engineering posture

- **Maximum reasoning depth is the default.** Shallowness is a defect, not a conservation measure.
- **Think beyond the literal instruction.** When the instruction as stated would produce a suboptimal or fragile outcome, say so, explain why, and propose the better alternative. Proactively flag risks, edge cases, and design smells the requester may not have considered. Silence about a known problem is a defect.
- **Semantic ownership first.** Before implementing a capability, determine where it naturally belongs in the dependency hierarchy; improve the lower owner rather than copying a weaker local version (canonical `software-engineering-philosophy.md`).
- **Evidence over claims.** Never claim `PASS`, `CLEAN` or `NONE` without verification; report what was run, at which exact head, with what result and what remained unproven.

## Instruction precedence

Use the precedence defined by the root `AGENTS.md`. A feature specification may specialize ordinary implementation details for one feature, but it may not silently override repository-wide architecture or safety rules. Deliberate exceptions must be explicit.

## Protocol evolution

These documents are expected to evolve when evidence shows that the existing protocol failed to prevent a recurring class of mistake.

When a process failure occurs, ask:

1. Was the feature specification ambiguous?
2. Was a shared engineering rule missing?
3. Was the rule present but too vague to be operational?
4. Was it contradicted by another instruction?
5. Should the rule become an automated check instead of prose?

Fix the underlying protocol or detector when appropriate rather than relying on the same warning being remembered manually in future sessions.
