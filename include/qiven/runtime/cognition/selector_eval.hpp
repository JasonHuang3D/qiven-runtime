#pragma once

// ============================================================================
// cognition/selector_eval.hpp — CA-1 typed selection (design §1; TCA-ARCH
// §10 steps 4-10; selector-schema v1 law)
//
// Protected (P0/P1) evaluation is TYPED-ONLY over the descriptor facts:
// phases, risk, repositories, path prefixes, languages, explicit IDs.
// A rule whose selectors depend on facts the fixed normalizer cannot
// derive (boundary kinds in v1) evaluates UNRESOLVED — unknown
// applicability is never false; unresolved critical rules stay in the
// bundle and may block readiness. Superseded/legacy/retired rules are
// selectable only as P4 history. P3/P4 candidates are ranked by
// deterministic lexical signals (exact matches outrank). Protected
// material beyond the budget fails BudgetInsufficient — never silent
// truncation.
// ============================================================================

#include <qiven/runtime/cognition/activation_policy.hpp>
#include <qiven/runtime/cognition/task_descriptor.hpp>

#include <string>
#include <vector>

namespace qiven::runtime::cognition
{
struct Budgets
{
    u64 compact_core_max_bytes       = 12288;
    u64 task_payload_max_bytes       = 65536;
    u64 inlined_body_max_bytes       = 8192;
    u64 supporting_share_max_percent = 25;
};

enum class SelectorVerdict : u8
{
    Applicable    = 0,
    NotApplicable = 1,
    Unresolved    = 2,
};

enum class Readiness : u8
{
    ReadyForPhase      = 0,
    NeedsEvidence      = 1,
    ReDeliberate       = 2,
    Blocked            = 3,
    StaleGeneration    = 4,
    BudgetInsufficient = 5,
};

struct RuleEvaluation
{
    const ActivationRule* rule = nullptr;
    SelectorVerdict verdict    = SelectorVerdict::NotApplicable;
    std::string evidence; // which selector dimension decided it
    u64 rank_score = 0;   // deterministic lexical score (P3/P4 only)
};

struct SelectionResult
{
    std::vector<RuleEvaluation> evaluations; // every rule, in policy order
    std::vector<const ActivationRule*> protected_included;
    std::vector<const ActivationRule*> unresolved;        // critical unknowns stay visible
    std::vector<const ActivationRule*> ranked_candidates; // P3 ordered by score desc, rule_id asc
    Readiness readiness = Readiness::ReadyForPhase;
};

[[nodiscard]] std::string_view readiness_text(Readiness readiness) noexcept;
[[nodiscard]] std::string_view selector_verdict_text(SelectorVerdict verdict) noexcept;

// Evaluate the typed rule table against the normalized task.
// selection_budget_bytes ≤ 0 means the policy default (task payload cap).
[[nodiscard]] SelectionResult evaluate_selection(const ActivationPolicy& policy,
                                                 const TaskDescriptor& task,
                                                 const Budgets& budgets,
                                                 u64 requested_budget_bytes = 0);
} // namespace qiven::runtime::cognition
