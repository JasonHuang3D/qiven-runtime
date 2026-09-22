# MVP-2 — Native Cognition Publisher and DeploymentProfile (batch design)

Status: **batch design for MVP-2** (design-first standard; extends
`docs/architecture/runtime-production-mvp-cpp-design.md` — cited below as
**DESIGN** — implementing `docs/architecture/runtime-production-mvp-architecture.md`
— cited below as **ARCH** — §3, §7, §15 MVP-2, §16.1 rows 8-11).

Batch: MVP-2 of OBL-20260922T155800Z-9A7B41. Exit gate (ARCH §15):

1. dirty checkout changes cannot affect a pinned RuntimeGeneration;
2. modifying any bundle file makes bundle loading fail;
3. Git revision and content digest are separately visible;
4. a profile or bundle update creates a new generation and invalidates
   old unconsumed tokens;
5. rich source records are not lossy-converted into the v8 Snapshot.

---

## 1. Basis

- ARCH §3 (DeploymentProfile, governed scope, complete-mediation claim),
  §7 (production cognition delivery: no dirty-checkout reads, bundle v1,
  single machine-readable policy source, refresh/generation rules),
  §15 MVP-2 work list, §16.1 verification rows (stale decision, payload
  change, resolver mismatch), §10.2 `generations`/`decisions` tables.
- DESIGN §8 (cognition bundle & publisher), §9 (profile subsystem),
  §2 ground rules (GR-1..GR-7), §5 error taxonomy, §6 identity, §16 test
  spine (`bundle_publish_pin`, `profile_accept`), §19 D-4 (no general
  YAML in runtime).
- MVP-0 landed ports this batch implements: `port/cognition_bundle.hpp`
  (`ICognitionBundlePort`, `CognitionBundleManifest`), `port/deployment_profile.hpp`
  (`IProfilePort`), `port/cognition_port.hpp` (`DraftSnapshotReader`,
  `PinnedCognition.source_revision`).
- MVP-1 landed journal: `RuntimeJournal::advance_generation` exists but
  does NOT stale-mark — this batch extends it (§3.6, exit gate 4).
- qiven-context lands `runtime/invocation-policy.yaml` (ARCH §7.3, the
  machine instance of the existing v4 `InvocationPolicy` specification);
  that repository change rides this batch as its own PR.

### Decisions recorded against DESIGN (deltas, not silent deviations)

1. **`processx/` first form lands HERE, not MVP-3.** ARCH §15 MVP-2
   requires "implement exact Git commit/tree reading"; DESIGN §8 routes
   git plumbing through the process runner. The runner therefore lands
   in MVP-2 as its bounded first form (explicit argv + cwd, capped
   stdout/stderr capture via reader threads, deadline → TerminateProcess,
   no shell). The MVP-3 slice then HARDENS it per DESIGN §11 (Job Object
   kill-on-close tree reclamation, environment allowlist, executable
   allowlist by version/digest) and carries the adversarial exit-gate
   tests. Split rationale: MVP-2 needs bounded child execution for git
   plumbing only; the security hardening is MVP-3's named work.
2. **`jsonx/` first form lands HERE** (manifest encode/decode only:
   objects, strings, u64, bool, arrays of objects; depth ≤ 8, total
   ≤ 1 MiB, no floats — the manifest has none). IPC bodies (MVP-3)
   reuse and may extend it. D-1 (first-party bounded codec, no vendored
   parser) governs both.
3. **Per-file `git cat-file blob` instead of streaming `--batch`.**
   DESIGN §8 names `cat-file --batch`; the bounded source set (≤ 256
   files, ≤ 64 MiB total, profile-capped) makes one process per blob the
   simpler fail-closed form. Streaming batch is a recorded optimization
   deferral (§8), not a semantic difference.

## 2. Module map

New files (migration law, DESIGN §3 — target subdirectories from the start):

