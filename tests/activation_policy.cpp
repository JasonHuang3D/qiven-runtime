// ============================================================================
// activation_policy — CA-1 PR-1 loader gates (design §3; fail-closed
// bounded parsers; unknown vocabulary fails visibly)
// ============================================================================

#include <qiven/runtime/cognition/activation_policy.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace
{
using qiven::runtime::cognition::ActivationPolicyError;
using qiven::runtime::cognition::parse_activation_policy;
using qiven::runtime::cognition::parse_cognition_core;

constexpr std::string_view valid_core = R"(# leading comment
schema_version: 1
corpus:
  repositories:
    - repository: qiven-context
      path_filters:
        - memory/records
        - decisions
capabilities:
  - repository: qiven-foundation
    path: docs/architecture/capability-surface.yaml
budgets:
  compact_core_max_bytes: 12288
  task_payload_max_bytes: 65536
  inlined_body_max_bytes: 8192
  supporting_share_max_percent: 25
registry:
  selector_schema_version: 1
  activation_policy_path: runtime/cognition/cognition-activation-policy.yaml
  task_schema_repository: qiven-devkit
  task_schema_path: docs/schemas/engineering-task-v1.schema.json
  fixtures_repository: qiven-context
  fixtures_path: evidence/cognition/fixtures
consumer_profiles:
  - zcode-jason
latency_objectives:
  cold_rebuild_max_ms: 5000
  warm_activation_p95_max_ms: 250
)";

constexpr std::string_view valid_policy = R"(schema_version: 1
rules:
  - rule_id: RULE-A
    priority_class: P1-protected
    lifecycle: active
    source:
      repository: qiven-context
      path: memory/records/x.md
      anchor: body
    selectors:
      phases:
        - design
        - implementation
      risk:
        - all
      boundary_kinds:
        - ipc
        - serialization
    expected_controls:
      - a control statement
    independent_evidence:
      - mechanical
    explicit_ids:
      - MEM-X
  - rule_id: RULE-B
    priority_class: P4-on-demand
    lifecycle: superseded
    source:
      repository: qiven-docs
      path: accepted
      anchor: body
    selectors:
      explicit_ids:
        - ADR-0050
    expected_controls:
      - deliberation record
    independent_evidence:
      - fresh-cognitive
)";
} // namespace

