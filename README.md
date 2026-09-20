# qiven-runtime

Home of the Qiven Cognitive Control Runtime: the trusted component system
that converts observable participant proposals into policy-derived,
evidence-backed, exact-action decisions at the boundary where Judgment
becomes world-changing Mechanism (ADR-0038).

- Architecture: `docs/architecture/runtime-component-adl.md` (component
  model; frozen v4 semantics of `qiven-context-draft` @ `4cbc995` are the
  immutable semantic dependency).
- Engineering conventions: Devkit `docs/conventions/` (index README there).
- Engineering standards: `docs/engineering/README.md`.

Bootstrap scaffold by qiven-devkit (template 0.1.5); the first
implementation program is RCA-1..RCA-16 in the component ADL. Machine-local
mutation authority remains separately gated by qiven-host (ADR-0026).