```text
include/qiven/runtime/
  jsonx/json_codec.hpp           bounded first-party JSON codec (manifest grade)
  processx/process_runner.hpp    bounded child execution (first form)
  cognition/policy.hpp           strict invocation-policy parser (D-4 subset)
  cognition/bundle.hpp           BundleStore: layout, verify, ACTIVE pointer
  cognition/publisher.hpp        CanonicalCognitionPublisher (git-plumbing-driven)
  host/deployment_profile.hpp    FileProfileSource: IProfilePort + instance format
  host/generation_activation.hpp journal-backed generation activation
src/jsonx/json_codec.cpp
src/processx/process_runner.cpp
src/cognition/policy.cpp
src/cognition/bundle.cpp
src/cognition/publisher.cpp
src/host/deployment_profile.cpp
src/host/generation_activation.cpp
apps/runtimectl_main.cpp         minimal read-only client: cognition show /
                                 profile show (status/doctor land MVP-3)
config/profiles/zcode-jason-context-record-mvp.yaml   the accepted instance
tests/policy_parse.cpp           parser subset + fail-closed classes
tests/bundle_publish_pin.cpp     exit gates 1/2/3/5 (real-git fixtures)
tests/profile_accept.cpp         profile acceptance fail-closed rules
tests/generation_stale.cpp       exit gate 4 (journal integration)
tests/process_runner_bounded.cpp  first-form bounds (cap, deadline, no-shell)
tests/headers/*.cpp              self-containment entries for every new header
```

Changed: `src/journal/runtime_journal.cpp` (`advance_generation` stale
extension), `CMakeLists.txt` (new sources, runtimectl app), `.qiven/operator.json`
unchanged (gate covers new tests via ctest).

Out of scope (later batches): SessionStart refresh loop + remote fetch +
freshness-window fallback policy (host startup, MVP-3 composes it; the
publisher takes an explicit ref), IPC transport (MVP-3), hook adapter
(MVP-4), K5 compression (post-MVP evidence), CI profiles.

## 3. Contracts

### 3.1 `jsonx` (D-1 first form)

```cpp
namespace qiven::runtime::jsonx {
struct JsonError { std::string detail; };
class JsonValue { /* variant: null/bool/u64/string/array/object */ };
[[nodiscard]] qiven::Result<JsonValue> parse(std::string_view bytes); // depth ≤ 8, ≤ 1 MiB
[[nodiscard]] std::string write(const JsonValue& value);              // deterministic
}
```

Strict UTF-8; unknown escapes rejected; duplicate object keys rejected;
numbers are u64 only (manifest carries none larger). Writer emits fixed
field order given by construction (the manifest builder controls order).

### 3.2 `processx::ProcessRunner` (first form)

```cpp
struct ProcessSpec {
    std::filesystem::path executable;      // absolute, explicit — never a shell string
    std::vector<std::string> argv;         // argv[0] = executable name by caller
    std::filesystem::path working_dir;     // absolute
    u64 deadline_ms = 30'000;              // hard ceiling; 0 = default 30 s
    u64 output_cap_bytes = 8 * 1024 * 1024; // per stream; overflow = typed failure
};
struct ProcessRun {
    enum class End { Exited, TimedOut, SpawnFailed };
    End end = End::Exited;
    i32 exit_code = 0;
    std::string out; std::string err;      // capped captures
};
class ProcessRunner {
    [[nodiscard]] qiven::Result<ProcessRun> run(const ProcessSpec& spec) const;
};
```

Error codes 70–74 (DESIGN §5): 71 spawn, 72 deadline (end=TimedOut — the
run RESULT carries it; 72 is returned when the caller needs a typed
failure rather than a classified result: git plumbing maps TimedOut to a
71-class `SourceUnverifiable`), 73 output-limit. `CreateProcessW` with
`CREATE_NO_WINDOW` (MEM-20260923T212000Z-D4E5F6), no inherited stdin
(closed pipe), reader threads append into capped buffers and stop at cap
(73 on overflow). MVP-3 hardening list recorded in §8.

### 3.3 `cognition::policy` — strict parser (D-4)

Parses exactly the qiven-context machine instance (see that repository's
`runtime/invocation-policy.yaml`, schema `qiven-invocation-policy-v1`):
top-level `schema`, `version`, `policy.present`, `policy.rules[]`
(`action`, optional `claim`, `requirement`, `boundary`, `subject`,
`blocking`), `resolvers[]` (`requirement`, `type`, `min_version`),
`freshness.evidence_ttl_ms` / `freshness.bundle_freshness_ms`,
`enforcement.unsatisfied_before_judgment` ∈ {deny, redeliberate},
`enforcement.unsatisfied_before_execution` ∈ {deny}. Unknown key, unknown
enum value, wrong scalar type, missing required field, nesting depth > 3,
input > 256 KiB, > 512 rules → typed parse failure (mapped to
`BundleError{MalformedManifest}`-class at the publisher boundary). Enum
vocabularies are the frozen v4 `ActionKind`/`ClaimClass`/`RequirementKind`/
`RequirementBoundary` names — spelled as in the draft, compared as
strings, converted to draft enum values via explicit tables.

