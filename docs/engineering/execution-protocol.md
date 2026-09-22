# Execution Workflow

Execution workflow for engineering work in this repository. Roles-as-labels,
typed handoffs (H1-H4), authority, and unattended-automation limits
are canonical in `JasonHuang3D/qiven-context`
(`collaboration/operating-contract.md`,
`collaboration/human-handoff-boundary.md`, ADR-0035/ADR-0036/ADR-0044);
supervised local execution follows the active ContextView workflow. This
document repeats none of them and overrides none of them.

## Branches

- Implementation branches use the acting designation or capability
  namespace (for example `jason-extended-cognition/<feature>`,
  `zcode/<feature>`); the legacy label namespaces
  `jason-brother/<feature>` and `jason-worker/<feature>` remain valid.
- Stacked batches keep their serial order; earlier completed layers freeze
  for the batch. A defect found in a frozen layer stops and escalates.
- Never force-push, rewrite `main`, or change Git identity/configuration.

## Validation

- `FULL` and `FOCUSED` validation profiles are defined in
  `testing-standard.md`. The session does not invent reduced scopes;
  `FOCUSED` requires an exact scope from the task specification.
- The final stack tip of a batch receives repository-wide FULL validation
  before publication.
- Prefer the repository Operator (`tools\qiven.py`) for gates and tasks;
  `--verbose` staged output is legible to humans and agents alike.
  Long or unknown-duration commands route through `qiven exec`
  (canonical hang contract).

## Commits

- Conventional subjects. LLM-authored commits carry the attribution
  trailer (`role:` / `LLM:` / reasoning level) as the last block of the
  message, per the canonical operating contract.
- `git diff --check` clean; no unrelated reformatting; register only
  intended new files with the formatting workflow.

## Authoring mechanics (hardened rules)

- File content goes through native file-write tooling, never shell
  heredocs (truncation and byte-corruption defect class, recorded
  2026-09-21). Escape-sensitive content — backslash sequences in
  Windows paths — is additionally verified at byte level after
  authoring: `\f`-shaped sequences silently degrade to control bytes
  in some authoring paths (form-feed defect, caught 2026-09-22), and
  byte-level fixes construct the escape bytes programmatically rather
  than as raw sequences inside shell-authored source.
- `git add -N` is prohibited: an intent-to-add entry holds an empty
  blob, and a later `git checkout -- .` restores that empty blob over
  real content — silent data loss.
- An absolute-path file write must be re-verified in the TARGET
  repository before claiming a landing (the 2026-09-21 stray-tree
  defect: a write resolved into a different repository's tree).
- PR bodies are authored as plain text or with escaped quoting; raw
  backticks in a shell-authored body trigger command substitution and
  can splice repository file content into the body.

## Publication

- No push, PR creation, or merge without explicit owner authorization
  or a standing accepted delegation (execution authority is an
  owner boundary, not a role boundary).
- After any merge-class action, verify the MERGED state explicitly
  (query the PR or remote state); never record a landing from a
  swallowed or unobserved merge-call output (the 2026-09-21
  recorded-merge defect).
- After a merge, reconcile local `main` and delete merged branches
  per the canonical cleanup policy.

## Blockers and escalation

Stop and escalate rather than guessing around architectural ambiguity:
report the branch/commit/worktree state, the exact failing command or
condition, and the decision required. An escalation reports results,
evidence and assumptions — never claim `PASS`, `NONE` or `CLEAN` without
verification.
