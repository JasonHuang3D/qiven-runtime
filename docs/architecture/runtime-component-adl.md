# Runtime Component ADL — Cognitive Control Runtime Component Model

**Status:** accepted (owner adjudication 2026-09-21; landed in this repository the same day)
**Architecture level:** Runtime ADL / component model
**Normative upstream:** ADR-0038
**Frozen semantic dependency:** `qiven-context-draft` `4cbc99549c70ef1caa5ccfd6bc15f95607b10322`
**Canonical baseline reviewed:** `qiven-context/main` `e1623290d3bb2adfc7aeb651b3e8341507c5aefc`
**Authority constraints:** ADR-0026, ADR-0033, ADR-0035, ADR-0036
**Next boundary after acceptance:** detailed C++ interface/type design and first Runtime landing plan

---

## 1. Purpose

ADR-0038 decides **where Runtime Cognitive Control sits causally**:

```text
Judgment proposes
    ↓
Harness observes
    ↓
Cognitive Control derives and satisfies mandatory requirements
    ↓
exact action is allowed / denied / returned for re-deliberation
    ↓
Mechanism
    ↓
Outcome observation
```

This document decides the next layer:

> **What concrete logical components exist, what each component owns, what data crosses each port, where trust lies, how concurrent streams are correlated, and how one exact proposal travels from interception to execution and observation without creating a second cognition authority or a bypassable control plane.**

This ADL is still above detailed C++ API design.

It fixes:

```text
component responsibilities
component ownership
component lifetimes
coherency boundaries
logical ports
data direction
trust direction
concurrency model
decision binding
resolver binding
runtime-state recovery
execution outcome state machine
canonical-cognition interaction
deployment-profile semantics
```

It deliberately does **not** fix:

```text
final C++ class names
method signatures
serialization formats
IPC wire format
database/storage product
ZCode-specific hook syntax
MCP-specific schemas
final executable names
canonical authority cutover
free-text claim classification implementation
```

---

# 2. Hard constraints inherited from ADR-0038

This ADL MUST preserve all of the following:

```text
1. Cognitive Control is external to Judgment.

2. Mandatory control does not depend on participant self-invocation.

3. ObservedAction and semantic ActionIntent are distinct.

4. Action/claim triggers and RequirementBoundary deadlines are orthogonal.

5. BeforeJudgment requirements may be discovered after a provisional
   proposal, but that proposal cannot continue until re-deliberation
   occurs with the requirement satisfied.

6. Requirement satisfaction comes from trusted mechanisms, not
   participant assertion.

7. Complete mediation is scoped by:
   DeploymentProfile
   + GovernedActorSet
   + CapabilityUniverse
   + MediationInventory
   + OperationClass.

8. Cognitive Control and Host execution authority are conjunctive,
   orthogonal gates.

9. ALLOW is non-transferable and bound to the exact controlled world.

10. Succeeded / Failed / Indeterminate are distinct execution outcomes.

11. Indeterminate requires reconciliation, not blind retry.

12. Canonical cognition is upstream authority input.

13. Runtime state may propose cognition changes but cannot directly
    become canonical truth.

14. Runtime must remain participant-independent and harness-independent.

15. Process topology must not create a second canonical cognition authority.

16. Free-text claim governance is outside the first landing's hard
    enforcement claim.
```

---

# 3. Root component topology

The Runtime architecture is divided into five logical planes.

```text
┌──────────────────────────────────────────────────────────────────┐
│  PARTICIPANT / HARNESS PLANE                                     │
│                                                                  │
│  LLM / Human / Thinker                                           │
│          │                                                       │
│          ▼                                                       │
│  Harness Adapter                                                 │
│   ├─ Activation Boundary                                         │
│   ├─ Action Interception Boundary                                │
│   ├─ Post-Action Boundary                                        │
│   └─ Outbound Claim Boundary                                     │
└──────────┬───────────────────────────────────────────────────────┘
           │
           ▼
┌──────────────────────────────────────────────────────────────────┐
│  RUNTIME CONTROL PLANE                                           │
│                                                                  │
│  RuntimeHost                                                     │
│   ├─ IntentClassifier                                            │
│   ├─ CognitiveControl                                            │
│   ├─ PreparationCoordinator                                      │
│   ├─ RequirementResolverRegistry                                 │
│   ├─ DecisionBinder                                              │
│   ├─ ActionAdmissionCoordinator                                  │
│   ├─ PostActionObserver                                          │
│   ├─ FailureTracker                                              │
│   ├─ ReconciliationCoordinator                                   │
│   └─ RuntimeControlState                                         │
└──────┬─────────────────┬──────────────────┬──────────────────────┘
       │                 │                  │
       ▼                 ▼                  ▼
┌───────────────┐ ┌───────────────┐ ┌─────────────────────────────┐
│ COGNITION     │ │ EXECUTION     │ │ REQUIREMENT MECHANISMS      │
│ AUTHORITY     │ │ AUTHORITY     │ │                             │
│               │ │               │ │ LiveFactPort                │
│ Canonical     │ │ Execution     │ │ LowerLayerSearchPort        │
│ CognitionPort │ │ AuthorityPort │ │ ToolContractPort            │
│               │ │               │ │ MechanicalCheckPort         │
│ Cognition     │ │ qiven-host    │ │ HumanHandoffPort            │
│ ProposalPort  │ │ or equivalent │ │ other accepted resolvers    │
└───────────────┘ └───────────────┘ └─────────────────────────────┘
       │
       ▼
Canonical cognition transaction path
       │
       ▼
Canonical Cognition
```

> **Amendment 2026-09-26 (ADR-0043 and its 2026-09-26 forfeiture
> amendment):** the separate `qiven-host` box is historical topology. The
> execution-authority role is absorbed into the RuntimeHost-internal
> execution-authority subsystem behind `IExecutionAuthorityPort`; the
> qiven-host repository is deleted with its history forfeited
> (`DELETED_REMOTE + ARCHIVE_FORFEITED`). Read "qiven-host or equivalent"
> above as that internal subsystem. The conjunctive cognition+execution
> gates invariant is unchanged.

The most important separation is:

```text
Cognitive Control
≠
Canonical Cognition Authority
≠
Machine Execution Authority
```

All three participate in one governed operation, but none substitutes for another.

---

# 4. RuntimeHost

`RuntimeHost` is the composition root of the Cognitive Control runtime.

It owns the **runtime control domain**, not project truth and not machine execution authority.

Its responsibilities are:

```text
load one accepted DeploymentProfile revision

bind one accepted RuntimeGeneration

register harness adapters

own RuntimeControlState

own IntentClassifier

own CognitiveControl

own PreparationCoordinator

own RequirementResolverRegistry

own DecisionBinder

own PostActionObserver

own FailureTracker

own ReconciliationCoordinator

coordinate ExecutionAuthorityPort

bind canonical cognition and resolver ports

refuse hard-governed operation when required bindings are absent
```

It MUST NOT:

```text
mint InvocationPolicy

silently rewrite DeploymentProfile

write directly into canonical cognition

self-authorize machine mutation

treat an LLM declaration as evidence

turn unknown execution into failure

allow one harness to bypass another profile rule

maintain an independent durable project-memory truth
```

---

# 5. RuntimeHost lifetime and process boundary

The first production topology uses **one long-lived local RuntimeHost process per runtime control domain**.

This is deliberate.

The following components belong to one coherency/failure domain and MUST be co-resident for the first landing:

