# CA-1 — Native Deterministic Activation Core: Design

Status: DESIGN (v41 session, 2026-09-28; implementation follows under the
CA-0 bounded-batch declaration — ≤3 runtime PRs + 1 context policy PR +
1 devkit schema PR; stall law on failed exit).
Authority: roadmap amendment §4 CA-1; TCA architecture §0/§7/§9-§12/§15/
§17/§18; selector schema v1 (`qiven-context runtime/cognition/
selector-schema.md`); acceptance protocol §5 (Profile A) / §6 (Profile B);
CA-0 exit report (sealed corpus + declarations).

## 0. Shape in one paragraph

A `cognition activate` path inside qiven-runtime that, given a pinned
canonical bundle (existing `qiven-cognition-bundle-v1` + its execution
`RuntimeGeneration`) and a normalized task descriptor, resolves a
complete external source lock from validated local checkouts, builds (or
reuses, by exact cache key) an activation-index sidecar with its own
`ActivationGeneration`, evaluates the typed selector policy (P0/P1
exact; P3/P4 deterministically ranked), publishes an immutable
`TaskCognitionBundle` atomically, and issues a `ContextActivationReceipt`
that binds every generation/policy/task/budget fact and invalidates on
any change. One-shot runtimectl verbs over the same core the resident
host refresh worker will use. No network, no LLM, no Python on the
production path; byte-identical closures produce byte-identical
canonical outputs.

## 1. Component decomposition (all new code under `cognition/`)

| Component | Files (include/qiven/runtime/cognition/ + src/cognition/) | Role |
| --- | --- | --- |
| SourceLockBuilder | `source_lock.hpp/.cpp` | Resolve the CA-0 repo/filter table from validated LOCAL checkouts (exact HEAD, clean tree, per-file sha256, no network); emit `SourceLock` (sorted entries: repository, commit, tree oid, path, content sha256, bytes) + its canonical digest |
| ActivationPolicyLoader | `activation_policy.hpp/.cpp` | D-4-style bounded parsers for `cognition-core.yaml` and `cognition-activation-policy.yaml` (fail-closed, line-typed errors; schemas owned by this design §3) |
| ActivationIndex | `activation_index.hpp/.cpp` | Build the sidecar `index.sqlite` per TCA §9.3 tables (sources/relations/selectors/rule_sources/capabilities/obligations/documents) with a deterministic inverted index (no FTS dependency); `index-manifest.json` binding bundle digest + RuntimeGeneration + policy digest + source-lock digest + schema version + publisher build; ActivationGeneration = sha256 cache key per TCA §9.4 |
| TaskNormalizer | `task_descriptor.hpp/.cpp` | The fixed condition-blind normalizer: envelope {objective, repository, revision, changed paths, phase, risk} → typed TaskDescriptor; judgment-bearing fields stay empty unless mechanically derivable (path-prefix→subsystem/boundary, extension→language, textual IDs) |
| SelectorEvaluator | `selector_eval.hpp/.cpp` | Typed-only P0/P1 evaluation (schema v1 vocabulary; unknown enum = `unresolved`, visible), P2 inclusion, deterministic P3/P4 lexical ranking within budget; readiness result per §10 step 10 |
| TaskBundlePublisher | `task_bundle.hpp/.cpp` | Assemble + atomically publish `TaskCognitionBundle` (compact core ≤12 KiB, payload ≤64 KiB, inlined body ≤8 KiB, supporting ≤25%); manifest binds both generations + lock + policy digests |
| ActivationReceipts | `activation_receipt.hpp/.cpp` | Issue/persist/explain/invalidate/verify `ContextActivationReceipt`; persistence = the activation sidecar's receipt journal (SQLite, own schema; RuntimeJournal records control facts only — CA-4 will absorb) |
| ActivationService | `activation_service.hpp/.cpp` | The shared core: pin bundle → ensure index (build/reuse by exact key) → normalize task → evaluate → publish bundle → receipt; used by BOTH the one-shot CLI and (interface-ready) the host resident worker |
| CLI | `apps/runtimectl_main.cpp` (index + activation verbs) | `cognition activate`, `cognition activation show|explain|verify-receipt`, `cognition index status|rebuild` (TCA §13.2 verb set; the activate verb validates every operator-supplied axis against the ACTIVE sidecar manifest before minting a receipt) |

Naming: nothing here touches `port/activation.hpp` (MVP-0
evidence-activation — a different concept, kept distinct).

