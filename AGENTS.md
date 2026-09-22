# qiven-runtime Agent Contract

qiven-runtime is a Devkit-managed native repository; see `README.md` for
its mission and current status.

- Engineering conventions (naming, layout, scripts, CMake) are canonical in
  the Devkit: `docs/conventions/README.md` there, including the agent-entry
  rule (`agent-entry.md`). Read that index before creating files, folders,
  branches or targets.
- Engineering standards (implementation, testing, execution, specification)
  are ALSO canonical in the Devkit: `docs/engineering/README.md` there
  (ADR-0046 — repositories carry no copies; this pointer is the entry).
- Architecture documents, where present: `docs/architecture/`.

Roles, typed handoffs, execution authority and workflow are canonical in
`JasonHuang3D/qiven-context` (collaboration contracts, loaded at cold boot).
This file grants no authority and repeats no contracts.
