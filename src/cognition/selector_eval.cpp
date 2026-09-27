#include <qiven/runtime/cognition/selector_eval.hpp>

#include <algorithm>

namespace qiven::runtime::cognition
{
namespace
{
bool contains(const std::vector<std::string>& values, const std::string& value)
{
    return std::find(values.begin(), values.end(), value) != values.end();
}

bool path_under_prefix(const std::string& path, const std::string& prefix)
{
    if (path.size() > prefix.size() && path.compare(0, prefix.size(), prefix) == 0)
    {
        return true;
    }
    // component-wise for prefixes written without a trailing slash
    return path.size() > prefix.size() && path.compare(0, prefix.size(), prefix) == 0 &&
           path[prefix.size()] == '/';
}

// Deterministic lexical rank: exact-token overlap with the objective and
// changed paths; exact matches outrank partial runs; ties break by
// rule_id (stable order enforced by the caller's sort).
[[nodiscard]] u64 rank_score_of(const ActivationRule& rule, const TaskDescriptor& task)
{
    u64 score                   = 0;
    const std::string objective = task.objective;
    for (const std::string& control : rule.expected_controls)
    {
        if (objective.find(control) != std::string::npos)
        {
            score += 100; // exact phrase overlap
        }
        else
        {
            // token overlap
            for (const char ch : control)
            {
                (void)ch;
            }
            usize begin = 0;
            while (begin < control.size())
            {
                const usize end = control.find(' ', begin);
                const std::string word =
                    control.substr(begin, end == std::string::npos ? std::string::npos
                                                                   : end - begin);
                if (word.size() >= 5 && objective.find(word) != std::string::npos)
                {
                    score += 10;
                }
                if (end == std::string::npos)
                {
                    break;
                }
                begin = end + 1;
            }
        }
    }
    for (const std::string& id : rule.selectors.explicit_ids)
    {
        if (contains(task.explicit_ids, id))
        {
            score += 50;
        }
    }
    if (contains(rule.selectors.repositories, task.repository))
    {
        score += 5;
    }
    return score;
}
} // namespace

std::string_view readiness_text(Readiness readiness) noexcept
{
    switch (readiness)
    {
    case Readiness::ReadyForPhase:
        return "ReadyForPhase";
    case Readiness::NeedsEvidence:
        return "NeedsEvidence";
    case Readiness::ReDeliberate:
        return "ReDeliberate";
    case Readiness::Blocked:
        return "Blocked";
    case Readiness::StaleGeneration:
        return "StaleGeneration";
    case Readiness::BudgetInsufficient:
        return "BudgetInsufficient";
    }
    return "Blocked";
}

std::string_view selector_verdict_text(SelectorVerdict verdict) noexcept
{
    switch (verdict)
    {
    case SelectorVerdict::Applicable:
        return "applicable";
    case SelectorVerdict::NotApplicable:
        return "not_applicable";
    case SelectorVerdict::Unresolved:
        return "unresolved";
    }
    return "not_applicable";
}

SelectionResult evaluate_selection(const ActivationPolicy& policy, const TaskDescriptor& task,
                                   const Budgets& budgets, u64 requested_budget_bytes)
{
    SelectionResult result;
    const std::string phase(phase_text(task.phase));
    const std::string risk(risk_text(task.risk));

    for (const ActivationRule& rule : policy.rules)
    {
        RuleEvaluation evaluation;
        evaluation.rule = &rule;

        // History selection law: non-active lifecycles ride only P4.
        if (rule.lifecycle != RuleLifecycle::Active &&
            rule.priority_class != PriorityClass::P4OnDemand)
        {
            evaluation.verdict  = SelectorVerdict::NotApplicable;
            evaluation.evidence = "lifecycle=" +
                                  std::to_string(static_cast<unsigned>(rule.lifecycle)) +
                                  " is selectable only as P4 history";
            result.evaluations.push_back(std::move(evaluation));
            continue;
        }

        // Explicit-ID trigger: an exact ID match applies regardless of
        // the other typed dimensions (the strongest selector).
        if (!rule.selectors.explicit_ids.empty())
        {
            bool triggered = false;
            for (const std::string& id : rule.selectors.explicit_ids)
            {
                if (contains(task.explicit_ids, id))
                {
                    triggered = true;
                    evaluation.evidence += "explicit_id=" + id + " ";
                }
            }
            if (triggered)
            {
                evaluation.verdict = SelectorVerdict::Applicable;
                result.evaluations.push_back(std::move(evaluation));
                continue;
            }
            // declared IDs not present: the trigger did not fire; the rule
            // may still apply through its other typed selectors below.
        }

        // Typed dimensions. A dimension the rule does not declare is a
        // wildcard; a declared-but-underivable one is UNRESOLVED.
        bool applicable = true;
        bool unresolved = false;
        std::string because;

        if (!rule.selectors.phases.empty())
        {
            if (contains(rule.selectors.phases, "all") || contains(rule.selectors.phases, phase))
            {
                because += "phase ";
            }
            else
            {
                applicable = false;
                because    = "phase";
            }
        }
        if (applicable && !rule.selectors.risk.empty())
        {
            if (contains(rule.selectors.risk, "all") || contains(rule.selectors.risk, risk))
            {
                because += "risk ";
            }
            else
            {
                applicable = false;
                because    = "risk";
            }
        }
        if (applicable && !rule.selectors.repositories.empty())
        {
            if (contains(rule.selectors.repositories, task.repository))
            {
                because += "repository ";
            }
            else
            {
                applicable = false;
                because    = "repository";
            }
        }
        if (applicable && !rule.selectors.path_prefixes.empty())
        {
            bool under = false;
            for (const std::string& prefix : rule.selectors.path_prefixes)
            {
                for (const std::string& path : task.changed_paths)
                {
                    if (path_under_prefix(path, prefix))
                    {
                        under = true;
                    }
                }
            }
            if (under)
            {
                because += "path_prefix ";
            }
            else
            {
                applicable = false;
                because    = "path_prefix";
            }
        }
        if (applicable && !rule.selectors.languages.empty())
        {
            bool shared = false;
            for (const std::string& language : rule.selectors.languages)
            {
                if (contains(task.languages, language))
                {
                    shared = true;
                }
            }
            if (shared)
            {
                because += "language ";
            }
            else
            {
                applicable = false;
                because    = "language";
            }
        }
        if (applicable && !rule.selectors.boundary_kinds.empty())
        {
            // v1: the fixed normalizer derives NO boundary kinds — a
            // boundary-kind-dependent rule has unknown applicability.
            unresolved = true;
            because    = "boundary_kinds (no mechanical derivation)";
        }

        if (unresolved)
        {
            evaluation.verdict  = SelectorVerdict::Unresolved;
            evaluation.evidence = because;
        }
        else if (applicable)
        {
            evaluation.verdict  = SelectorVerdict::Applicable;
            evaluation.evidence = because;
        }
        else
        {
            evaluation.verdict  = SelectorVerdict::NotApplicable;
            evaluation.evidence = because;
        }
        result.evaluations.push_back(std::move(evaluation));
    }

    // Partition: protected included / unresolved / ranked candidates.
    u64 protected_bytes = 0;
    for (const RuleEvaluation& evaluation : result.evaluations)
    {
        const ActivationRule& rule = *evaluation.rule;
        if (evaluation.verdict == SelectorVerdict::Applicable)
        {
            if (rule.priority_class == PriorityClass::P0Core ||
                rule.priority_class == PriorityClass::P1Protected ||
                rule.priority_class == PriorityClass::P2Required)
            {
                result.protected_included.push_back(&rule);
                for (const std::string& control : rule.expected_controls)
                {
                    protected_bytes += control.size() + 1;
                }
                protected_bytes += rule.rule_id.size() + rule.source.path.size() + 64;
            }
            else
            {
                result.ranked_candidates.push_back(&rule);
            }
        }
        else if (evaluation.verdict == SelectorVerdict::Unresolved &&
                 (rule.priority_class == PriorityClass::P0Core ||
                  rule.priority_class == PriorityClass::P1Protected))
        {
            // critical unknowns stay in the bundle and may block readiness
            result.unresolved.push_back(&rule);
            protected_bytes += rule.rule_id.size() + rule.source.path.size() + 64;
        }
    }

    // Deterministic ranking: score desc, rule_id asc (stable, total).
    std::stable_sort(result.ranked_candidates.begin(), result.ranked_candidates.end(),
                     [&](const ActivationRule* a, const ActivationRule* b) {
                         const u64 score_a = rank_score_of(*a, task);
                         const u64 score_b = rank_score_of(*b, task);
                         if (score_a != score_b)
                         {
                             return score_a > score_b;
                         }
                         return a->rule_id < b->rule_id;
                     });
    for (RuleEvaluation& evaluation : result.evaluations)
    {
        if (evaluation.rule->priority_class == PriorityClass::P3Supporting ||
            evaluation.rule->priority_class == PriorityClass::P4OnDemand)
        {
            evaluation.rank_score = rank_score_of(*evaluation.rule, task);
        }
    }

    // Budget law: protected material first; overflow fails closed.
    const u64 budget = requested_budget_bytes > 0 ? requested_budget_bytes
                                                  : budgets.task_payload_max_bytes;
    if (protected_bytes > budget)
    {
        result.readiness = Readiness::BudgetInsufficient;
        return result;
    }

    // Ranked candidates fill the remaining budget; the supporting share
    // cap bounds how much of the payload may be optional material.
    const u64 remaining      = budget - protected_bytes;
    const u64 supporting_cap = remaining * budgets.supporting_share_max_percent / 100;
    u64 supporting_bytes     = 0;
    std::vector<const ActivationRule*> fit;
    for (const ActivationRule* candidate : result.ranked_candidates)
    {
        u64 size = candidate->rule_id.size() + candidate->source.path.size() + 64;
        for (const std::string& control : candidate->expected_controls)
        {
            size += control.size() + 1;
        }
        if (supporting_bytes + size > supporting_cap)
        {
            break; // deterministic priority order; later candidates drop
        }
        supporting_bytes += size;
        fit.push_back(candidate);
    }
    result.ranked_candidates = std::move(fit);

    result.readiness = result.unresolved.empty() ? Readiness::ReadyForPhase
                                                 : Readiness::NeedsEvidence;
    return result;
}
} // namespace qiven::runtime::cognition