```cpp
struct ParsedPolicy {
    qiven::context::InvocationPolicy invocation; // present=true, rules
    struct Resolver { qiven::context::RequirementKind kind; std::string type; u64 min_version; };
    std::vector<Resolver> resolvers;
    u64 evidence_ttl_ms = 0; u64 bundle_freshness_ms = 0;
    bool redeliberate_on_before_judgment = true;   // §8.2 default law
    bool deny_on_before_execution = true;
};
[[nodiscard]] qiven::Result<ParsedPolicy> parse_invocation_policy(std::string_view bytes);
```

### 3.4 `cognition::BundleStore` + publisher

```cpp
struct PublishRequest {
    std::filesystem::path repo_root;   // qiven-context checkout (plumbing -C)
    std::string ref;                   // exact oid or refspec; NEVER the working tree
    std::filesystem::path runtime_root;// <repo>/.qiven/runtime (GR-5)
    std::string publisher_build;       // build identity stamped in the manifest
    u64 now_ms;
    // source selection from the accepted profile:
    std::vector<std::string> source_paths; // git-relative files/directories
    std::string policy_path;               // e.g. "runtime/invocation-policy.yaml"
    std::string repository_url;            // stamped verbatim
    const processx::ProcessRunner* runner; // injected; git executable from profile
    std::filesystem::path git_executable;
};
struct PublishResult { std::filesystem::path bundle_dir; ContentDigest bundle_digest;
                       port::CognitionBundleManifest manifest; };

class CanonicalCognitionPublisher {
    [[nodiscard]] qiven::Result<PublishResult> publish(const PublishRequest&) const;
};
```

Publish steps (each fail-closed, ARCH §7.1/§7.2): `rev-parse <ref>^{commit}`
→ commit oid; `rev-parse <oid>^{tree}` → tree oid; `ls-tree -r --name-only
<tree>` filtered to the declared source paths + policy path (directory
paths expand to their files; count ≤ 256, per-file ≤ 2 MiB via `cat-file -s`,
total ≤ 64 MiB); `cat-file blob` per file (exact bytes); parse policy;
construct the v8 `Snapshot` = policy-only (invocation present + rules;
every record vector EMPTY — exit gate 5); serialize via
`qiven::context::serialize_snapshot`; write `<runtime_root>/bundles/.staging-<pid>/`
(manifest.json + snapshot.qvs + policy.yaml + source/…) with per-file
SHA-256 into the manifest; fsync-less single-filesystem rename staging →
`bundles/<bundle-digest-hex>/`; rewrite `bundles/ACTIVE` (temp + rename)
to the digest line. `bundle_digest` = SHA-256 over the written
manifest.json bytes. Publisher NEVER reads the working tree, never mutates
the checkout, never touches an existing bundle dir (immutable; re-publish
of identical manifest bytes is idempotent-rename-safe, different bytes get
a different digest dir).

```cpp
class BundleStore {
    explicit BundleStore(std::filesystem::path runtime_root);
    // Load + fully reverify (exit gate 2): ACTIVE → dir → manifest parse →
    // EVERY manifest-named file re-hashed → snapshot/policy digests match.
    [[nodiscard]] qiven::Result<VerifiedBundle, port::BundleError> load_active() const;
    [[nodiscard]] qiven::Result<VerifiedBundle, port::BundleError> load(const ContentDigest&) const;
    // ICognitionBundlePort impl: verifies ACTIVE equals the passed manifest
    // field-wise, then pins snapshot bytes through DraftSnapshotReader with
    // the digest as precondition; PinnedCognition.source_revision = manifest
    // source_revision (exit gate 3's separation).
};
```

### 3.5 `host::FileProfileSource` (IProfilePort)

Parses `config/profiles/<id>.yaml` — schema `qiven-deployment-profile-v1`
with the SAME bounded line-parser discipline (D-4): `schema`, `profile_id`,
`revision`, `control_version`, `resolver_registry_revision`,
`classifier_contract_revision`, `conformance_evidence` (non-empty —
`EvidenceMissing` otherwise), `freshness_window_ms`, `governed_paths[]`
(non-empty, explicit, glob-free — `ScopeAmbiguous` otherwise; handed to
`ResourceScope::Builder` whose normalization failures also deny),
`actors[]`, `capabilities[]`, `mediation[]`, `claims`, `cognition`
(repository, authorized_ref, policy_path, policy_sha256 — the hard-gate
digest, source_paths[]), `tools.git.executable` + `min_version`.
Validation beyond parsing: every mediation entry references a declared
capability; actor bindings unique (ProfileBuilder enforces the rest).
Construction goes through `ProfileBuilder::build()` — profiles are
loaded, never synthesized; `expected_revision` mismatch → typed
`VersionMismatch`. The in-code `ProfileBuilder` remains test-only usage.

