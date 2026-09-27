// ============================================================================
// activation_policy — CA-1 PR-1 loader gates (design §3; fail-closed
// bounded parsers; unknown vocabulary fails visibly)
// ============================================================================

#include <qiven/runtime/cognition/activation_policy.hpp>

#include <cstdio>
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

    std::printf("ACTIVATION-POLICY PASS%s", eol.c_str());
    return 0;
}