```text
IntentClassifier
CognitiveControl
PreparationCoordinator
RequirementResolverRegistry
DecisionBinder
ActionAdmissionCoordinator
PostActionObserver
FailureTracker
ReconciliationCoordinator
RuntimeControlState
```

They are not initially split across distributed services.

Reason:

```text
proposal correlation
requirement state
decision freshness
single-use ALLOW
failure tracking
indeterminate barriers
reconciliation
profile-generation consistency
```

all depend on strongly ordered local state.

Splitting those responsibilities across RPC boundaries before their semantics are proven would create distributed-consistency problems inside the control plane itself.

Harnesses may live in other processes.

External authorities and mechanisms may also live in other processes.

Therefore the first topology is:

```text
Harness / hook process
        │
        │ local authenticated adapter protocol
        ▼
Long-lived RuntimeHost
        │
        ├──── canonical cognition authority
        ├──── qiven-host / execution authority
        └──── resolver mechanisms
```

> **Amendment 2026-09-26 (ADR-0043 forfeiture):** the "qiven-host /
> execution authority" arm above is the RuntimeHost-internal
> execution-authority subsystem (lease/fencing/quarantine/journal
> semantics absorbed per the 2026-09-21 amendment); there is no separate
> host process, and the separate repository no longer exists.

This process is **not** `qiven-context` canonical authority and is **not** the Host execution broker.

It is a third, orthogonal responsibility:

> **runtime cognitive-decision authority.**

The eventual executable name and IPC technology are not fixed by this ADL.

---

# 6. RuntimeGeneration

Runtime configuration MUST NOT mutate invisibly beneath in-flight decisions.

A `RuntimeHost` therefore operates under one immutable:

```text
RuntimeGeneration
```

A RuntimeGeneration binds at minimum:

```text
DeploymentProfileRevision

GovernedActorSet revision

CapabilityUniverse revision

MediationInventory revision

adapter capability-manifest identities

RequirementResolverRegistry revision

classifier contract revision

control implementation/version identity
```

A profile or resolver change does not edit an existing generation in place.

It creates a new generation.

Conceptually:

```text
Generation G17
    profile P5
    resolver registry R8
    adapter manifest A12

change profile
    ↓

Generation G18
    profile P6
    resolver registry R8
    adapter manifest A12
```

Any unconsumed decision produced under `G17` is stale once the runtime crosses into `G18`.

Already-dispatched operations from the old generation may remain present only for:

```text
post-action observation
reconciliation
audit closure
```

They may not acquire new execution authorization.

---

# 7. DeploymentProfile

A `DeploymentProfile` is immutable, versioned runtime configuration.

It contains:

```text
ProfileIdentity
ProfileRevision

GovernedActorSet

CapabilityUniverse

MediationInventory

accepted harness-adapter bindings

accepted execution-authority binding

accepted resolver-registry binding

claim-channel coverage

runtime enforcement mode

conformance evidence reference
```

The first hard-governed Runtime landing uses only an **accepted, conformance-proven profile**.

No profile synthesis from prompts is allowed.

---

# 8. GovernedActorSet

`GovernedActorSet` defines who the hard-enforcement claim applies to.

Examples of identity dimensions include:

```text
participant class
registered agent/harness binding
session/runtime principal
local execution identity
```

Actor identity MUST originate from an accepted adapter/session binding.

It MUST NOT be inferred from statements such as:

```text
"I am Jason"

"I am the worker"

"I am the reviewer"
```

Natural-language identity is not runtime authentication.

A direct human path excluded from the GovernedActorSet remains outside this Runtime enforcement claim and remains governed by canonical collaboration contracts.

---

# 9. CapabilityUniverse

`CapabilityUniverse` is the closed universe against which complete mediation is claimed.

Each capability descriptor establishes at least:

```text
CapabilityId

adapter binding

capability class

reachable operation classes

resource/effect domains

whether it can mutate world state

whether it can publish claims

execution-authority requirement
```

The CapabilityUniverse is not:

```text
"everything that exists on Windows"
```

It is:

> everything exposed to the declared GovernedActorSet through the deployment profile.

Hard enforcement is meaningful only relative to this universe.

---

# 10. MediationInventory

`MediationInventory` maps capabilities to accepted interception paths.

Conceptually:

```text
Capability
    ↓
interception adapter
    ↓
observable pre-action boundary
    ↓
deny semantics
    ↓
post-action observation
```

For each governed operation class:

```text
all producing capabilities in CapabilityUniverse
must be mediated
```

or the operation class is:

```text
not fully governed
```

No runtime heuristic attempts to discover arbitrary unknown programs on the host.

Instead:

```text
declared universe
+
adapter capability manifest
+
conformance evidence
```

must agree.

---

# 11. Adapter capability handshake

At RuntimeHost startup, each harness adapter presents an actual capability manifest.

RuntimeHost compares it against the accepted DeploymentProfile.

If the adapter exposes:

```text
new tool
new mutation path
new shell path
changed deny behavior
changed claim channel
```

not represented in the accepted profile, the corresponding hard-enforcement claim becomes invalid.

The RuntimeHost MUST NOT silently continue under the old profile.

For a hard-governed profile:

```text
accepted profile
≠
actual adapter manifest
```

means fail closed for affected operation classes.

The handshake's honesty boundary is explicit: it detects drift of an
honest adapter manifest against the accepted profile. It does not detect
a dishonest manifest that under-declares capabilities; that residual is
bounded by the adapter's position inside the trust base (Section 69) and
by conformance evidence cross-checking the declared surface.

---

# 12. Harness boundary adapter model

A harness-specific adapter provides four logical channels.

```text
ActivationBoundaryPort

ActionInterceptionPort

PostActionObservationPort

OutboundClaimPort
```

They may physically share one adapter implementation.

The core Runtime MUST NOT expose names such as:

```text
PreToolUse
PostToolUse
Stop
PermissionRequest
```

in its architecture-level contracts.

Those are adapter mappings.

The adapter-to-RuntimeHost channel is **authenticated**: an adapter
instance must prove an accepted registration/credential before its
observations or manifests are admitted. The concrete credential model is
a mandatory C++ design boundary item (Section 90); this ADL fixes the
requirement, not the mechanism.

---

# 13. ActivationBoundaryPort

Activation events include:

```text
session start
prompt submission
task transition
workflow transition
explicit declared intent
```

Activation is an optimization and preparation opportunity.

It can preload cognition and create trusted activation receipts.

But correctness never depends on activation predicting every later action.

If a requirement becomes knowable only when an action proposal exists, the action boundary remains authoritative.

---

# 14. IActionInterceptionPort

This is the primary pre-execution boundary.

It receives an immutable observation of the exact proposed operation.

The adapter must expose enough data to construct:

```text
ObservedAction

actor identity

session identity

adapter identity

capability identity

operation identity

target/resource identity

structured arguments

harness correlation identity
```

The observed request is immutable for the control attempt.

If the harness later changes arguments or target, that is a new proposal.

---

# 15. PostActionObservationPort

The post-action channel reports the best mechanically supported execution outcome.

Its state is at least:

```text
Succeeded
Failed
Indeterminate
```

`Failed` requires evidence that the action is known to have failed under the mechanism contract.

`Indeterminate` is mandatory when completion cannot be proven.

Examples:

```text
lost response
transport disconnect
timeout after dispatch
hook crash
process disappearance
partial observation
ambiguous remote result
```

No adapter may translate:

```text
"no success acknowledgement"
```

into:

```text
"Failed"
```

unless the underlying mechanism contract proves that inference valid.

---

# 16. IOutboundClaimPort