### 3.6 Generation activation + stale rules (exit gate 4)

`RuntimeJournal::advance_generation` gains, inside its existing
transaction (after the insert): `UPDATE decisions SET state='stale'
WHERE state='bound' AND generation != <new id>` and appends the marked
count to the `generation_advanced` audit payload (append-only field
extension; stored-bytes verification unaffected). Semantics: creating a
new generation retires the prior active one and invalidates every
UNCONSUMED decision bound under any older generation. Consumed decisions
are untouched (their effects already happened; ARCH §7.4: an
already-dispatched action stays attached to its original generation).

`host::activate_generation(journal, bundle_digest, profile_digest,
build_id, now_ms)`: reads `MAX(id)` from `generations` (new journal read
`max_generation_id()`), mints `id = max+1`, calls `advance_generation`,
returns the id. Callers (RuntimeHost, MVP-3) own WHEN activation happens;
this batch's tests drive it directly. Boot-epoch stale-marking (MVP-1
recovery) is unchanged and composes: either mechanism alone leaves no
consumable old token.

### 3.7 `qiven-runtimectl` (minimal, read-only)

`runtimectl cognition show [--root <qiven-context checkout>]` → loads
ACTIVE via BundleStore, prints source_revision, source_tree,
snapshot/policy digests, source-file count, publisher_build, published_at;
exit 1 with a typed message when no ACTIVE or verification fails.
`runtimectl profile show [--profile <path>]` → accepts the profile and
prints identity, revision, governed paths, policy digest, mediation
summary. `status`/`doctor` are MVP-3 (IPC); absent here. App target
`qiven-runtimectl` (+ alias `qiven::runtimectl_app`), FOLDER "Apps".

## 4. Concurrency and lifecycle

All MVP-2 components are control-thread, single-call libraries: no
background threads except the runner's transient per-stream reader
threads (joined before `run` returns — no detached state escapes).
`BundleStore` and the publisher are stateless besides their configured
roots; `ProcessRunner` is const-callable. Journal interaction follows
GR-4 (single control thread; MVP-2 does not add writers). The ACTIVE
pointer swap is rename-atomic; readers that opened the old bundle dir
keep a consistent view (dirs are immutable). No startup order is imposed
here — RuntimeHost (MVP-3) sequences recovery → bundle load → activation.

## 5. Failure modes (fail-closed)

| Failure class | Behavior |
| --- | --- |
| git spawn failure / non-zero / timeout / output cap | typed `SourceUnverifiable` (publisher); bundle never written |
| ref does not resolve / ambiguous ref | typed `SourceUnverifiable` |
| policy file absent at the commit / parse failure / unknown enum | typed `MalformedManifest`-class failure; publish aborts |
| source set exceeds count/size/total caps | typed failure; publish aborts |
| bundle file modified after publish (any member incl. manifest) | `load`/`load_active` → `DigestMismatch` (exit gate 2) |
| ACTIVE missing / dangling / digest-dir absent | `Unavailable`; no fallback to the working tree (ARCH §7.4) |
| profile file malformed / scope empty or globbed / evidence absent / revision mismatch | typed `Malformed`/`ScopeAmbiguous`/`EvidenceMissing`/`VersionMismatch`; no profile, no generation |
| advance_generation races a bound decision | same-transaction stale-mark; the decision can no longer be consumed (53) |
| staging dir left by a crash | next publish sweeps same-prefix `.staging-*` under bundles/ (regenerable class) |

## 6. Test spine (each row names its exit-gate proof)

| Test | Proves (ARCH §15 MVP-2) |
| --- | --- |
| `policy_parse` | parser subset: happy path mirrors the 10 default rows; unknown key/enum/type, oversize, depth → typed failure (D-4 fail-closed) |
| `bundle_publish_pin` (real git fixtures under `.generated-temp/runtime/tests/`) | **gate 1**: publish pins from the explicit ref while the working tree is dirty (modified tracked + new untracked under a source dir) — bundle bytes prove unaffected. **gate 2**: flip a byte in source/snapshot/policy/manifest → load fails `DigestMismatch`; tampered ACTIVE too. **gate 3**: manifest + `PinnedCognition` carry commit oid AND content digests as distinct fields; two commits with identical trees → same digests, different source_revision. **gate 5**: a rich record (ADR-shaped markdown) rides `source/` byte-identical to the git blob; the deserialized snapshot has EMPTY record vectors and present policy |
| `profile_accept` | acceptance fail-closed: empty/globbed scope → `ScopeAmbiguous`; missing evidence → `EvidenceMissing`; revision mismatch → `VersionMismatch`; unknown capability reference → deny; happy path round-trips the shipped instance |
| `generation_stale` | **gate 4**: bind decision under g1 (unconsumed) → activate g2 (bundle digest change) → decision `stale`, consume denied 53; a CONSUMED decision stays consumed; same for a profile-digest change; boot recovery does not resurrect |
| `process_runner_bounded` | first-form bounds: capped output (73), deadline kill (TimedOut + partial output), echo argv no-shell (metacharacters literal), missing executable (71) |

