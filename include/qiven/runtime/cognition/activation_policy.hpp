#pragma once

// ============================================================================
// cognition/activation_policy.hpp — CA-1 policy loaders (design §3;
// roadmap §4 item 3)
//
// Bounded fail-closed parsers for the two canonical instances under
// qiven-context runtime/cognition/:
//   cognition-core.yaml               (corpus, capabilities, budgets,
//                                      registry, consumer profiles,
//                                      latency objectives)
//   cognition-activation-policy.yaml  (typed rule table)
// D-4 family law: STRICT BLOCK STYLE subset — exact keys, fixed nesting,
// no flow syntax, no anchors, no comments-in-values beyond '#' line
// comments; ≤256 KiB input; fail-closed with line-typed errors; unknown
// selector vocabulary is `unresolved` and FAILS VISIBLY (schema v1 law).
// ============================================================================

#include <qiven/result.hpp>
#include <qiven/types.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace qiven::runtime::cognition
{
inline constexpr usize max_activation_policy_bytes = 256 * 1024;
inline constexpr usize max_activation_policy_rules = 512;

enum class ActivationPolicyError : u8
{
    Malformed         = 1, // structure/key violation at a line
    UnknownVocabulary = 2, // enum value outside schema v1
    BoundExceeded     = 3,
    MissingField      = 4,
};

struct ActivationPolicyParseError
{
    ActivationPolicyError kind = ActivationPolicyError::Malformed;
    usize line                 = 0;
    std::string detail;
};

enum class PriorityClass : u8
{
    P0Core       = 0,
    P1Protected  = 1,
    P2Required   = 2,
    P3Supporting = 3,
    P4OnDemand   = 4,
};

enum class RuleLifecycle : u8
{
    Active     = 0,
    Superseded = 1,
    Legacy     = 2,
    Retired    = 3,
};

struct RuleSource
{
    std::string repository;
    std::string path;
    std::string anchor;
};

struct RuleSelectors
{
    std::vector<std::string> phases; // schema v1: specify|design|implementation|review|acceptance|all
    std::vector<std::string> risk;   // R0..R3|all
    std::vector<std::string> repositories;
    std::vector<std::string> path_prefixes;  // optional
    std::vector<std::string> languages;      // cpp|python|batch|cmake|markdown|yaml
    std::vector<std::string> boundary_kinds; // schema v1 vocabulary
    std::vector<std::string> explicit_ids;   // optional triggers
};

struct ActivationRule
{
    std::string rule_id;
    PriorityClass priority_class = PriorityClass::P3Supporting;
    RuleLifecycle lifecycle      = RuleLifecycle::Active;
    RuleSource source;
    RuleSelectors selectors;
    std::vector<std::string> expected_controls;
    std::vector<std::string> independent_evidence; // mechanical|environmental|fresh-cognitive|owner-boundary
};

struct CorpusRepository
{
    std::string repository;
    std::vector<std::string> path_filters;
};

struct CapabilitySurface
{
    std::string repository;
    std::string path;
};

struct CognitionCore
{
    u32 schema_version = 0;
    std::vector<CorpusRepository> corpus;
    std::vector<CapabilitySurface> capabilities;
    u64 compact_core_max_bytes       = 0;
    u64 task_payload_max_bytes       = 0;
    u64 inlined_body_max_bytes       = 0;
    u64 supporting_share_max_percent = 0;
    u32 selector_schema_version      = 0;
    std::string activation_policy_path;
    std::string task_schema_repository;
    std::string task_schema_path;
    std::string fixtures_repository;
    std::string fixtures_path;
    std::vector<std::string> consumer_profiles;
    u64 cold_rebuild_max_ms        = 0;
    u64 warm_activation_p95_max_ms = 0;
};

struct ActivationPolicy
{
    u32 schema_version = 0;
    std::vector<ActivationRule> rules;
};

struct ActivationPolicyOutcome
{
    CognitionCore core;
    ActivationPolicy policy;
};

// Parses cognition-core.yaml bytes (fail-closed; every error names its
// line and kind).
[[nodiscard]] qiven::Result<CognitionCore, ActivationPolicyParseError> parse_cognition_core(
    std::string_view bytes);

// Parses cognition-activation-policy.yaml bytes. Every enum value is
// validated against the schema-v1 EXACT vocabulary; unknown values are
// UnknownVocabulary failures (never silently irrelevant).
[[nodiscard]] qiven::Result<ActivationPolicy, ActivationPolicyParseError> parse_activation_policy(
    std::string_view bytes);

[[nodiscard]] std::optional<PriorityClass> parse_priority_class(std::string_view name) noexcept;
[[nodiscard]] std::optional<RuleLifecycle> parse_lifecycle(std::string_view name) noexcept;
[[nodiscard]] bool is_schema_v1_phase(std::string_view value) noexcept;
[[nodiscard]] bool is_schema_v1_risk(std::string_view value) noexcept;
[[nodiscard]] bool is_schema_v1_language(std::string_view value) noexcept;
[[nodiscard]] bool is_schema_v1_boundary_kind(std::string_view value) noexcept;
[[nodiscard]] bool is_schema_v1_evidence(std::string_view value) noexcept;
} // namespace qiven::runtime::cognition