## 2. Source lock (WR-7-critical deliverable)

- Repo/filter table = CA-0 declaration, carried IN the activation policy
  instance (not compiled in): qiven-context, qiven-devkit,
  qiven-foundation, qiven-runtime filters per the digest; qiven-docs
  deliberation records only when P4-referenced.
- Resolution mechanics reuse `CanonicalCognitionPublisher`'s git-plumbing
  patterns (rev-parse/ls-tree/cat-file at an exact commit; never the
  working tree): per filter, enumerate tracked paths at the pinned
  commit, hash blob content (sha256), record tree oid. Dirty checkout =
  typed failure (validated-local law).
- `SourceLock` canonical serialization: entries sorted by
  (repository, path); digest = sha256 over the canonical JSON
  (RFC-8785-subset canonicalization per devkit resolver precedent —
  reuse the devkit canonicalization recipe as a ported, tested subset,
  no new generic codec).
- Binding: `external_source_lock_sha256` + per-entry digests appear in
  the index manifest, task-bundle manifest, and receipt; any movement
  inside the selected closure changes ActivationGeneration; movement
  outside does not.

## 3. Policy instances (the context policy PR)

Two canonical instances land in qiven-context `runtime/cognition/` with
schemas documented in-repo (validated by the runtime loader; devkit
schema PR registers the JSON Schemas under `docs/schemas/cognition/`):

- `cognition-core.yaml`: schema_version, corpus table (repositories +
  path filters), capability-surface pointers (foundation/devkit
  capability files), budget table (§15.1 constants), schema registry
  (task-schema, selector-schema versions).
- `cognition-activation-policy.yaml`: schema_version, rule table for
  v1: each rule = rule_id, priority_class, selector (schema-v1 typed
  fields), source_ref (repository/path/anchor), expected_controls,
  independent_evidence, lifecycle. The initial protected bootstrap set =
  the CA-0-enumerated scar records (F-01..F-08 mappings) + the four TCA
  documents' section-level selectors via the landing manifest.

Both parse via bounded line-parsers in the D-4 family (strict subset,
block style, ≤256 KiB, ≤512 rules, fail-closed line-typed errors) — no
general YAML in runtime.

## 4. Determinism, publication, crash

- Canonical selection outputs carry no timestamp/random/volatile path;
  `bundle_id` derived without self-reference; issuance nonce/time live
  only in the receipt envelope (Profile A.1/A.2).
- Sidecar root: `<runtime_root>/activation-generations/<id>/`; built in
  `private` staging (`.tmp-<id>` sibling), schema/reference/lifecycle/
  digest validated, then atomic rename of the ACTIVE pointer (same
  pattern as bundle publish). Crash before switch → candidate deleted on
  next open; after switch → pointer+manifest recovery, digest-exact
  classification. Partial indexes never visible (Profile A.8).