The logical component exists now, but the first landing does not claim general free-text hard enforcement.

Two channels remain distinct:

```text
tool-mediated claims
    commit message
    PR text
    review verdict
    publication payload
    structured tool argument

free-response claims
    arbitrary assistant response text
```

Tool-mediated claims are governed through ordinary action interception.

Free-response classification remains a later profile.

Therefore the first profile MUST report:

```text
free-text MakeCanonicalClaim / MakeLiveClaim:
    hard enforcement not claimed
```

unless and until an accepted OutboundClaim adapter and classifier contract exists.

---

# 17. Correlation model

There is no global `currentAction`.

Every exact proposal receives a Qiven-owned:

```text
ControlTransactionId
```

The external boundary also carries a structured correlation key.

Conceptually:

```text
CorrelationKey
{
    RuntimeGenerationId
    AdapterInstanceId
    HarnessSessionId
    ActorInstanceId
    HarnessActionId
}
```

The exact serialized representation is deferred.

The requirements are not.

For one exact harness action:

```text
CorrelationKey
    ↔
one ControlTransactionId
```

must remain stable through:

```text
pre-action interception
resolver execution
decision
mechanism dispatch
post-action observation
reconciliation
```

---

# 18. Causal chains and re-deliberation

A `BeforeJudgment` miss causes the original proposal to stop.

Example:

```text
ControlTxn A
    Write new primitive
        ↓
SearchLowerLayer required
        ↓
not satisfied
        ↓
ReDeliberate
        ↓
ControlTxn A terminates
```

The next participant proposal is a new exact action:

```text
ControlTxn B
```

It may carry:

```text
causal_parent = A
```

for traceability.

It MUST NOT reuse A's action authorization.

Thus:

```text
re-deliberation chain
≠
one mutable action transaction
```

Each exact proposal has its own transaction identity.

---

# 19. ControlTransaction lifetime

A ControlTransaction begins when an externally meaningful proposal enters a governed boundary.

It terminates as one of:

```text
ReDeliberationRequired

Denied

CancelledBeforeDispatch

Succeeded

KnownFailed

ReconciledSucceeded

ReconciledFailed
```

`Indeterminate` is not terminal.

It remains active until reconciliation resolves it or governance explicitly closes it under a recovery procedure.

---

# 20. IntentClassifier

`IntentClassifier` converts mechanism observations into policy-relevant semantic intent.

It consumes:

```text
ObservedAction
structural repository/runtime facts
optional declared semantic intent
optional semantic-classifier output
```

Its most important rule is:

> **ActionKind is not assumed to be mutually exclusive at the physical action level.**

One physical action may imply several semantic intents.

Example:

```text
Write include/qiven/public/foo.hpp
```

may simultaneously imply:

```text
CreateCppSymbol
IntroducePrimitive
ModifyReferencedContract
```

Therefore production classification yields an:

```text
IntentSet
```

rather than forcing one exclusive label.

---

# 21. IntentSet semantics

An `IntentSet` contains one or more `ActionIntent` projections.

Conceptually:

```text
IntentSet
    intent A
    intent B
    intent C
```

Frozen v4 semantics are applied independently to each ActionIntent.

The resulting requirements are combined without losing provenance.

The control plane MUST NOT choose:

```text
"the most likely intent"
```

and discard others merely to simplify policy evaluation.

Mechanically established intents are mandatory members of the set.

If semantic classification is uncertain and a possible classification would introduce a blocking requirement, the runtime must either:

```text
safely over-approximate by evaluating that intent
```

or:

```text
fail closed / obtain further classification
```

It may not silently choose the less restrictive interpretation.

---

# 22. Classification evidence

Every IntentSet records why each intent exists.

Classification basis can be:

```text
mechanical
structural
declared
semantic-judgment
```

A semantic classifier result is itself Judgment.

It is not mechanical proof.

If a hard-enforcement guarantee depends on semantic-only classification, the DeploymentProfile must identify:

```text
accepted classifier
classifier/version
uncertainty contract
coverage scope
```

and the enforcement claim is bounded accordingly.

The first landing SHOULD rely on structural classification wherever possible.

---

# 23. CanonicalCognitionPort

The canonical read side exposes one authoritative capability:

> **pin one exact cognition revision for a ControlTransaction.**

It returns a logical:

```text
PinnedCognition
{
    canonical revision identity
    Snapshot
    materialized InvocationPolicy
    policy digest
    canonical authority identity
}
```

The Runtime consumes the InvocationPolicy carried by the Snapshot.

Physically, the pinned Snapshot may be a lazy view/handle bound to the
pinned revision rather than a per-transaction corpus-sized copy; the
logical contract — one immutable revision for the transaction's lifetime
— is what is fixed here.

It MUST NOT reconstruct effective policy by re-reading ADR prose, memory prose, constraints or pit text and independently recompiling rules.

Those records may explain and govern how canonical policy changed.

They are not a second runtime policy source.

---

# 24. CognitionProposalPort

Canonical reads and canonical write proposals are structurally separated.

Runtime components NEVER receive a direct:

```text
writeCanonical(...)
```

capability.

Instead they may emit:

```text
CognitionTransactionProposal
```

containing such material as:

```text
new execution evidence
failure evidence
candidate pit
candidate lesson
candidate policy amendment
runtime audit evidence
```

That proposal enters the existing governed canonical cognition transaction path.

Only that path may:

```text
authorize
review
accept
persist
publish
```

the cognition change.

Therefore:

```text
Runtime discovered X
```

does not imply:

```text
X is canonical cognition.
```

---

# 25. CognitiveControl

`CognitiveControl` is the semantic core.

It is deliberately small.

Its responsibility is:

```text
Pinned InvocationPolicy
+
IntentSet
+
trusted requirement state
    ↓
derive frozen v4 requirements
    ↓
evaluate readiness
```

It does not:

```text
search lower layers

query live systems

run tests

ask humans

execute tools

write cognition

classify harness APIs

mint Host authority
```

Those are mechanisms around it.

`CognitiveControl` should remain as close as possible to a production realization of the frozen executable specification.

---

# 26. Multi-intent requirement derivation

For every intent in the IntentSet:

```text
derive requirements using frozen v4 semantics
```

The final ControlTransaction contains the union of requirement instances.

A requirement instance retains:

```text
RequirementKind

subject

RequirementBoundary

blocking

originating InvocationRule

originating ActionIntent(s)
```

Two identical physical resolver operations may be executed once and their evidence shared.

But semantic requirement provenance is never erased merely because resolution is deduplicated.

Thus:

```text
resolution deduplication
≠
policy requirement deletion
```

---

# 27. Requirement identity

A runtime requirement has a stable logical identity derived from:

```text
RequirementKind
subject
boundary
blocking semantics
policy-rule provenance
relevant intent provenance
```

The exact digest format is deferred.

The identity prevents a receipt produced for:

```text
SearchLowerLayer("hashing")
```

from satisfying:

```text
SearchLowerLayer("serialization")
```

merely because both share a RequirementKind.

---

# 28. RequirementResolverRegistry

The registry answers:

> Which accepted mechanism is authorized to satisfy this RequirementKind in this RuntimeGeneration?

A binding contains at least:

```text
RequirementKind

ResolverIdentity

ResolverVersion

accepted evidence type

trust domain

DeploymentProfile applicability
```

The registry is immutable inside one RuntimeGeneration.

Changing a resolver binding creates a new generation.

There is no silent fallback to an arbitrary available mechanism.

---

# 29. Resolver failure semantics

For a mandatory requirement:

```text
no accepted resolver binding
```

means the requirement remains unsatisfied.

It does not mean:

```text
skip requirement
```

Likewise:

```text
resolver unavailable
resolver timed out
resolver returned unverifiable evidence
```

cannot become `Satisfied`.

Before the applicable deadline, the result is fail-closed.

---

# 30. PreparationCoordinator

`PreparationCoordinator` orchestrates requirement resolution.

It does not decide which requirements exist.

The flow is:

```text
IntentSet
    ↓
CognitiveControl derives requirements
    ↓
PreparationCoordinator inspects unsatisfied requirements
    ↓
ResolverRegistry selects accepted resolver
    ↓
resolver executes
    ↓
typed evidence receipt
    ↓
CognitiveControl re-evaluates readiness
```

Independent resolvers MAY execute concurrently.

Final readiness is evaluated only after all blocking requirement instances relevant to the gate have a known state.

---

# 31. BeforeJudgment path

If a blocking `BeforeJudgment` requirement is not satisfied:

```text
current proposal cannot continue
```

The coordinator may resolve the requirement.

If resolution supplies cognition required for reasoning:

```text
PreparationContext
    ↓
Harness Adapter
    ↓
next Judgment cycle
```

The exact proposal terminates as:

```text
ReDeliberationRequired
```

The next participant proposal becomes a new ControlTransaction.

This prevents post-hoc repair of an already accepted judgment.

---

# 32. BeforeExecution path

Once:

```text
ready_for_judgment() == true
```

the exact action may proceed through remaining `BeforeExecution` requirements.

Examples:

```text
InspectToolContract
RunMechanicalCheck
RequestReview
AskHuman / typed handoff
```

These requirements may be satisfied without another Judgment cycle if the exact proposed action remains unchanged.

If satisfying the requirement changes:

```text
tool arguments
target
content
diff
operation class
semantic intent
```

the action is a new proposal and must be reclassified.

---

# 33. Trusted evidence receipt

A resolver never returns a naked:

```text
true
```

for mandatory satisfaction.

It returns a typed immutable evidence receipt.

A logical receipt binds at least:

```text
EvidenceId

RequirementIdentity

ResolverIdentity
ResolverVersion

subject / scope

source identity

result

content/result digest

observation identity

cognition revision where applicable

semantic time where applicable

validity / expiry where applicable

provenance
```

The receipt is runtime evidence.

It is not automatically canonical evidence.

---

# 34. Resolver ports

The first component model defines these authority/mechanism ports.

| Port                     | Requirement role                                                |
| ------------------------ | --------------------------------------------------------------- |
| `CanonicalCognitionPort` | MandatoryRecall / VerifyCanonical source                        |
| `LiveFactPort`           | VerifyLive                                                      |
| `LowerLayerSearchPort`   | SearchLowerLayer                                                |
| `ToolContractPort`       | InspectToolContract                                             |
| `MechanicalCheckPort`    | RunMechanicalCheck                                              |
| `HumanHandoffPort`       | RequestReview / AskHuman / typed H1-H4 evidence                 |
| `ExecutionAuthorityPort` | machine execution admission, orthogonal to CognitiveRequirement |

These are logical contracts.

Their implementation may be:

```text
in-process
local IPC
MCP-backed
CLI-backed
GitHub-backed
repository-backed
remote API-backed
human-mediated
```

without changing CognitiveControl semantics.

---

# 35. LowerLayerSearchPort

Lower-layer search returns mechanically attributable search evidence.

A valid receipt must establish:

```text
searched scope

search mechanism/version

query/subject

candidate identities

search boundary / exclusions

result digest
```

The LLM saying:

```text
"I checked and found nothing"
```

does not satisfy the requirement.

---

# 36. ToolContractPort

Tool contracts belong to Mechanism.

The port returns the accepted contract for the exact:

```text
tool
operation
profile
```

and can mechanically construct or validate the invocation.

The participant may propose arguments.

It does not define the legal invocation shape.

---

# 37. MechanicalCheckPort

A mechanical-check receipt must correspond to the exact artifact/action it protects.

For example:

```text
gate PASS
```

must bind:

```text
exact head / artifact digest
check identity
check version
result
execution identity
```

A PASS from an older head cannot satisfy a requirement on a newer head.

---

# 38. HumanHandoffPort

Human review/handoff is a typed authority/evidence mechanism.

It preserves ADR-0036:

```text
H1 execution handoff
H2 review handoff
H3 authority handoff
H4 recovery-presence handoff
```

A runtime request for review produces a typed handoff request.

A satisfying receipt binds the protected object.

For H2, for example:

```text
reviewed exact delta
reviewer authority/binding
review result
content identity
```

A conversational statement disconnected from the protected object is insufficient.

---

# 39. DecisionBinder

When `PreparationPacket` is ready for execution, `DecisionBinder` creates an immutable logical:

```text
ExecutionDecision
```

An ALLOW decision binds the controlled world.

At minimum:

```text
ControlTransactionId

RuntimeGenerationId

ObservedAction digest

IntentSet digest

Pinned canonical cognition revision

InvocationPolicy digest

derived requirement-set digest

trusted evidence-set digest

DeploymentProfileRevision

RequirementResolverRegistry revision

classifier-contract identity where applicable
```

Host lease/fencing authority is intentionally **not** included.

Host admission remains an independent live gate.

---

# 40. Single-use authorization

`ALLOW` is logically single-use.

It is not:

```text
bool may_execute_forever
```

It means:

> this exact action, under this exact controlled state, has satisfied the Cognitive Control gate.

If an explicit permit representation is later introduced, consumption of that permit must be one-shot.

If the harness uses synchronous allow/deny semantics instead, adapter conformance must prove the allow applies only to the immutable intercepted proposal.

---

# 41. Decision staleness

An existing ALLOW becomes stale if any bound fact changes before execution.

Examples:

```text
arguments change

target changes

action payload changes

intent classification changes

canonical cognition head advances

InvocationPolicy changes

evidence expires

evidence is superseded

DeploymentProfile revision changes

ResolverRegistry revision changes

RuntimeGeneration changes
```

A stale action re-enters Cognitive Control.

It does not receive an incremental patch to an old ALLOW.

---

# 42. Final freshness check

Immediately before an adapter returns or consumes final execution permission, the ActionAdmissionCoordinator revalidates:

```text
exact action digest unchanged

RuntimeGeneration unchanged

canonical revision still equals pinned revision

required evidence still valid

no relevant reconciliation barrier exists

decision not previously consumed
```

This is the last Cognitive Control freshness point.

An adapter claiming hard enforcement must ensure the mechanism executes against the same immutable proposal checked at this point.

Its conformance test must document any residual check-to-execution window.

---

# 43. ActionAdmissionCoordinator

`ActionAdmissionCoordinator` composes the two orthogonal gates.

```text
Cognitive Control ALLOW
        AND
Execution Authority admission
        ↓
dispatch permitted
```

It does not merge their meanings.

Cognitive Control asks:

```text
"are the epistemic / policy prerequisites satisfied?"
```

Execution authority asks:

```text
"is this exact caller/action currently admitted to execute
under machine authority?"
```

Neither receipt satisfies the other.

---

# 44. ExecutionAuthorityPort

For JasonPC machine-local operations, the accepted implementation is the ADR-0026 Host execution-authority path once that path is itself accepted and enabled.

The port may provide such concepts as:

```text
execution transaction identity
lease
fencing epoch
admission result
reconciliation handle
```

but those remain Host semantics.

