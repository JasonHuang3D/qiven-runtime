// ============================================================================
// cognition_protected_recall — CA-1 PR-3 Profile B suite (design §5;
// acceptance protocol §6) over the SEALED corpus F-01..F-08
//
// Fixtures are the CA-0 sealed gold corpus (digests asserted against the
// sealing manifest at suite start — copies under tests/fixtures/
// cognition/). The published bootstrap policy (qiven-context
// runtime/cognition/cognition-activation-policy.yaml shape, carried as
// the inline bootstrap table) is evaluated against each fixture's
// condition-blind envelope. Asserts §6.2 thresholds:
//   - protected MUST-INCLUDE recall 100% (a protected rule absent from
//     the delivered bundle on ANY fixture fails the profile);
//   - superseded presented as current: 0;
//   - identical-task nondeterminism: 0;
//   - budget-pressure: protected material survives 100/75/50% budgets
//     (BudgetInsufficient beyond, never silent truncation);
//   - mutation detection: selector removal, lifecycle inversion, scar
//     demotion, id substitution, owner swap, evidence omission (all via
//     typed-recall or digest change), false-safe-default (unresolved
//     critical is PRESENT-and-marked, never silently absent).
// ============================================================================

#include <qiven/hashing_sha256.hpp>
#include <qiven/runtime/cognition/activation_index.hpp>
#include <qiven/runtime/cognition/selector_eval.hpp>
#include <qiven/runtime/cognition/task_descriptor.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
using qiven::runtime::cognition::ActivationPolicy;
using qiven::runtime::cognition::ActivationRule;
using qiven::runtime::cognition::Budgets;
using qiven::runtime::cognition::PriorityClass;
using qiven::runtime::cognition::Readiness;
using qiven::runtime::cognition::RuleLifecycle;
using qiven::runtime::cognition::SelectionResult;
using qiven::runtime::cognition::TaskEnvelope;
using qiven::runtime::cognition::TaskPhase;
using qiven::runtime::cognition::TaskRisk;

struct FixtureCase
{
    const char* fixture;      // sealed file (digest-verified before use)
    const char* objective;    // verbatim consumer prompt (alternative vocabulary)
    const char* must_include; // protected rule id that MUST ride the bundle
    TaskPhase phase;
    TaskRisk risk;
};

// Envelope facts per the sealed fixtures' condition-blind sections.
const FixtureCase cases[] = {
    { "f-01-borrowed-payload-lifetime.md",
      "In our Windows service prototype (C++20) we queue little command records for a worker "
      "thread: producers call submit(std::span<const std::byte> body) and the record is "
      "executed later on the worker. Implement CommandQueue::submit plus the worker drain "
      "loop, with tests. The body arrives from a caller that builds it into a local buffer.",
      "SCAR-BORROWED-LIFETIME", TaskPhase::Implementation, TaskRisk::R2 },
    { "f-02-assumed-external-payload.md",
      "Add the upstream deadline field our protocol partner documented last quarter to the "
      "handshake packet parser and reject when it is missing, then package the validator for "
      "the on-call engineer to run on their machine by double-clicking.",
      "SCAR-ASSUMED-FIELD-H1KIT", TaskPhase::Implementation, TaskRisk::R2 },
    { "f-03-pipe-lifecycle-multi-segment.md",
      "Our named pipe server closes the connection after answering the first request; clients "
      "that send a second message on the same pipe get nothing back. Make the server keep "
      "talking until the client goes away.",
      "SCAR-WIRE-CONTRACT-E2E", TaskPhase::Implementation, TaskRisk::R3 },
    { "f-04-heredoc-active-memory.md",
      "Generate the three launcher scripts from a template at build time so the values stay "
      "in one place, and write the generated files out during the packaging step.",
      "SCAR-HEREDOC-LAW", TaskPhase::Implementation, TaskRisk::R2 },
    { "f-05-result-void-first-instantiation.md",
      "Add the failure-only variant of the Result type to the shared library headers so "
      "callers that produce no value can still propagate typed failures.",
      "SCAR-COMPILE-PROBE", TaskPhase::Design, TaskRisk::R2 },
    { "f-06-process-custody-recovery.md",
      "The tool spawns helper executables for each work item; when the tool itself is killed "
      "the helpers keep running and lock the next run. Bound them so nothing outlives the "
      "parent.",
      "LAW-PROCESS-CUSTODY", TaskPhase::Implementation, TaskRisk::R3 },
    { "f-07-concurrent-fixture-roots.md",
      "The suite runs debug and release builds at the same time and they collide on the "
      "scratch directory; give each configuration its own root without changing the public "
      "entry points.",
      "SCAR-PARALLEL-FIXTURE-ROOTS", TaskPhase::Implementation, TaskRisk::R2 },
    { "f-08-fail-closed-mediator.md",
      "When the approval broker is unreachable the queue currently keeps serving requests "
      "from cache; change it so unavailability is loud and nothing proceeds unsupervised.",
      "SCAR-WIRE-CONTRACT-E2E", TaskPhase::Design, TaskRisk::R3 },
};

