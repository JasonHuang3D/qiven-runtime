# Runtime C++ Detailed Design — Cognitive Control Runtime Types and Ports

Status: **accepted design** (owner-directed 2026-09-21, one turn with
the layer contracts). Implements Component ADL §90 against the frozen v4
semantics (`qiven-context-draft` @ `4cbc995`) and conforms to
`qiven-foundation/docs/architecture/cpp-architecture.md`. Satisfies
every ADR-0038 requirement; the compliance map is §14.

---

## 1. Design basis and reading order

1. Component ADL (`docs/architecture/runtime-component-adl.md`) —
   components, causality, state machines (accepted).
2. Frozen v4 draft types — the semantic definitions the Runtime
   realizes: `qiven::context::Snapshot/InvocationPolicy/ActionIntent/
   CognitiveRequirement/PreparedRequirement/PreparationPacket/
   derive_requirements/ready_for_*` (`runtime.hpp`), value tree
   (`cognition.hpp`), serialization (`persistence.hpp`).
3. Layer contracts (foundation doc above).

The Runtime NEVER redefines a frozen semantic. Where the ADL introduces
a runtime concept (IntentSet, ControlTransaction, ExecutionDecision...),
it is a NEW type in `qiven::runtime` composed around draft types.

## 2. Library topology (this repository)

```text
qiven-runtime        (STATIC)   control plane: identity, generation,
                                transactions, decisions, ports, state
qiven-runtime-loopback (STATIC) deterministic in-process adapter +
                                simulated mechanisms (RCA proof driver)
qiven-runtime-app    (Apps)     RuntimeHost composition root
tests/qiven-runtime-<topic>     one executable per RCA cluster
```

Dependencies: `qiven::foundation`, `qiven::context` (draft, pinned).
Namespace layout:

```text
qiven::runtime            core control plane
qiven::runtime::identity  strong ids and digests
qiven::runtime::port      port interfaces
qiven::runtime::adapter   adapter contracts + loopback
qiven::runtime::resolver  resolver contracts + loopback resolvers
```

## 3. Identity types (`identity.hpp`)

All ids are strong value types (no raw strings across APIs):

```cpp
enum class IdKind : std::uint8_t { Generation, Transaction, Adapter,
                                   Session, Actor, Capability, Evidence,
                                   Decision, Requirement, EffectScope };
struct RuntimeGenerationId { std::uint64_t value; };  // monotonic per host
struct ControlTransactionId { std::uint64_t value; }; // monotonic per host
struct AdapterInstanceId  { qiven::u64 fnv; };  // stable across restarts
struct EvidenceId         { qiven::u64 fnv; };  // unique per receipt
struct DecisionToken      { qiven::u64 fnv; };  // single-use
struct ContentDigest      { std::array<std::byte, 32> sha256; };
```

Rules: human-facing logs render ids as `<kind>-<16 hex>`; `fnv1a64`
backs ephemeral correlation identity; `ContentDigest` (SHA-256) backs
integrity binding (decisions, evidence content) per layer-contract §3.6.
`CorrelationKey` is the ADL §17 five-tuple, equality-comparable,
hashable, immutable.

## 4. Observation and classification

```cpp
struct ObservedAction {           // immutable for the control attempt
    AdapterInstanceId adapter; HarnessSessionId session;
    ActorInstanceId actor;        // from accepted binding, never NL claims
    CapabilityId capability; std::string operation;
    std::string target;           // resource identity
    ContentDigest argumentDigest; // sha256 over canonical argument bytes
    qiven::OwnedVector<std::byte> argumentBlob; // retained for binding
};
struct IntentClassification {     // basis retained (ADL §22)
    qiven::context::ActionIntent intent;
    ClassificationBasis basis;    // Mechanical|Structural|Declared|Judgment
};
struct IntentSet {                // ADL §20/21: never narrowed
    qiven::OwnedVector<IntentClassification> members; // ≥1, deduped
};
```

`IntentClassifier` is a pure function over `ObservedAction` + structural
facts: `(ObservedAction, StructuralFacts) -> IntentSet`. Over-
approximation rule (ADL §21) is enforced by a test that injects an
ambiguous action and asserts the union of requirements (C-02).

## 5. Generation and profile

```cpp
struct DeploymentProfile {        // immutable; built via ProfileBuilder
    ProfileRevision revision;
    GovernedActorSet actors;      // accepted adapter/session bindings
    CapabilityUniverse capabilities;  // CapabilityDescriptor list
    MediationInventory mediation;     // CapabilityId -> interception path
    ClaimChannelCoverage claims;      // tool-mediated | free-text: none
};
struct RuntimeGeneration {        // everything immutable while active
    RuntimeGenerationId id;
    DeploymentProfile profile;
    ResolverRegistry resolvers;       // RequirementKind -> ResolverBinding
    ClassifierContract classifier;    // accepted version + uncertainty rule
    qiven::context::Snapshot minimumCognition; // pinned floor at startup
    std::vector<AdapterManifest> admittedAdapters;
};
```