Runtime Cognitive Control cannot mint them.

If the DeploymentProfile requires machine-local mutation and no accepted execution-authority binding exists:

```text
mutation remains unavailable
```

The RuntimeHost MUST NOT bypass the authority port.

---

> **Amendment 2026-09-21 (ADR-0043):** qiven-host is sealed; the
> ADR-0026 execution-authority invariants (single-writer lease, fencing,
> quarantine, reconciliation, journal) are absorbed as
> RuntimeHost-internal subsystem semantics behind
> IExecutionAuthorityPort. Sections 44/45/§82 now read "the RuntimeHost
> execution-authority subsystem" wherever they name the separate Host
> path. The conjunctive-gates invariant is unchanged in meaning.

# 45. Current JasonPC consequence

ADR-0026's current fail-closed state remains binding.

Therefore accepting this component ADL does **not** itself re-enable mutating DCR/Host workflows.

Runtime implementation may prove:

```text
classification
preparation
deny
re-deliberation
resolver behavior
decision binding
simulated execution
read-only interception
```

without gaining machine-local mutation authority.

Actual governed JasonPC mutation requires the separately accepted execution-authority path.

---

# 46. PostActionObserver

After mechanism dispatch, the adapter submits one correlated observation.

`PostActionObserver` validates:

```text
ControlTransactionId

HarnessActionId

action digest

execution-authority correlation where applicable

outcome evidence
```

It then classifies:

```text
Succeeded
Failed
Indeterminate
```

Conflicting observations for the same exact execution are a runtime integrity failure.

---

# 47. Succeeded

Succeeded means the runtime possesses evidence sufficient under the mechanism contract to state that execution completed successfully.

The observer may produce:

```text
success evidence receipt
live-state refresh evidence
candidate canonical evidence proposal
```

The ControlTransaction becomes terminal.

---

# 48. Known failure

Failed means failure is mechanically established.

The observer produces a normalized:

```text
FailureFingerprint
```

and hands it to `FailureTracker`.

The failure can then influence the next equivalent proposal through:

```text
RetryFailure
→ InspectKnownPit
→ retry discipline
```

The participant does not need to remember the failure.

---

# 49. Indeterminate

Indeterminate means:

```text
execution may have happened
execution may not have happened
runtime cannot prove which
```

This creates:

```text
ReconciliationBarrier
```

rather than FailureFingerprint.

No equivalent retry is permitted merely because success was not observed.

---

# 50. EffectScope

Every potentially side-effecting action is assigned an `EffectScope`.

The EffectScope identifies the world region for which an unknown outcome is unsafe.

Examples conceptually include:

```text
repository + ref

file path

artifact

remote PR

deployment resource

canonical object
```

Exact effect-scope schemas are adapter-specific but must be mechanically derived from the ObservedAction wherever possible.

---

# 51. ReconciliationBarrier

An indeterminate action creates a barrier over its EffectScope.

While active:

```text
equivalent mutation
conflicting mutation
retry
success/failure publication
```

within that scope is blocked unless recovery policy explicitly proves safety.

Independent scopes may continue.

Thus the system does not need a global stop merely because one action is uncertain.

---

# 52. ReconciliationCoordinator

The coordinator seeks authoritative observation.

Possible mechanisms include:

```text
Host execution journal

Git remote state

filesystem identity

tool/provider transaction status

GitHub object identity

live service state

human recovery handoff
```

A reconciliation result can be:

```text
ResolvedSucceeded

ResolvedFailed

StillIndeterminate
```

`StillIndeterminate` leaves the barrier in force.

---

# 53. Reconciliation is not retry

This invariant is absolute:

```text
unknown outcome
≠
failure
≠
permission to retry
```

If resolved as Failed:

```text
FailureFingerprint
→ FailureTracker
→ retry discipline
```

If resolved as Succeeded:

```text
success evidence
→ transaction completion
```

Only after resolution does the normal state machine resume.

---

# 54. FailureTracker

FailureTracker holds runtime-known failure state.

It indexes known failures by such dimensions as:

```text
actor/session
tool/operation
normalized FailureFingerprint
EffectScope
causal chain
```

Its runtime state is not automatically durable cognition.

It can emit a candidate cognition proposal when a failure becomes materially reusable knowledge.

Acceptance into a canonical pit/lesson remains outside Runtime.

---

# 55. RuntimeControlState

RuntimeControlState stores only control-plane state.

Examples:

```text
active ControlTransactions

correlation mappings

PreparationPackets

evidence receipts

consumed decisions

known failures

ReconciliationBarriers

adapter registrations

RuntimeGeneration

activation receipts
```

It MUST NOT become:

```text
another Snapshot

another InvocationPolicy store

another project-memory database

another ADR database
```

---

# 56. Runtime state durability

Runtime state and canonical cognition have different durability semantics.

However, **non-canonical does not mean disposable**.

For a hard-governed mutating production profile, the runtime must survive enough failure to avoid forgetting:

```text
a dispatched-but-unobserved mutation

a consumed execution decision

an active reconciliation barrier

a correlation needed to interpret a later post-action result
```

Therefore the Runtime exposes an abstract:

```text
RuntimeStateStore
```

for restart recovery.

The storage product is not selected here.

The store also serves an append-only **control-decision audit view**
(which ExecutionDecision was consumed when, by which ControlTransaction,
under which RuntimeGeneration) for dispute resolution and C-19 restart
evidence; it is derived state, not canonical cognition.

---

# 57. RuntimeStateStore acceptance levels

An in-memory RuntimeStateStore is sufficient for:

```text
unit tests
reference implementation
single-process semantic proof
non-mutating control demonstrations
```

It is insufficient to claim crash-safe governed mutation.

A production mutating profile must provide either:

```text
restart-capable RuntimeStateStore
```

or:

```text
an accepted authoritative execution journal from which
all required in-flight/reconciliation state can be reconstructed
```

with conformance evidence proving reconstruction.

---

# 58. Restart semantics

On RuntimeHost restart:

```text
no previous unconsumed ALLOW remains executable
```

The new process receives a new RuntimeGeneration.

Previously dispatched but unfinished operations must be recovered as:

```text
known completed
known failed
or Indeterminate
```

They cannot disappear.

Any effect scope whose state cannot be reconstructed safely acquires or retains a reconciliation barrier.

---

# 59. Concurrency model

RuntimeHost supports multiple concurrent ControlTransactions.

There is no default global cognitive-control lock.

Independent proposals may run concurrently if:

```text
their resolver work is independent

no reconciliation barrier overlaps

their execution authority permits concurrency

their mechanisms permit concurrency
```

ADR-0026 may still impose stricter Host serialization on JasonPC.

That is execution-authority policy, not Cognitive Control policy.

---

# 60. Duplicate interception

If the same harness event is delivered twice with the same CorrelationKey and identical content:

```text
processing is idempotent
```

RuntimeHost returns the already-known control state.

If the same correlation identity arrives with different content:

```text
runtime integrity failure
```

The second payload is not treated as a new action under the old identity.

---

# 61. Out-of-order events

A post-action event received before a corresponding dispatch state, or an event whose action digest cannot be matched, is not guessed into place.

It becomes:

```text
correlation integrity failure
```

and may require reconciliation.

The Runtime must prefer uncertainty over fabricated causality.

---

# 62. Resolver concurrency

Independent requirements may resolve concurrently.

For example:

```text
SearchLowerLayer
VerifyLive
InspectToolContract
```

may run in parallel if their mechanisms and policy allow it.

But concurrency never weakens gating.