int main()
{
    const std::string eol(1, char(10));

    // ---- valid core parses with every field ----
    auto core = parse_cognition_core(valid_core);
    if (!core.is_ok())
    {
        std::printf("[FAIL] valid core rejected at line %zu: %s%s", core.reason().line,
                    core.reason().detail.c_str(), eol.c_str());
        return 1;
    }
    if (core.value().corpus.size() != 1 || core.value().corpus[0].repository != "qiven-context" ||
        core.value().corpus[0].path_filters.size() != 2 ||
        core.value().corpus[0].path_filters[1] != "decisions" ||
        core.value().capabilities.size() != 1 ||
        core.value().capabilities[0].path != "docs/architecture/capability-surface.yaml" ||
        core.value().compact_core_max_bytes != 12288 ||
        core.value().activation_policy_path != "runtime/cognition/cognition-activation-policy.yaml" ||
        core.value().consumer_profiles != std::vector<std::string> { "zcode-jason" } ||
        core.value().cold_rebuild_max_ms != 5000)
    {
        std::printf("[FAIL] core fields wrong%s", eol.c_str());
        return 2;
    }
    std::printf("[ OK ] valid core parses with every field%s", eol.c_str());

    // ---- core fail-closed classes ----
    auto wrong_version = parse_cognition_core("schema_version: 2\n");
    if (wrong_version.is_ok() ||
        wrong_version.reason().kind != ActivationPolicyError::UnknownVocabulary)
    {
        std::printf("[FAIL] wrong schema_version not typed-rejected%s", eol.c_str());
        return 3;
    }
    auto missing_field = parse_cognition_core("schema_version: 1\ncorpus:\n  repositories:\n");
    if (missing_field.is_ok() || missing_field.reason().kind != ActivationPolicyError::MissingField)
    {
        std::printf("[FAIL] incomplete core not typed-rejected%s", eol.c_str());
        return 4;
    }
    std::printf("[ OK ] core fail-closed classes typed%s", eol.c_str());

    // ---- valid policy parses; vocabulary validated ----
    auto policy = parse_activation_policy(valid_policy);
    if (!policy.is_ok())
    {
        std::printf("[FAIL] valid policy rejected at line %zu: %s%s", policy.reason().line,
                    policy.reason().detail.c_str(), eol.c_str());
        return 5;
    }
    const auto& rules = policy.value().rules;
    if (rules.size() != 2 || rules[0].rule_id != "RULE-A" ||
        rules[0].priority_class != qiven::runtime::cognition::PriorityClass::P1Protected ||
        rules[0].lifecycle != qiven::runtime::cognition::RuleLifecycle::Active ||
        rules[0].selectors.phases != std::vector<std::string> { "design", "implementation" } ||
        rules[0].selectors.boundary_kinds != std::vector<std::string> { "ipc", "serialization" } ||
        rules[0].selectors.explicit_ids != std::vector<std::string> { "MEM-X" } ||
        rules[1].lifecycle != qiven::runtime::cognition::RuleLifecycle::Superseded)
    {
        std::printf("[FAIL] policy fields wrong%s", eol.c_str());
        return 6;
    }
    std::printf("[ OK ] valid policy parses with vocabulary validated%s", eol.c_str());

    // ---- unknown vocabulary fails VISIBLY (schema v1 law) ----
    const std::string bad_phase = R"(schema_version: 1
rules:
  - rule_id: R
    priority_class: P1-protected
    lifecycle: active
    source:
      repository: qiven-context
      path: p
      anchor: body
    selectors:
      phases:
        - deployment
    expected_controls:
      - c
    independent_evidence:
      - mechanical
)";
    auto bad                    = parse_activation_policy(bad_phase);
    if (bad.is_ok() || bad.reason().kind != ActivationPolicyError::UnknownVocabulary)
    {
        std::printf("[FAIL] unknown phase not typed-rejected%s", eol.c_str());
        return 7;
    }
    const std::string bad_priority = std::string(R"(schema_version: 1
rules:
  - rule_id: R
    priority_class: P9-magic
)");
    auto bad2                      = parse_activation_policy(bad_priority);
    if (bad2.is_ok() || bad2.reason().kind != ActivationPolicyError::UnknownVocabulary)
    {
        std::printf("[FAIL] unknown priority not typed-rejected%s", eol.c_str());
        return 8;
    }
    std::printf("[ OK ] unknown vocabulary fails visibly%s", eol.c_str());

    // ---- flow syntax on a selector list header: typed rejection ----
    // (a discarded inline value would silently turn the dimension into an
    // undeclared wildcard - the strict-block-style law)
    const std::string flow_list = std::string(R"(schema_version: 1
rules:
  - rule_id: R-FLOW
    priority_class: P1-protected
    lifecycle: active
    source:
      repository: qiven-context
      path: memory/records/x.md
      anchor: body
    selectors:
      phases: [design]
    expected_controls:
      - control
    independent_evidence:
      - mechanical
)");
    auto flow                   = parse_activation_policy(flow_list);
    if (flow.is_ok() || flow.reason().kind != ActivationPolicyError::Malformed)
    {
        std::printf("[FAIL] flow-style selector list not typed-rejected%s", eol.c_str());
        return 20;
    }
    std::printf("[ OK ] flow-style selector list typed-rejected%s", eol.c_str());

    // ---- rule with no source anchor: typed rejection ----
    const std::string no_anchor = std::string(R"(schema_version: 1
rules:
  - rule_id: R-NOANCHOR
    priority_class: P1-protected
    lifecycle: active
    source:
      repository: qiven-context
      path: memory/records/x.md
    expected_controls:
      - control
    independent_evidence:
      - mechanical
)");
    auto anchorless             = parse_activation_policy(no_anchor);
    if (anchorless.is_ok() || anchorless.reason().kind != ActivationPolicyError::MissingField)
    {
        std::printf("[FAIL] rule without anchor accepted%s", eol.c_str());
        return 21;
    }
    std::printf("[ OK ] rule without anchor typed-rejected%s", eol.c_str());

    // ---- rule with no expected_controls: typed rejection ----
    const std::string no_controls = std::string(R"(schema_version: 1
rules:
  - rule_id: R-NOCONTROLS
    priority_class: P1-protected
    lifecycle: active
    source:
      repository: qiven-context
      path: memory/records/x.md
      anchor: body
    independent_evidence:
      - mechanical
)");
    auto controlless              = parse_activation_policy(no_controls);
    if (controlless.is_ok() || controlless.reason().kind != ActivationPolicyError::MissingField)
    {
        std::printf("[FAIL] rule without expected_controls accepted%s", eol.c_str());
        return 22;
    }
    std::printf("[ OK ] rule without expected_controls typed-rejected%s", eol.c_str());

    // ---- lowercase rule_id charset: typed rejection ----
    const std::string lower_id = std::string(R"(schema_version: 1
rules:
  - rule_id: r-lowercase
    priority_class: P1-protected
    lifecycle: active
    source:
      repository: qiven-context
      path: memory/records/x.md
      anchor: body
    expected_controls:
      - control
    independent_evidence:
      - mechanical
)");
    auto lowered               = parse_activation_policy(lower_id);
    if (lowered.is_ok() || lowered.reason().kind != ActivationPolicyError::Malformed)
    {
        std::printf("[FAIL] lowercase rule_id accepted%s", eol.c_str());
        return 23;
    }
    std::printf("[ OK ] rule_id charset enforced%s", eol.c_str());

    // ---- core missing a registry key: typed rejection ----
    const std::string no_registry = std::string(R"(schema_version: 1
corpus:
  repositories:
    - repository: qiven-context
      path_filters:
        - memory/records
budgets:
  compact_core_max_bytes: 12288
  task_payload_max_bytes: 65536
  inlined_body_max_bytes: 8192
  supporting_share_max_percent: 25
registry:
  activation_policy_path: runtime/cognition/cognition-activation-policy.yaml
consumer_profiles:
  - zcode-jason
latency_objectives:
  cold_rebuild_max_ms: 5000
  warm_activation_p95_max_ms: 250
)");
    auto registryless             = parse_cognition_core(no_registry);
    if (registryless.is_ok() || registryless.reason().kind != ActivationPolicyError::MissingField)
    {
        std::printf("[FAIL] core without registry keys accepted%s", eol.c_str());
        return 24;
    }
    std::printf("[ OK ] core registry keys required%s", eol.c_str());

    // ---- incomplete capability entry: typed rejection (no silent drop) ----
    const std::string partial_capability = std::string(R"(schema_version: 1
corpus:
  repositories:
    - repository: qiven-context
      path_filters:
        - memory/records
capabilities:
  - repository: qiven-foundation
budgets:
  compact_core_max_bytes: 12288
  task_payload_max_bytes: 65536
  inlined_body_max_bytes: 8192
  supporting_share_max_percent: 25
registry:
  selector_schema_version: 1
  activation_policy_path: runtime/cognition/cognition-activation-policy.yaml
  task_schema_repository: qiven-devkit
  task_schema_path: docs/schemas/engineering-task-v1.schema.json
  fixtures_repository: qiven-context
  fixtures_path: evidence/cognition/fixtures
consumer_profiles:
  - zcode-jason
latency_objectives:
  cold_rebuild_max_ms: 5000
  warm_activation_p95_max_ms: 250
)");
    auto capabilityless                  = parse_cognition_core(partial_capability);
    if (capabilityless.is_ok() ||
        capabilityless.reason().kind != ActivationPolicyError::MissingField)
    {
        std::printf("[FAIL] incomplete capability entry silently dropped%s", eol.c_str());
        return 25;
    }
    std::printf("[ OK ] incomplete capability entry typed-rejected%s", eol.c_str());

    // ---- the PUBLISHED instances parse (roadmap work item: validate the
    // canonical cognition-core.yaml and cognition-activation-policy.yaml;
    // digest-bound copies under tests/fixtures/cognition/policy from
    // qiven-context main where they are canonical) ----
    {
        const auto fixtures_root =
            std::filesystem::path(QIVEN_RUNTIME_COGNITION_POLICY_FIXTURES);
        const auto core_path   = fixtures_root / "cognition-core.yaml";
        const auto policy_path = fixtures_root / "cognition-activation-policy.yaml";
        if (!std::filesystem::exists(core_path) || !std::filesystem::exists(policy_path))
        {
            std::printf("[FAIL] published-instance fixtures missing under %s%s",
                        fixtures_root.string().c_str(), eol.c_str());
            return 14;
        }
        std::ifstream core_in(core_path, std::ios::binary);
        std::string core_text((std::istreambuf_iterator<char>(core_in)),
                              std::istreambuf_iterator<char>());
        std::ifstream policy_in(policy_path, std::ios::binary);
        std::string policy_text((std::istreambuf_iterator<char>(policy_in)),
                                std::istreambuf_iterator<char>());

        auto real_core = parse_cognition_core(core_text);
        if (!real_core.is_ok())
        {
            std::printf("[FAIL] published cognition-core.yaml rejected at line %zu: %s%s",
                        real_core.reason().line, real_core.reason().detail.c_str(),
                        eol.c_str());
            return 16;
        }
        if (real_core.value().corpus.size() != 4 ||
            real_core.value().corpus[0].repository != "qiven-context" ||
            real_core.value().corpus[0].path_filters.size() != 6 ||
            real_core.value().compact_core_max_bytes != 12288 ||
            real_core.value().consumer_profiles != std::vector<std::string> { "zcode-jason" })
        {
            std::printf("[FAIL] published core fields wrong%s", eol.c_str());
            return 17;
        }
        auto real_policy = parse_activation_policy(policy_text);
        if (!real_policy.is_ok())
        {
            std::printf(
                "[FAIL] published cognition-activation-policy.yaml rejected at line %zu: %s%s",
                real_policy.reason().line, real_policy.reason().detail.c_str(), eol.c_str());
            return 18;
        }
        // the bootstrap set after the F-05/F-07 scar additions: 19 rules
        // (4 TCA docs + 6 scars + 4 laws + 2 supporting + 1 P4 + TCA-GOV scar-lifecycle)
        if (real_policy.value().rules.size() != 19)
        {
            std::printf("[FAIL] published policy rule count %zu (expected 19)%s",
                        real_policy.value().rules.size(), eol.c_str());
            return 19;
        }
        std::printf("[ OK ] published instances parse field-exact (core corpus 4 repos;"
                    " policy 19 rules)%s",
                    eol.c_str());
    }

    std::printf("ACTIVATION-POLICY PASS%s", eol.c_str());
    return 0;
}