`ProfileBuilder` is C++ code (no prompt synthesis, ADL §7); a file
format is deferred to the real-adapter batch. Handshake mismatch
(profile vs live adapter manifest) fails closed for affected classes
(ADL §11; test C-08).

## 6. Transactions and requirement flow

```cpp
struct ControlTransaction {       // one exact proposal (ADL §18/19)
    ControlTransactionId id;
    std::optional<ControlTransactionId> causalParent;  // re-deliberation
    CorrelationKey correlation;
    ObservedAction action;
    IntentSet intents;
    PinnedCognition cognition;     // §7 below
    qiven::context::PreparationPacket packet;  // FROZEN draft type
    TransactionPhase phase;        // the ADL §73 state machine
};
struct RequirementInstance {      // runtime provenance wrapper
    RequirementIdentity identity; // kind+subject+boundary+blocking+rule
    qiven::context::CognitiveRequirement spec;  // frozen semantics
    qiven::OwnedVector<std::uint32_t> originIntents; // indices into set
};
```

Derivation calls the DRAFT's `derive_requirements` per intent and unions
with provenance (dedup resolution ≠ requirement deletion, ADL §26).
Both ADL semantic gates reduce to the draft predicates:
`JudgmentContinuationAllowed == packet.ready_for_judgment()` and
`ExecutionAllowed == packet.ready_for_execution()` (§7 of ADR-0038
inherited contract; preparation-failure fails both — draft S0-02).

## 7. Cognition access

```cpp
struct PinnedCognition {
    qiven::context::RevisionId revision;   // draft type
    const qiven::context::Snapshot* snapshot; // non-owning; generation-
                                              // owned; lazy-view allowed
    ContentDigest policyDigest;            // sha256 of serialized policy
};
class CanonicalCognitionPort {
public:
    virtual ~CanonicalCognitionPort() = default;
    virtual PinResult pin(PinRequest) = 0;  // Result<PinnedCognition, PinError>
};
```

First implementation: `DraftSnapshotReader` parses the draft's
golden-pinned serialization (persistence.hpp, format v8) from a file.
Advancing the canonical revision stales unconsumed decisions (ADL §41;
revision-level invalidation, test C-11).

## 8. Ports (`port/`)

All ports are abstract interfaces; no exceptions; `Result` returns.

```text
8.1  IActionInterceptionPort   observe_proposal / allow / deny /
                               return_context (ADL §12-14)
8.2  IActivationBoundaryPort   activation events; creates receipts (§66)
8.3  IPostActionObservationPort  tri-state outcome reports (§15)
8.4  IOutboundClaimPort        tool-mediated claims only in first
                               landing; free text is NotGoverned (§16/67)
8.5  IExecutionAuthorityPort   admission/lease/fencing — Host semantics;
                               Runtime cannot mint (ADL §44/85)
8.6  LowerLayerSearchPort      attributable search evidence (§35)
8.7  ToolContractPort          accepted invocation shape (§36)
8.8  MechanicalCheckPort       binds exact artifact digest (§37); real
                               subprocess execution waits on qiven-process
8.9  HumanHandoffPort          typed H1-H4 request/receipt (§38)
8.10 LiveFactPort              live verification evidence
8.11 CognitionProposalPort     proposals only; never writes canonical
```

Day-one implementations: `LoopbackAdapter` (8.1-8.4) with a scripted
harness model; `NullExecutionAuthority` (8.5, fail-closed DENY for real
mutation + simulated dispatch for proofs); loopback resolvers (8.6-8.10)
plus `FileProposalLog` (8.11). Real ZCode adapter and Host binding land
at RCA-13/14.

Adapter channel authentication (ADL §12/§90 day-one constraint): the
adapter registration carries a credential; first landing is in-process
(loopback registers directly with a fixed credential) so the CONTRACT is
`RegistrationRequest{manifest, credential}` validated by RuntimeHost;
the named-pipe/localhost transport with per-install tokens is specified
with the real adapter batch.

## 9. Concurrency model

- ONE control thread owns `RuntimeControlState` and all transaction
  phase transitions (deterministic ordering, ADL §5).
- Adapter/resolver threads NEVER touch state; they post
  `IngressMessage` values into a bounded MPSC queue drained by the
  control thread.
- Independent resolvers run concurrently on a bounded executor
  (`qiven::runtime::Executor`, N workers, no unbounded queues);
  readiness is evaluated only after all blocking instances report
  (ADL §30/§62).
- Duplicate CorrelationKey + identical content → idempotent replay;
  different content → integrity failure (ADL §60). Out-of-order events
  are never guessed into place (ADL §61).

## 10. Decisions

```cpp
struct ExecutionDecision {       // immutable value; ADL §39
    DecisionToken token; ControlTransactionId transaction;
    RuntimeGenerationId generation;
    ContentDigest actionDigest; ContentDigest intentSetDigest;
    qiven::context::RevisionId cognitionRevision;
    ContentDigest policyDigest;  ContentDigest requirementSetDigest;
    ContentDigest evidenceSetDigest;
    ProfileRevision profile;     ResolverRegistryRevision resolvers;
    std::optional<ClassifierContractId> classifier;
    Disposition disposition;     // Allow | ReDeliberate | Await | Deny |
                                 // NotGoverned (ADL §75)
};
```