A blocking requirement remains blocking until its trusted evidence is committed into the current PreparationPacket.

---

# 63. Evidence sharing

One evidence receipt MAY satisfy several requirement instances only when:

```text
the receipt's subject/scope covers each requirement

the resolver type is accepted for each requirement

validity remains current

policy semantics are identical
```

The runtime records each satisfaction relation separately.

This prevents accidental transitive proof.

---

# 64. Canonical revision pinning

Every ControlTransaction pins one canonical cognition revision.

Requirement derivation for that transaction uses that revision only.

One transaction may not combine:

```text
policy from revision N

pit data from revision N+1

canonical fact from revision N+2
```

without explicitly abandoning the old transaction and re-preparing against a new pin.

This preserves one coherent epistemic world.

---

# 65. Canonical advancement while preparing

If canonical cognition advances before execution:

```text
old ExecutionDecision becomes stale
```

even if the changed canonical record appears unrelated.

The first implementation uses revision-level invalidation, not speculative dependency tracking.

This is intentionally conservative.

Fine-grained dependency invalidation may be revisited only after correctness is proven.

---

# 66. Activation receipts

Cognition injected before a specific proposal may later help satisfy a requirement only if a trusted receipt proves:

```text
subject

canonical revision

actor/session

injection event

effective-context delivery

validity
```

"the system probably showed the rule earlier" is not sufficient evidence.

If no valid activation receipt exists, normal preparation occurs.

---

# 67. First-landing claim scope

The first runtime landing hard-governs:

```text
structured tool/action proposals

tool-mediated publication claims

mechanical execution outcomes
```

It does not claim general free-response factual correctness.

Therefore the first DeploymentProfile explicitly reports:

```text
free-text canonical/live claim coverage:
    not hard governed
```

This is an honest scope boundary, not an unfinished hidden feature.

---

# 68. Canonical write-back path

Execution evidence flows:

```text
Mechanism
    ↓
PostActionObserver
    ↓
Runtime evidence
    ↓
optional CognitionTransactionProposal
    ↓
canonical transaction/governance path
    ↓
accepted canonical cognition
```

Never:

```text
Mechanism
    ↓
RuntimeControlState
    ↓
direct canonical mutation
```

This preserves one cognition authority.

---

# 69. Trust model

The Runtime trust base contains narrowly scoped trusted mechanisms.

| Component                         | Trusted for                                   | Not trusted for                          |
| --------------------------------- | --------------------------------------------- | ---------------------------------------- |
| Harness Adapter                   | exact boundary observation and deny semantics | project truth                            |
| IntentClassifier mechanical rules | mechanically proven classifications           | requirement satisfaction                 |
| Semantic classifier               | bounded Judgment classification               | mechanical proof                         |
| CognitiveControl                  | frozen policy derivation/readiness            | external evidence                        |
| ResolverRegistry                  | binding requirement kind to accepted resolver | project policy creation                  |
| Requirement Resolver              | its declared evidence mechanism               | unrelated requirement kinds              |
| DecisionBinder                    | exact decision binding                        | machine execution authority              |
| RuntimeStateStore                 | runtime state persistence                     | canonical cognition                      |
| PostActionObserver                | execution-observation classification          | unobserved world state                   |
| ReconciliationCoordinator         | authoritative recovery orchestration          | assuming an unknown result               |
| CanonicalCognitionPort            | canonical Snapshot/revision                   | live external truth                      |
| ExecutionAuthorityPort            | execution admission/fencing                   | cognitive prerequisites                  |
| Participant                       | Judgment/proposal                             | self-certification of mandatory evidence |

---

# 70. Data classification

Objects are divided into three classes.

### Canonical cognition

```text
Snapshot
InvocationPolicy
accepted Memory/Lesson/Risk/Decision
canonical obligations
canonical provenance
accepted durable evidence
```

Authority: canonical cognition system only.

### Runtime state

```text
ObservedAction
IntentSet
ControlTransaction
PreparationPacket
resolver receipts
ExecutionDecision
FailureFingerprint
ReconciliationBarrier
RuntimeGeneration
adapter correlation
```

Authority: RuntimeHost for current runtime operation only.

### Derived data

```text
digests
coverage reports
classification projections
search candidate lists
context injection packets
conformance reports
cached canonical views
```

Authority: none beyond the exact derivation and source identities they carry.

---

# 71. Component ownership

The logical ownership tree is:

```text
RuntimeHost
│
├── immutable RuntimeGeneration
│   ├── DeploymentProfile
│   ├── ResolverRegistry
│   └── classifier/adapter contract identities
│
├── IntentClassifier
├── CognitiveControl
├── PreparationCoordinator
├── DecisionBinder
├── ActionAdmissionCoordinator
├── PostActionObserver
├── FailureTracker
├── ReconciliationCoordinator
│
├── RuntimeControlState
│   ├── ControlTransactions
│   ├── correlation indexes
│   ├── evidence receipts
│   ├── consumed decisions
│   └── reconciliation barriers
│
└── bound ports
    ├── HarnessAdapter(s)
    ├── CanonicalCognitionPort
    ├── CognitionProposalPort
    ├── ExecutionAuthorityPort
    └── Requirement Resolver ports
```

External authoritative systems are referenced through ports.

RuntimeHost does not own them.

---

# 72. Lifetime table

| Object                | Lifetime                                                   |
| --------------------- | ---------------------------------------------------------- |
| RuntimeHost           | long-lived local runtime process                           |
| RuntimeGeneration     | immutable host generation                                  |
| DeploymentProfile     | immutable accepted revision                                |
| AdapterInstance       | adapter connection/registration                            |
| HarnessSession        | harness session                                            |
| ControlTransaction    | one exact proposal through terminal/reconciled state       |
| PinnedCognition       | one ControlTransaction                                     |
| IntentSet             | one ControlTransaction                                     |
| PreparationPacket     | one ControlTransaction                                     |
| EvidenceReceipt       | immutable; retained as long as required for decision/audit |
| ExecutionDecision     | until consumed, denied, or stale                           |
| Host Admission        | dispatch boundary only                                     |
| PostActionObservation | one execution observation                                  |
| FailureFingerprint    | runtime failure history; optionally proposed canonical     |
| ReconciliationBarrier | until authoritative resolution                             |
| RuntimeControlState   | RuntimeHost lifetime plus recovery persistence as required |

---

# 73. Pre-execution state machine

The logical state machine is:

```text
Observed
    ↓
Classified
    ↓
PolicyPinned
    ↓
RequirementsDerived
    ↓
PreparingBeforeJudgment
    │
    ├── failed ───────────────► Denied
    │
    ├── needs cognition ──────► ReDeliberationRequired
    │
    └── ready
          ↓
PreparedForJudgment
          ↓
PreparingBeforeExecution
    │
    ├── failed ───────────────► Denied
    │
    ├── waiting evidence ─────► AwaitingRequirement
    │
    └── ready
          ↓
CognitiveAllowed
          ↓
FreshnessCheck
    │
    ├── stale ────────────────► Reprepare
    │
    └── fresh
          ↓
ExecutionAuthorityAdmission
    │
    ├── denied ───────────────► Denied
    │
    ├── unavailable ──────────► Await / Denied by profile
    │
    └── admitted
          ↓
Dispatched
```

---

# 74. Post-execution state machine