- Index loss → full rebuild from pinned sources (Profile A.9); canonical
  mutation never blocks on bundle generation (generation is read-path
  only; Profile A.10 via the existing publisher's immutability).
- v1 = full rebuild per new generation; incremental is prohibited until
  measurement demands (TCA §9.4).

## 5. Test plan (the runtime PRs' exit evidence)

- **Profile A evidence** (delivered shape, amended 2026-09-28): the 12
  §5.1 conformance rows are carried DISTRIBUTED across the five unit
  suites (source-lock: missing checkout / revision mismatch / dirty
  tree / exact-revision identity; activation-policy: unknown-schema /
  missing-field / vocabulary-visible failures + the published-instance
  parse; activation-index: crash staging / pointer atomicity / loss
  rebuild / manifest-corruption typed rejection; task-bundle-receipt:
  determinism / bindings / replay matrix / budget-axis identity;
  activation-conformance: 100x repeated full-pipeline activations with
  byte-identical canonical outputs and envelope-only variance).
  100x determinism runs in-process over the stateless pipeline (the
  binary itself is the clean-process boundary: stateless across runs,
  workroot-isolated, both gate configs); a subprocess-restart loop adds
  no discriminating power at v1 and is deferred with that rationale.
- **Profile B suite** (`tests/cognition_protected_recall.cpp`): sealed
  corpus F-01..F-08 copied at exact sealing digests; MUST-INCLUDE
  recall; the §6.2 thresholds; budget-pressure at 100/75/50% plus the
  starvation row; the 7 mutation classes.
- **Budget measurement** (`tests/cognition_budget_measurement.cpp`):
  cold rebuild and warm activation timings on a synthetic corpus at the
  CA-0 sealed scale with declared pass thresholds at 2x headroom, exact
  numbers printed as evidence rows. KNOWN v1 limitation (disclosed):
  the lock's blob hashing spawns one `git cat-file` per file — measured
  4768 ms at the 120-file synthetic scale, close to the 5000 ms
  objective; the batch-mode (`git cat-file --batch`) optimization path
  is named and lands when the real corpus measurement demands it.

## 6. PR split (within the ceiling)

1. **runtime PR-1**: source lock + policy loaders + index + cache key +
   `cognition index rebuild|status` + unit tests (determinism of lock +
   index at fixed checkouts; fault rows: missing checkout, revision
   mismatch, dirty tree, unknown schema).
2. **runtime PR-2**: task normalizer + selector evaluator + bundle
   publisher + receipts + `cognition activate|activation show|explain|
   verify-receipt` + Profile A suite + budget measurement.
3. **runtime PR-3**: Profile B suite + sealed-corpus harness + mutation
   classes + budget-pressure + exit-report evidence.
4. **context PR**: `runtime/cognition/cognition-core.yaml` +
   `cognition-activation-policy.yaml` + schema docs (lands FIRST as the
   validated input for PR-1's loader tests; order: context PR → PR-1 →
   PR-2 → PR-3).
5. **devkit PR**: `docs/schemas/cognition/*.schema.json` registration.

## 7. Implementer decisions on the open questions (binding for this batch)

1. Policy schemas: authored by this design (§3) — the loaders and the
   instances co-design; the context PR carries the canonical text.
2. Reference corpus: latency budgets measured on the LOCKED corpus
   (CA-0 scale); recall budgets on the sealed fixture corpus. Both
   reported in the exit evidence.
3. Sidecar root: `<runtime_root>/activation-generations/` (sibling to
   `bundles/`, same ownership).
4. Index text search: deterministic inverted index only (FTS5 explicitly
   not required; protected selection never depends on it).
5. Receipt persistence pre-CA-4: activation sidecar SQLite journal
   (own schema; RuntimeJournal carries control facts only).
6. Positive/negative condition selectors: v1 = typed fields only
   (schema-v1 vocabulary); condition expressions are NOT in v1 scope —
   recorded as a v2 extension point, not silently approximated.
7. CLI verbs mapped per §1 (house grammar: `cognition <noun> [verb]`).
8. Task-schema import: devkit JSON Schema vendored at the pinned devkit
   revision through the source lock (a locked doc file — no runtime
   import mechanism invented).
9. Requested budget vs policy table: min(requested, policy class cap);
   protected overflow → `BudgetInsufficient` (never silent truncation).
10. Live evidence: v1 carries the freshness fields specified by §12.2;
    binding of external mutable-state receipts beyond that is CA-2+
    scope (declared, not approximated).
11. Consumer profiles: enumerated in `cognition-core.yaml` (v1:
    zcode-jason), validated against the enum.
12. `published_at` on bundle v1: remains on the MVP-2 manifest (its own
    lifecycle); §11.2 determinism governs canonical SELECTION files and
    the ACTIVATION artifacts this batch creates — the index manifest
    binds the bundle by DIGEST, so the timestamp never enters a
    canonical hashed payload.

## 8. Honest scope notes

- The resident-service integration (host worker calling the shared core)
  is interface-ready but not switched on: activation remains CLI-driven
  in CA-1; the host consumes CA-2's ingress gate.
- No Profile C (fresh-consumer utility) work in this batch — CA-1's exit
  gate is Profiles A+B only (roadmap); Profile C rides CA-2+.
- The English wording of every typed selector enum value is law (schema
  v1 exact vocabulary); any vocabulary growth is a schema-version bump,
  never a silent extension.
- Sidecar table set, dated amendment (2026-09-28 review revision): the
  delivered index.sqlite carries sources/selectors/rule_sources/
  documents/inverted with v1-adapted columns; the TCA §9.3
  relations/capabilities/obligations tables are DEFERRED to the first
  batch that consumes them (CA-2 ingress / the resident worker), with
  the index schema version as the compatibility axis. Failure signal:
  a consumer needing cross-source relations or capability rows must
  bump index_schema_version and rebuild (the full-rebuild-per-generation
  law already guarantees correctness). rule_sources binds the REAL
  sources row (out-of-closure rule sources fail closed) — no placeholder
  joins ship.