Single-use: `DecisionToken` consumption is recorded in state; stale
inputs invalidate (ActionAdmissionCoordinator final freshness check,
ADL §42; tests C-10/C-11/C-12). Host admission is a separate live gate
never embedded in the decision (§43/85).

## 11. Observation, failure, reconciliation

`PostActionObserver` validates correlation and classifies
Succeeded/Failed/Indeterminate (Indeterminate mandatory on unprovable
completion, ADL §15; test C-14). Failed → `qiven::context::
FailureFingerprint` (draft type) → `FailureTracker` (index by actor/
tool/fingerprint/EffectScope; influences the next RetryFailure intent,
test C-17). Indeterminate → `ReconciliationBarrier` over `EffectScope`
(blocks equivalent/conflicting mutations in scope until
`ReconciliationCoordinator` resolves via authoritative observation;
StillIndeterminate keeps the barrier, ADL §49-53; tests C-15/C-16).
Restart: `RuntimeStateStore` interface first implemented in-memory
(ADL §57 acceptance levels); production mutating profile requires the
restart-capable store or Host-journal reconstruction (C-19).

## 12. Foundation contributions (RCA-0, P-49 discipline)

1. **Result vocabulary** (OBL trigger fires — resolver/receipt/pin
   returns need it): `qiven::Result<T, Reason>` — typed reason enum or
   small struct, `[[nodiscard]]`, no heap on the failure path, no
   exceptions. Landed and tested in foundation first; Runtime consumes.
2. **SHA-256**: `qiven/hashing_sha256.hpp` (integrity-grade digests;
   golden test vectors).
3. Existing reuse (no reimplementation): `fnv1a64`/`to_hex_u64`,
   `ByteWriter`/`endian` for digest preimage serialization, checked
   conversions, `u64/usize`, allocator protocol for argument blobs.

## 13. Repository layout

```text
include/qiven/runtime/identity.hpp  generation.hpp  profile.hpp
     observed_action.hpp  intent.hpp  transaction.hpp  decision.hpp
     outcome.hpp  failure.hpp  reconciliation.hpp  state.hpp  executor.hpp
     port/*.hpp  adapter/*.hpp  resolver/*.hpp  result_aliases.hpp
src/            one .cpp per header family
apps/host_main.cpp                 (qiven-runtime-app)
adapters/loopback/                 scripted adapter + simulated tools
tests/  runtime-generation  correlation  derivation  before-judgment-path
        decision-binding  stale-decision  tri-state  reconciliation
        restart  concurrency  publish-boundary  replacement  ...
```

Tests are named after the proof obligation they discharge; the mapping
table below is normative.

## 14. Compliance map (ADR-0038 -> enforcement locus)

| ADR-0038 requirement | Type / mechanism | Test |
| --- | --- | --- |
| External to Judgment; no self-invocation | port ingress only; control thread | C-01 loopback |
| ObservedAction ≠ ActionIntent | §4 types; authority asymmetry | intent tests |
| Declaration advisory | classifier basis; mechanical floor | C-02 union |
| Triggers vs deadlines (orthogonal) | §6 + draft predicates | derivation tests |
| BeforeJudgment late discovery | ReDeliberate disposition + causal parent | C-04 |
| Trusted satisfaction only | EvidenceReceipt resolver binding | C-05/C-06 |
| Complete mediation (profile) | DeploymentProfile + handshake | C-07/C-08 |
| Immutable generation | RuntimeGeneration | C-09 |
| Exact binding / staleness / single-use | ExecutionDecision + admission | C-10..C-12 |
| Conjunctive host authority | IExecutionAuthorityPort | C-13 |
| Tri-state + reconcile + no blind retry | §11 barrier model | C-14..C-16 |
| Failure causality | FailureTracker → derivation input | C-17 |
| Correlation integrity | idempotent replay / conflict fail | C-18 |
| Restart integrity | RuntimeStateStore contract | C-19 |
| Canonical upstream / proposals only | PinnedCognition / ProposalPort | C-20 + audit |
| Participant / harness replacement | ports + loopback determinism | C-21/C-22 |
| Claim honesty (tool-mediated first) | ClaimChannelCoverage | C-23 |
| No cutover | no storage migration in scope | C-24 |

## 15. Deliberately deferred

Free-text claim classification; profile file format; real adapter
transport + credential issuance; real subprocess MechanicalCheck (waits
on qiven-process); K5 transport (OBL-B6C1D8); restart-capable store
selection; Host production binding. Each deferral is explicit scope,
not hidden backlog (ADL §67 honesty rule).

## 16. Provenance

Designed 2026-09-21 in one turn together with the layer contracts
(owner direction), satisfying ADR-0038 as amended and the Component ADL
§90 type/port inventory. Semantic reuse verified against
`qiven-context-draft@4cbc995` public headers; foundation day-one
contributions follow the P-49 lower-layer survey discipline.