```text
Dispatched
    ↓
PostActionObservation
    │
    ├── Succeeded
    │      ↓
    │   Succeeded (terminal)
    │
    ├── Failed
    │      ↓
    │   FailureFingerprint
    │      ↓
    │   KnownFailed (terminal)
    │
    └── Indeterminate
           ↓
       ReconciliationBarrier
           ↓
       Reconciling
        ┌──┴──────────────┐
        │                 │
 ResolvedSucceeded   ResolvedFailed
        │                 │
        ▼                 ▼
 Succeeded (terminal) FailureFingerprint
                           ↓
                     KnownFailed (terminal)
```

`StillIndeterminate` remains in `Reconciling`.

---

# 75. Control disposition returned to a harness

The logical disposition is not only allow/deny.

Runtime may return:

```text
Allow

ReDeliberate
    required cognition supplied

Await
    trusted external requirement still pending

Deny
    requirement failed / policy blocks

NotGoverned
    outside hard-enforcement coverage
```

For the first hard-governed action channel, `NotGoverned` must not silently masquerade as hard-governed ALLOW.

Whether an advisory-only profile permits continuation is outside the first production profile.

---

# 76. First production enforcement mode

The first production Runtime profile is **hard-enforcement-first**, not advisory-first.

For operation classes claimed as governed:

```text
unclassified
unmediated
missing policy
missing resolver
invalid evidence
stale decision
authority unavailable
correlation ambiguity
indeterminate conflicting effect
```

all fail closed.

Advisory observation may exist for diagnostics but is not used to justify a hard-governed execution.

---

# 77. Profile reload

Runtime profile changes use controlled generation replacement.

Sequence:

```text
construct new RuntimeGeneration

validate DeploymentProfile + capability manifests

validate resolver registry

establish required external ports

prevent new dispatch under old generation

allow old dispatched operations only to observe/reconcile

invalidate old unconsumed decisions

activate new generation
```

No mutable global configuration object is edited underneath transactions.

---

# 78. Resolver-registry reload

Resolver changes follow the same rule.

A new resolver implementation/version creates a new RuntimeGeneration.

An old receipt may remain evidentiary only if its validity contract says so.

An unconsumed old decision is still stale because its resolver-registry revision changed.

---

# 79. Harness replacement invariant

Replacing:

```text
ZCode
→ another harness
```

may change:

```text
event mapping
tool schema
IPC
capability manifest
deny implementation
post-action mechanism
```

but must not change:

```text
IntentSet semantics
InvocationPolicy
requirement derivation
evidence trust
decision binding
tri-state outcome semantics
reconciliation semantics
canonical write direction
```

A new adapter proves conformance to these component contracts.

---

# 80. Participant replacement invariant

Replacing:

```text
GPT
→ GLM
→ Claude
→ another model
→ mediated human participant
```

may change Judgment.

It must not change:

```text
mandatory requirement derivation
trusted resolver source
allow/deny rules
decision binding
execution authority
failure handling
```

A participant cannot receive weaker rules merely because it is assumed smarter.

---

# 81. First landing adapter

ZCode remains the first candidate adapter because its harness appears to expose the necessary lifecycle/tool boundaries.

However, no ZCode event name becomes core Runtime API.

Before adapter acceptance it must prove:

```text
exact pre-execution observation

reliable deny

no fall-through after deny

immutable proposal between decision and execution

context/evidence return to next Judgment

post-success observation

post-failure observation

representation of unknown outcome

stable correlation identity

capability manifest

actor/session identity binding
```

A mismatch in ZCode's actual hook surface changes the adapter, not this ADL.

---

# 82. First landing mutation boundary

Until the accepted execution-authority path permits JasonPC mutation, the Runtime landing must not invent one.

Therefore first implementation work may include:

```text
real interception

real classification

real canonical policy pinning

real lower-layer/tool-contract resolvers

real deny/re-deliberation

real decision binding

real correlation/concurrency

simulated or quarantined execution

read-only mechanisms
```

while actual JasonPC mutation remains separately gated by ADR-0026.

---

# 83. No generic event bus

Runtime components communicate through explicit typed orchestration.

The first architecture does not introduce:

```text
generic pub/sub
unbounded event bus
plugin broadcast system
event-sourced cognitive runtime
```

Reason:

the control path requires explicit causality and transaction identity.

An event bus would obscure:

```text
who derived a requirement
which proposal evidence belongs to
which ALLOW corresponds to which action
which failure caused which retry rule
```

Direct typed component relationships are preferred.

---

# 84. No second policy engine

There is exactly one effective invocation policy per pinned Snapshot.

These mechanisms MUST NOT independently invent additional mandatory CognitiveRequirements:

```text
HarnessAdapter
IntentClassifier
PreparationCoordinator
Resolver
ExecutionAuthorityPort
RuntimeStateStore
```

They may produce facts that cause existing canonical policy to apply.

They do not become alternate policy authorities.

---

# 85. No second machine authority

Likewise, RuntimeHost is not a substitute for qiven-host.

A cognitive ALLOW cannot:

```text
override fencing

override quarantine

grant machine lease

clear reconciliation

bypass execution authority
```

The final action requires both domains.

> **Amendment 2026-09-26 (ADR-0043 forfeiture):** the separate qiven-host
> process no longer exists; "no second machine authority" now reads as
> "no machine authority outside the RuntimeHost execution-authority
> subsystem" — a direct mutating path around RuntimeHost remains a safety
> defect. The invariant above is unchanged in force and is exactly what
> survives the seal: cognition and execution authority remain separate
> gates inside the one trusted process; a cognitive ALLOW still cannot
> override fencing, quarantine, lease, reconciliation, or execution
> authority.

---

# 86. No second canonical authority

Runtime cannot turn:

```text
FailureTracker state

resolver receipt

profile configuration

execution journal

ReconciliationBarrier
```

directly into ProjectContext truth.

Canonicalization is always a separate governed transaction.

---

# 87. K5 relationship

K5 remains orthogonal.

A future K5 transport may change:

```text
how PinnedCognition / prepared cognition
is encoded for delivery to Judgment
```

It does not change:

```text
what Snapshot is authoritative

what policy applies

what requirement must be satisfied

what decision is valid

what RuntimeControlState means
```

`LiveFactPort` is outside K5.

---

# 88. Component ADL acceptance obligations

The component architecture is accepted only when the following proof obligations are agreed.

### C-01 — external invocation

A participant that never explicitly asks for Qiven still triggers control at a governed action boundary.

### C-02 — multi-intent preservation

One physical action classified into several ActionKinds receives the union of all applicable requirements.

No "best intent" narrowing is permitted.

### C-03 — canonical policy singularity

Runtime reads the materialized InvocationPolicy from one pinned Snapshot and does not derive a second policy from prose.

### C-04 — BeforeJudgment causality

A BeforeJudgment requirement discovered at the action boundary stops the exact proposal, prepares cognition and requires a new Judgment cycle.

### C-05 — trusted resolver binding

Only the accepted resolver/version for a RequirementKind can produce a satisfying receipt.

### C-06 — requirement-specific evidence

Evidence for one subject cannot accidentally satisfy another subject merely because RequirementKind matches.

### C-07 — complete mediation honesty

Hard enforcement is claimed only for operation classes whose producing capabilities are all mediated within the declared actor-scoped CapabilityUniverse.

### C-08 — profile mismatch

Adapter capability drift invalidates affected hard-enforcement claims.

### C-09 — immutable RuntimeGeneration

Profile/resolver/classifier changes create a new generation; they never mutate an active one invisibly.

### C-10 — exact decision binding

ALLOW binds exact proposal, intents, cognition revision, policy, requirements, evidence, profile and runtime generation.

### C-11 — stale decision rejection