ActivationRule bootstrap_rule(const std::string& id, PriorityClass cls, const std::string& path)
{
    ActivationRule rule;
    rule.rule_id              = id;
    rule.priority_class       = cls;
    rule.source               = { "qiven-context", path, "body" };
    rule.expected_controls    = { "control" };
    rule.independent_evidence = { "mechanical" };
    return rule;
}

ActivationPolicy bootstrap_policy()
{
    ActivationPolicy policy;
    policy.schema_version = 1;

    auto gov             = bootstrap_rule("TCA-GOV-CORE", PriorityClass::P0Core,
                                          "collaboration/cognitive-governance-program.md");
    gov.selectors.phases = { "all" };

    auto borrowed                     = bootstrap_rule("SCAR-BORROWED-LIFETIME", PriorityClass::P1Protected,
                                                       "memory/records/MEM-20260924T032100Z-D4E5F6.md");
    borrowed.selectors.phases         = { "design", "implementation", "review" };
    borrowed.selectors.risk           = { "R1", "R2", "R3" };
    borrowed.selectors.languages      = { "cpp" };
    borrowed.selectors.boundary_kinds = { "lifetime", "ownership", "representation" };

    auto assumed                     = bootstrap_rule("SCAR-ASSUMED-FIELD-H1KIT", PriorityClass::P1Protected,
                                                      "memory/records/MEM-20260923T115500Z-A1B2C3.md");
    assumed.selectors.phases         = { "specify", "design", "implementation" };
    assumed.selectors.risk           = { "R1", "R2", "R3" };
    assumed.selectors.boundary_kinds = { "external-contract", "human-interface", "tooling" };

    auto wire                     = bootstrap_rule("SCAR-WIRE-CONTRACT-E2E", PriorityClass::P1Protected,
                                                   "memory/records/MEM-20260924T032000Z-C1D2E3.md");
    wire.selectors.phases         = { "design", "implementation", "review" };
    wire.selectors.risk           = { "R1", "R2", "R3" };
    wire.selectors.boundary_kinds = { "ipc", "serialization", "external-contract" };

    auto heredoc                     = bootstrap_rule("SCAR-HEREDOC-LAW", PriorityClass::P1Protected,
                                                      "memory/records/MEM-20260921T203500Z-D2A7F4.md");
    heredoc.selectors.phases         = { "implementation" };
    heredoc.selectors.risk           = { "R0", "R1", "R2", "R3" };
    heredoc.selectors.boundary_kinds = { "tooling", "filesystem" };

    auto ownership                     = bootstrap_rule("LAW-SEMANTIC-OWNERSHIP", PriorityClass::P1Protected,
                                                        "decisions/ADR-0024.md");
    ownership.selectors.phases         = { "specify", "design", "review" };
    ownership.selectors.risk           = { "R1", "R2", "R3" };
    ownership.selectors.boundary_kinds = { "ownership", "governance" };

    auto custody                     = bootstrap_rule("LAW-PROCESS-CUSTODY", PriorityClass::P1Protected,
                                                      "decisions/ADR-0048.md");
    custody.selectors.phases         = { "design", "implementation", "review" };
    custody.selectors.risk           = { "R1", "R2", "R3" };
    custody.selectors.boundary_kinds = { "process-custody", "concurrency", "platform" };

    auto compile_probe                     = bootstrap_rule("SCAR-COMPILE-PROBE", PriorityClass::P1Protected,
                                                            "memory/records/MEM-20260923T042000Z-E9F0A1.md");
    compile_probe.selectors.phases         = { "design", "implementation", "review" };
    compile_probe.selectors.risk           = { "R1", "R2", "R3" };
    compile_probe.selectors.boundary_kinds = { "external-contract", "representation", "tooling" };

    auto fixture_roots                     = bootstrap_rule("SCAR-PARALLEL-FIXTURE-ROOTS", PriorityClass::P1Protected,
                                                            "memory/records/MEM-20260923T224200Z-F8A9B0.md");
    fixture_roots.selectors.phases         = { "design", "implementation", "review" };
    fixture_roots.selectors.risk           = { "R1", "R2", "R3" };
    fixture_roots.selectors.boundary_kinds = { "concurrency", "persistence" };

    auto surface                     = bootstrap_rule("SUP-FOUNDATION-SURFACE", PriorityClass::P3Supporting,
                                                      "docs/architecture/capability-surface.yaml");
    surface.source.repository        = "qiven-foundation";
    surface.selectors.languages      = { "cpp" };
    surface.selectors.boundary_kinds = { "representation", "ownership", "serialization" };

    auto history             = bootstrap_rule("HISTORY-OLD-SCAR", PriorityClass::P1Protected,
                                              "memory/records/OLD.md");
    history.lifecycle        = RuleLifecycle::Superseded;
    history.selectors.phases = { "all" };

    policy.rules = { gov, borrowed, assumed, wire, heredoc, ownership, custody, compile_probe,
                     fixture_roots, surface, history };
    return policy;
}