Real-git fixtures: tests create throwaway repos under
`QIVEN_RUNTIME_TEST_WORKROOT` (`git init`, config `user.name/email`
fixed, real commits) — the publisher's git surface is the thing being
proven, so no fake runner on the happy path. The runner's own bounds
test uses `cmd.exe /d /c` echoes and `ping`-class sleepers, explicitly
allowlisted by the TEST (executable allowlisting proper is MVP-3).

## 7. Dependencies and deployment

No new third-party slots (TP-1 SQLite only; jsonx/processx/policy are
first-party by D-1/D-4/DESIGN §11). Git is an EXTERNAL tool invoked as an
explicit executable recorded in the profile (absolute path + min version;
digest pinning lands with MVP-3's allowlist). Deployment bundle gains
`bin/qiven-runtimectl.exe` via the existing deploy task metadata; no
license changes. qiven-context gains `runtime/invocation-policy.yaml`
(+ validator reference-check) in the same batch — the runtime tests parse
a FIXTURE copy pinned byte-wise to the canonical file's schema, keeping
the repositories independently buildable.

## 8. Deferrals (falsifiable, constitution §15)

| # | Deferral | Failure signal / revisit trigger |
| --- | --- | --- |
| C-1 | `cat-file --batch` streaming (vs per-file) | a publish measurably slow at > 256 files or > 64 MiB (profile caps raised) |
| C-2 | Job Object kill-on-close + env/executable allowlist (processx hardening) | MVP-3 batch (named work); until then the runner is git-plumbing-only, not a general executor |
| C-3 | SessionStart refresh + bounded fetch + freshness-window fallback | MVP-3 host startup composes it; publisher API already takes explicit refs |
| C-4 | resolver min-version ENFORCEMENT (parsed, recorded in manifest context; registry enforcement is the resolver-registry batch) | first real resolver version mismatch |
| C-5 | `memory/index.yaml`-derived snapshot enrichment (records into v8 vectors) | K5 compressed delivery decision — exit gate 5 explicitly forbids lossy conversion NOW |

## 9. Review record

Self-review 2026-09-23 (v19 session, designation
`jason-extended-cognition`), performed against the design ALONE, before
any implementation code existed in the branch:

1. Checked every exit-gate row 1–5 against a named test (§6) — all five
   covered, gates 1/2/3/5 by real-git fixtures, not fakes.
2. Found and resolved the MVP-2/MVP-3 ordering knot: ARCH assigns the
   process runner to MVP-3 but exact-Git reading to MVP-2 — resolved by
   the §1.1 first-form/hardening split, recorded rather than silent.
3. Verified `advance_generation` extension is append-only-safe for the
   audit chain (verification rehashes stored bytes; the payload encoder
   appends one field; re-encoding old events is never attempted).
4. Checked staleness composition: generation-stale (this batch) +
   boot-epoch-stale (MVP-1) cannot leave a consumable old token; consumed
   decisions are never regressed (exit gate 4 vs MVP-1 gate 2 tension
   resolved in §3.6).
5. Considered bundle determinism: `published_at` makes manifest bytes
   (hence bundle dir identity) publish-unique; CONTENT digests inside are
   commit-deterministic. Documented so "reproducible derivative" is read
   as content-reproducible, not byte-identical manifests.
6. Verified no second domain truth: bundle is a derivative under
   `.qiven/runtime/` (gitignored, regenerable); journal gains no domain
   rows; qiven-context files remain the only domain authority.
7. Checked profile honesty: day-one mediation inventory marks the typed
   record path ActionInterception and the raw ZCode tool classes
   Unmediated (complete mediation is MVP-4's H1-proven claim); the
   shipped profile's actor binding is the accepted loopback binding —
   the file records it, revision bumps when MVP-4 lands the handshake.