A changed proposal or bound input cannot execute with an old ALLOW.

### C-12 — single-use authorization

One action decision cannot authorize another action or a second execution.

### C-13 — conjunctive Host authority

Cognitive ALLOW without execution-authority admission cannot produce machine-local execution.

### C-14 — post-action tri-state

Unknown completion is represented as Indeterminate, not guessed into Success or Failure.

### C-15 — reconciliation barrier

An indeterminate side effect blocks equivalent/conflicting mutations in its EffectScope until authoritative reconciliation.

### C-16 — no blind retry from uncertainty

Unknown completion never automatically produces FailureFingerprint or RetryFailure permission.

### C-17 — failure causality

A known tool failure automatically changes the next equivalent control state without participant memory.

### C-18 — correlation integrity

Concurrent actions retain exact pre/post correlation; duplicate same-content delivery is idempotent and conflicting reuse fails.

### C-19 — restart integrity

A RuntimeHost restart cannot forget an unresolved dispatched mutation and then permit an unsafe equivalent retry.

### C-20 — canonical write direction

Runtime may emit cognition proposals but cannot commit canonical cognition directly.

### C-21 — participant replacement

Changing the reasoning participant preserves control semantics.

### C-22 — harness replacement

Changing the harness preserves core component semantics after adapter conformance.

### C-23 — claim-channel honesty

The first landing does not claim hard enforcement of arbitrary free-response canonical/live claims.

### C-24 — no authority cutover

Accepting or implementing this component architecture does not change the canonical authority provider.

---

# 89. Minimum executable proof program

The first implementation program should prove the architecture in this order:

```text
RCA-1
RuntimeGeneration + DeploymentProfile + adapter manifest handshake

RCA-2
ObservedAction + correlation + deterministic IntentSet

RCA-3
Pinned canonical Snapshot + frozen policy derivation

RCA-4
RequirementResolverRegistry + typed evidence receipts

RCA-5
BeforeJudgment deny / preparation / re-deliberation

RCA-6
BeforeExecution preparation

RCA-7
DecisionBinding + stale-decision rejection

RCA-8
concurrent ControlTransactions

RCA-9
PostActionObserver tri-state

RCA-10
known failure → FailureFingerprint → retry control

RCA-11
Indeterminate → ReconciliationBarrier

RCA-12
restart/recovery proof

RCA-13
ExecutionAuthorityPort composition

RCA-14
real harness adapter conformance

RCA-15
participant replacement

RCA-16
multi-harness/profile conformance
```

Do not implement all resolver backends or storage migration before these core causal proofs exist.

---

# 90. Detailed C++ design boundary

Only after this Component ADL is accepted should the next document freeze concrete C++ types and interfaces.

That design should produce, at minimum, types corresponding to:

```text
RuntimeHost

RuntimeGeneration

DeploymentProfile
GovernedActorSet
CapabilityUniverse
MediationInventory

AdapterRegistration
ObservedAction
CorrelationKey
ControlTransactionId
EffectScope

IntentClassification
IntentSet

PinnedCognition

RequirementInstance
ResolverBinding
EvidenceReceipt
PreparationState

ExecutionDecision
DecisionBinding

PostActionObservation
ExecutionOutcome

FailureFingerprint
ReconciliationBarrier

RuntimeControlState
```

and ports corresponding to:

```text
ActivationBoundaryPort
ActionInterceptionPort
PostActionObservationPort
OutboundClaimPort

CanonicalCognitionPort
CognitionProposalPort

LiveFactPort
LowerLayerSearchPort
ToolContractPort
MechanicalCheckPort
HumanHandoffPort

ExecutionAuthorityPort
RuntimeStateStore
```

The C++ design must preserve this ADL rather than silently collapsing components for convenience.

Two standing design constraints apply from day one:

- **Lower-layer reuse.** The native implementation consumes
  `qiven-foundation` primitives and must not re-implement
  foundation-owned contracts (software-engineering-philosophy sections
  1 and 7; draft pit P-49: the preflight for any new capability includes
  a Foundation-surface survey before writing local code).
- **Adapter channel authentication.** The C++ design must fix the
  adapter-instance credential/attestation model required by Section 12;
  it may not ship with an unauthenticated adapter channel.

---

# 91. Central architectural invariant

The entire component model can be reduced to one causal chain:

```text
OBSERVE
    ↓
CLASSIFY WITHOUT NARROWING
    ↓
PIN ONE COGNITIVE WORLD
    ↓
DERIVE POLICY REQUIREMENTS
    ↓
SATISFY THEM THROUGH TRUSTED MECHANISMS
    ↓
BIND THE EXACT DECISION
    ↓
RECHECK FRESHNESS
    ↓
COMPOSE WITH EXECUTION AUTHORITY
    ↓
EXECUTE THE SAME PROPOSAL
    ↓
OBSERVE SUCCEEDED / FAILED / INDETERMINATE
    ↓
FAILURE → LEARN/RETRY CONTROL
UNKNOWN → RECONCILE
    ↓
ONLY GOVERNED PROPOSALS MAY FLOW BACK TOWARD CANONICAL COGNITION
```

Anything that shortcuts that chain is outside the hard-governed Runtime model.

---

# 92. Final architecture statement

The Runtime is not an LLM memory plugin.

It is not a tool router.

It is not a second Host broker.

It is not a second canonical store.

It is not a semantic wrapper around MCP.

Its role is narrower and stronger:

> **Qiven Runtime is the trusted component system that converts observable participant proposals into policy-derived, evidence-backed, exact-action decisions at the boundary where Judgment becomes world-changing Mechanism.**

The participant remains free to think.

The mechanism remains free to be replaced.

The harness remains free to be replaced.

Storage remains free to be replaced.

But a governed action cannot cross the boundary while required cognition is missing, its evidence is untrusted, its authorization is stale, its execution authority is absent, or the effect of a prior equivalent action is still unknown.

That is the component-level realization of frozen v4 Cognitive Control.

---

## 93. Landing provenance

Authored by the project owner as the Follow-on Design Boundary document
named by ADR-0038; reviewed by jason-extended-cognition (served by
GLM-5.3 non-flash, reasoning max, disclosed per ADR-0035 rule 4) on
2026-09-21. Review verified the 16 inherited ADR-0038 constraints, the
IntentSet union semantics against the frozen per-intent derivation and
list-based PreparationPacket readiness (`qiven-context-draft`@
`4cbc995`, `include/qiven/context/runtime.hpp`), and the ActionKind
vocabulary of the frozen enum. Owner adjudication accepted the review
findings F1-F6 the same day:

- F1 adapter-to-RuntimeHost channel authentication named as a
  requirement (Section 12) and a mandatory C++ design item (Section 90);
- F2 post-execution terminal-state names unified with Section 19
  (Succeeded / KnownFailed);
- F3 capability-manifest handshake honesty boundary stated explicitly
  (Section 11);
- F4 PinnedCognition may be a physical lazy view of the pinned revision
  (Section 23);
- F5 append-only control-decision audit view added to RuntimeStateStore
  (Section 56);
- F6 landing home and baseline: this repository, `qiven-runtime`,
  created from the parked Devkit bootstrap shell (template 0.1.5);
  canonical baseline at review `e1623290`, landing-day canonical main
  `2a8f34e`.

Day-one design constraints recorded at landing: lower-layer reuse
(qiven-foundation consumption, Foundation-surface preflight survey) and
adapter channel authentication (Section 90).

Landed 2026-09-21 as the first content of `qiven-runtime`'s
`docs/architecture/`.
