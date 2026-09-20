# Execution Workflow

Mechanical execution workflow for implementation work in this repository.
Roles, typed handoffs (H1-H4), authority, and unattended-automation limits
are canonical in `JasonHuang3D/qiven-context`
(`collaboration/operating-contract.md`,
`collaboration/human-handoff-boundary.md`, ADR-0035/ADR-0036); supervised
local execution follows the active ContextView workflow. This document
repeats none of them and overrides none of them.

## Branches

- Implementation branches: `jason-worker/<feature>`, or the designation
  namespace authorized by the active ContextView (for example
  `jason-extended-cognition/<feature>`).
- Stacked batches keep their serial order; earlier completed layers freeze
  for the batch. A defect found in a frozen layer stops and escalates.
- Never force-push, rewrite `main`, or change Git identity/configuration.

## Validation

- `FULL` and `FOCUSED` validation profiles are defined in
  `testing-standard.md`. The worker does not invent reduced scopes.
- The final stack tip of a batch receives repository-wide FULL validation
  before handoff.
- Prefer the repository Operator (`tools\qiven.py`) for gates and tasks;
  `--verbose` staged output is legible to humans and agents alike.

## Commits

- Conventional subjects. LLM-authored commits carry the attribution trailer
  (`role:` / `LLM:` / reasoning level) as the last block of the message,
  per the canonical operating contract.
- `git diff --check` clean; no unrelated reformatting; register only
  intended new files with the formatting workflow.

## Blockers and handoff

Stop and escalate rather than guessing around architectural ambiguity:
report the branch/commit/worktree state, the exact failing command or
condition, and the decision required. A handoff reports results, evidence
and assumptions — never claim `PASS`, `NONE` or `CLEAN` without
verification.