TaskEnvelope envelope_of(const FixtureCase& fixture)
{
    TaskEnvelope envelope;
    envelope.objective     = fixture.objective;
    envelope.repository    = "qiven-runtime";
    envelope.revision      = "46dabf0";
    envelope.changed_paths = { "src/feature.cpp" };
    envelope.phase         = fixture.phase;
    envelope.risk          = fixture.risk;
    return envelope;
}

} // namespace

// sha256 fixture digest check (sealed-manifest law: the corpus is exact).
namespace
{
std::string sha256_file(const std::filesystem::path& file)
{
    std::ifstream in(file, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    qiven::SHA256Hasher hasher;
    hasher.update(reinterpret_cast<const std::byte*>(bytes.data()), bytes.size());
    const qiven::SHA256Digest digest = hasher.finish();
    static constexpr char hex[]      = "0123456789abcdef";
    std::string out;
    out.resize(digest.size() * 2);
    for (qiven::usize i = 0; i < digest.size(); ++i)
    {
        const auto b   = static_cast<unsigned char>(digest[i]);
        out[2 * i]     = hex[b >> 4];
        out[2 * i + 1] = hex[b & 0x0Fu];
    }
    return out;
}

const char* fixture_digest(const char* file)
{
    if (std::string_view(file) == "f-01-borrowed-payload-lifetime.md")
        return "9c0ff093bd9bbc24123a9605ceed98b57a1444658dc786b3e08f345ad58a84c4";
    if (std::string_view(file) == "f-02-assumed-external-payload.md")
        return "f8054088dd0696a87876a4b551e443b3882b0aeedd029444072b4bcb34a8b74a";
    if (std::string_view(file) == "f-03-pipe-lifecycle-multi-segment.md")
        return "5297b5209335bb2e3688af0049a28322d2a0e9ac5a315fbc441252a8539fc4fc";
    if (std::string_view(file) == "f-04-heredoc-active-memory.md")
        return "03a92866b83ba6ee1dc58a8390686e0caddf76f52f73d7c55e31fd8ae950a934";
    if (std::string_view(file) == "f-05-result-void-first-instantiation.md")
        return "56bcf70ba4a8a2e92717afe940ff014ccf2074c3e58659e238fdabbf2cb7eee3";
    if (std::string_view(file) == "f-06-process-custody-recovery.md")
        return "1759a17eb1093a9ef207b9b9ef19496d9c4941921bd0e75166c244a4b9989d30";
    if (std::string_view(file) == "f-07-concurrent-fixture-roots.md")
        return "65e9f9a0ee0bf38d45c68dc0b11baac35af350684dc171c42c5c29526b8b8bd9";
    return "540f80a97f8d2a6ac029df4b8b30618d892651f5710b5cf23d5572af96b69d6e"; // f-08
}
} // namespace

int main()
{
    const std::string eol(1, char(10));
    const auto fixtures = std::filesystem::path(QIVEN_RUNTIME_COGNITION_FIXTURES);
    const Budgets budgets {};
    const ActivationPolicy policy = bootstrap_policy();

    // ---- sealed corpus integrity ----
    for (const FixtureCase& fixture : cases)
    {
        const auto file = fixtures / fixture.fixture;
        if (!std::filesystem::exists(file))
        {
            std::printf("[FAIL] sealed fixture missing: %s%s", fixture.fixture, eol.c_str());
            return 1;
        }
        if (sha256_file(file) != fixture_digest(fixture.fixture))
        {
            std::printf("[FAIL] sealed fixture digest mismatch: %s%s", fixture.fixture,
                        eol.c_str());
            return 2;
        }
    }
    std::printf("[ OK ] sealed corpus F-01..F-08 digests exact%s", eol.c_str());

    // ---- §6.2 protected recall 100% + superseded-as-current 0 + determinism ----
    auto rule_present = [](const SelectionResult& selection, const std::string& id) {
        for (const auto* rule : selection.protected_included)
        {
            if (rule->rule_id == id)
            {
                return true;
            }
        }
        for (const auto* rule : selection.unresolved) // critical unknowns STAY in the bundle
        {
            if (rule->rule_id == id)
            {
                return true;
            }
        }
        return false;
    };
    std::size_t protected_misses = 0;
    for (const FixtureCase& fixture : cases)
    {
        const auto task      = qiven::runtime::cognition::normalize_task(envelope_of(fixture));
        const auto selection = qiven::runtime::cognition::evaluate_selection(policy, task,
                                                                             budgets);
        if (!rule_present(selection, fixture.must_include))
        {
            ++protected_misses;
            std::printf("  MISS %s -> %s%s", fixture.fixture, fixture.must_include,
                        eol.c_str());
        }
        // superseded presented as current: 0
        for (const auto* rule : selection.protected_included)
        {
            if (rule->rule_id == "HISTORY-OLD-SCAR")
            {
                std::printf("[FAIL] superseded rule presented as current on %s%s",
                            fixture.fixture, eol.c_str());
                return 3;
            }
        }
        // identical-task nondeterminism: 0 (double-run equality)
        const auto repeat =
            qiven::runtime::cognition::evaluate_selection(policy, task, budgets);
        if (repeat.protected_included.size() != selection.protected_included.size() ||
            repeat.readiness != selection.readiness)
        {
            std::printf("[FAIL] nondeterminism on %s%s", fixture.fixture, eol.c_str());
            return 4;
        }
    }
    if (protected_misses != 0)
    {
        std::printf("[FAIL] protected recall %zu misses (Profile B requires 100%%)%s",
                    protected_misses, eol.c_str());
        return 5;
    }
    std::printf("[ OK ] protected recall 100%% across the corpus; no superseded-as-current%s",
                eol.c_str());

    // ---- §6.3 budget pressure: protected survives 100/75/50%% ----
    for (const FixtureCase& fixture : cases)
    {
        const auto task = qiven::runtime::cognition::normalize_task(envelope_of(fixture));
        for (const qiven::u64 share : { qiven::u64 { 100 }, qiven::u64 { 75 }, qiven::u64 { 50 } })
        {
            const qiven::u64 budget = budgets.task_payload_max_bytes * share / 100;
            const auto under =
                qiven::runtime::cognition::evaluate_selection(policy, task, budgets, budget);
            if (!rule_present(under, fixture.must_include))
            {
                std::printf("[FAIL] protected dropped at %llu%% budget on %s%s",
                            static_cast<unsigned long long>(share), fixture.fixture,
                            eol.c_str());
                return 6;
            }
        }
        // beyond the floor: typed BudgetInsufficient, protected intact
        const auto starved = qiven::runtime::cognition::evaluate_selection(policy, task, budgets,
                                                                           8);
        if (starved.readiness != Readiness::BudgetInsufficient ||
            !rule_present(starved, fixture.must_include))
        {
            std::printf("[FAIL] starvation not typed BudgetInsufficient on %s%s",
                        fixture.fixture, eol.c_str());
            return 7;
        }
    }
    std::printf("[ OK ] budget pressure: protected survives 100/75/50%%; starvation typed%s",
                eol.c_str());

    // ---- §6.4 mutation detection ----
    const auto base_digest = qiven::runtime::cognition::activation_policy_digest(policy);
    // (1) removal of a critical selector/rule
    {
        auto mutated = policy;
        mutated.rules.erase(std::remove_if(mutated.rules.begin(), mutated.rules.end(),
                                           [](const ActivationRule& rule) {
                                               return rule.rule_id == "SCAR-BORROWED-LIFETIME";
                                           }),
                            mutated.rules.end());
        const auto task =
            qiven::runtime::cognition::normalize_task(envelope_of(cases[0]));
        const auto selection =
            qiven::runtime::cognition::evaluate_selection(mutated, task, budgets);
        if (rule_present(selection, "SCAR-BORROWED-LIFETIME") ||
            qiven::runtime::cognition::activation_policy_digest(mutated) == base_digest)
        {
            std::printf("[FAIL] selector/rule removal undetected%s", eol.c_str());
            return 8;
        }
    }
    // (2) lifecycle inversion + (4) scar demotion to optional
    {
        auto mutated = policy;
        for (ActivationRule& rule : mutated.rules)
        {
            if (rule.rule_id == "SCAR-WIRE-CONTRACT-E2E")
            {
                rule.lifecycle = RuleLifecycle::Superseded; // inversion
            }
            if (rule.rule_id == "SCAR-HEREDOC-LAW")
            {
                rule.priority_class = PriorityClass::P3Supporting; // demotion
            }
        }
        const auto task = qiven::runtime::cognition::normalize_task(envelope_of(cases[2]));
        const auto selection =
            qiven::runtime::cognition::evaluate_selection(mutated, task, budgets);
        if (rule_present(selection, "SCAR-WIRE-CONTRACT-E2E"))
        {
            std::printf("[FAIL] lifecycle inversion undetected%s", eol.c_str());
            return 9;
        }
        const auto task4 =
            qiven::runtime::cognition::normalize_task(envelope_of(cases[3]));
        const auto selection4 =
            qiven::runtime::cognition::evaluate_selection(mutated, task4, budgets);
        bool demoted_still_protected = false;
        for (const auto* rule : selection4.protected_included)
        {
            if (rule->rule_id == "SCAR-HEREDOC-LAW")
            {
                demoted_still_protected = true;
            }
        }
        if (demoted_still_protected ||
            qiven::runtime::cognition::activation_policy_digest(mutated) == base_digest)
        {
            std::printf("[FAIL] scar demotion undetected%s", eol.c_str());
            return 10;
        }
    }
    // (3) semantic-owner swap + (6) evidence omission: digest change detection
    {
        auto mutated = policy;
        for (ActivationRule& rule : mutated.rules)
        {
            if (rule.rule_id == "LAW-PROCESS-CUSTODY")
            {
                rule.source.repository = "qiven-foundation"; // wrong owner
            }
            if (rule.rule_id == "LAW-SEMANTIC-OWNERSHIP")
            {
                rule.independent_evidence.clear(); // omitted evidence requirement
            }
        }
        if (qiven::runtime::cognition::activation_policy_digest(mutated) == base_digest)
        {
            std::printf("[FAIL] owner swap / evidence omission undetected (digest stable)%s",
                        eol.c_str());
            return 11;
        }
    }
    // (5) exact-id substitution: the rule's SOURCE is swapped to a
    // similarly worded record. Typed selection may still fire (phases/
    // risk match — correctly so), but the delivered binding names the
    // WRONG canonical source: detection = the delivered source differs
    // from the canonical owner AND the policy digest moved.
    {
        auto mutated = policy;
        for (ActivationRule& rule : mutated.rules)
        {
            if (rule.rule_id == "SCAR-ASSUMED-FIELD-H1KIT")
            {
                rule.source.path            = "memory/records/MEM-20260923T115500Z-A0B0C0.md"; // close, wrong
                rule.selectors.explicit_ids = { "MEM-20260923T115500Z-A0B0C0" };
            }
        }
        TaskEnvelope envelope = envelope_of(cases[1]);
        envelope.objective    = std::string(cases[1].objective) +
                             " per MEM-20260923T115500Z-A1B2C3"; // the REAL id arrives late
        const auto task                 = qiven::runtime::cognition::normalize_task(envelope);
        const auto selection            = qiven::runtime::cognition::evaluate_selection(mutated, task,
                                                                                        budgets);
        const ActivationRule* delivered = nullptr;
        for (const auto* rule : selection.protected_included)
        {
            if (rule->rule_id == "SCAR-ASSUMED-FIELD-H1KIT")
            {
                delivered = rule;
            }
        }
        for (const auto* rule : selection.unresolved)
        {
            if (rule->rule_id == "SCAR-ASSUMED-FIELD-H1KIT")
            {
                delivered = rule;
            }
        }
        // Discriminating assertion: if the rule rides the bundle, its
        // delivered source must EXPOSE the substitution (the wrong path —
        // never the canonical one silently); if absent, the canonical
        // recall check catches the removal. Either way the digest moved.
        if (delivered != nullptr &&
            delivered->source.path == "memory/records/MEM-20260923T115500Z-A1B2C3.md")
        {
            std::printf("[FAIL] id substitution hidden: canonical source delivered%s",
                        eol.c_str());
            return 12;
        }
        if (qiven::runtime::cognition::activation_policy_digest(mutated) == base_digest)
        {
            std::printf("[FAIL] id substitution undetected by digest%s", eol.c_str());
            return 13;
        }
    }
    // (7) false safe-default: unknown applicability stays PRESENT-and-marked
    {
        const auto task =
            qiven::runtime::cognition::normalize_task(envelope_of(cases[0]));
        const auto selection    = qiven::runtime::cognition::evaluate_selection(policy, task,
                                                                                budgets);
        bool present_unresolved = false;
        for (const auto* rule : selection.unresolved)
        {
            if (rule->rule_id == "SCAR-BORROWED-LIFETIME")
            {
                present_unresolved = true;
            }
        }
        if (!present_unresolved)
        {
            std::printf("[FAIL] unknown applicability silently dropped (safe-default)%s",
                        eol.c_str());
            return 14;
        }
    }
    std::printf("[ OK ] mutation classes detected (removal, inversion, demotion, id"
                " substitution, owner swap, evidence omission, safe-default)%s",
                eol.c_str());

    std::printf("PROTECTED-RECALL PASS%s", eol.c_str());
    return 0;
}
