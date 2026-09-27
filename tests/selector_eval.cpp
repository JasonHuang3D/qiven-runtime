// ============================================================================
// selector_eval + task_descriptor — CA-1 PR-2 selection gates (design §1;
// schema v1 law: typed-only protected selection, unknown ≠ false)
// ============================================================================

#include <qiven/runtime/cognition/selector_eval.hpp>
#include <qiven/runtime/cognition/task_descriptor.hpp>

#include <algorithm>
#include <cstdio>
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
using qiven::runtime::cognition::SelectorVerdict;
using qiven::runtime::cognition::TaskDescriptor;
using qiven::runtime::cognition::TaskEnvelope;
using qiven::runtime::cognition::TaskPhase;
using qiven::runtime::cognition::TaskRisk;

ActivationRule make_rule(const std::string& id, PriorityClass cls)
{
    ActivationRule rule;
    rule.rule_id        = id;
    rule.priority_class = cls;
    rule.source         = { "qiven-context", "memory/records/" + id + ".md", "body" };
    return rule;
}
} // namespace

int main()
{
    const std::string eol(1, char(10));

    // ---- normalizer: mechanical derivation only, deterministic digest ----
    TaskEnvelope envelope;
    envelope.objective     = "Consolidate byte mechanics per ADR-0024 and MEM-20260921T203500Z-D2A7F4";
    envelope.repository    = "qiven-runtime";
    envelope.revision      = "46dabf0";
    envelope.changed_paths = { "src/ipc/framing.cpp", "docs/design/x.md" };
    envelope.phase         = TaskPhase::Implementation;
    envelope.risk          = TaskRisk::R2;
    const auto task        = qiven::runtime::cognition::normalize_task(envelope);
    const auto has         = [](const std::vector<std::string>& values, const char* value) {
        return std::find(values.begin(), values.end(), std::string(value)) != values.end();
    };
    if (!has(task.languages, "cpp") || !has(task.languages, "markdown"))
    {
        std::printf("[FAIL] language mapping wrong%s", eol.c_str());
        return 1;
    }
    if (task.boundary_kinds.size() != 0)
    {
        std::printf("[FAIL] judgment field populated by the fixed normalizer%s", eol.c_str());
        return 2;
    }
    bool saw_adr = false;
    bool saw_mem = false;
    for (const std::string& id : task.explicit_ids)
    {
        if (id == "ADR-0024")
        {
            saw_adr = true;
        }
        if (id == "MEM-20260921T203500Z-D2A7F4")
        {
            saw_mem = true;
        }
    }
    if (!saw_adr || !saw_mem)
    {
        std::printf("[FAIL] explicit id extraction wrong (%zu ids)%s", task.explicit_ids.size(),
                    eol.c_str());
        for (const std::string& id : task.explicit_ids)
        {
            std::printf("  id: [%s]%s", id.c_str(), eol.c_str());
        }
        return 3;
    }
    const auto again = qiven::runtime::cognition::normalize_task(envelope);
    if (again.digest_hex() != task.digest_hex())
    {
        std::printf("[FAIL] normalizer not deterministic%s", eol.c_str());
        return 4;
    }
    std::printf("[ OK ] normalizer mechanical, deterministic%s", eol.c_str());

    // ---- typed protected selection ----
    ActivationPolicy policy;
    policy.schema_version = 1;

    ActivationRule core   = make_rule("CORE", PriorityClass::P0Core);
    core.selectors.phases = { "all" };
    core.selectors.risk   = { "all" };

    ActivationRule typed      = make_rule("TYPED", PriorityClass::P1Protected);
    typed.selectors.phases    = { "implementation" };
    typed.selectors.risk      = { "R2", "R3" };
    typed.selectors.languages = { "cpp" };

    ActivationRule wrong_phase   = make_rule("WRONGPHASE", PriorityClass::P1Protected);
    wrong_phase.selectors.phases = { "specify" };

    ActivationRule boundary           = make_rule("BOUNDARY", PriorityClass::P1Protected);
    boundary.selectors.phases         = { "all" };
    boundary.selectors.boundary_kinds = { "serialization" };

    ActivationRule history   = make_rule("HISTORY", PriorityClass::P1Protected);
    history.lifecycle        = RuleLifecycle::Superseded;
    history.selectors.phases = { "all" };

    ActivationRule support    = make_rule("SUP-A", PriorityClass::P3Supporting);
    support.selectors.phases  = { "all" };
    support.expected_controls = { "Consolidate byte mechanics per ADR-0024 and MEM-20260921T203500Z-D2A7F4" };

    ActivationRule support2    = make_rule("SUP-B", PriorityClass::P3Supporting);
    support2.selectors.phases  = { "all" };
    support2.expected_controls = { "unrelated material entirely different words" };

    policy.rules = { core, typed, wrong_phase, boundary, history, support, support2 };

    Budgets budgets;
    budgets.task_payload_max_bytes       = 65536;
    budgets.supporting_share_max_percent = 25;
    const auto selection =
        qiven::runtime::cognition::evaluate_selection(policy, task, budgets);

    // verdicts per rule
    const auto verdict_of = [&](const std::string& id) {
        for (const auto& evaluation : selection.evaluations)
        {
            if (evaluation.rule->rule_id == id)
            {
                return evaluation.verdict;
            }
        }
        return SelectorVerdict::NotApplicable;
    };
    if (verdict_of("CORE") != SelectorVerdict::Applicable ||
        verdict_of("TYPED") != SelectorVerdict::Applicable ||
        verdict_of("WRONGPHASE") != SelectorVerdict::NotApplicable ||
        verdict_of("BOUNDARY") != SelectorVerdict::Unresolved ||
        verdict_of("HISTORY") != SelectorVerdict::NotApplicable)
    {
        std::printf("[FAIL] typed verdicts wrong%s", eol.c_str());
        return 5;
    }
    if (selection.protected_included.size() != 2 || selection.unresolved.size() != 1 ||
        selection.unresolved[0]->rule_id != "BOUNDARY")
    {
        std::printf("[FAIL] partition wrong (%zu protected, %zu unresolved)%s",
                    selection.protected_included.size(), selection.unresolved.size(),
                    eol.c_str());
        return 6;
    }
    if (selection.readiness != Readiness::NeedsEvidence)
    {
        std::printf("[FAIL] unresolved critical must affect readiness%s", eol.c_str());
        return 7;
    }
    // ranking: exact-overlap SUP-A outranks SUP-B
    if (selection.ranked_candidates.size() != 2 ||
        selection.ranked_candidates[0]->rule_id != "SUP-A")
    {
        std::printf("[FAIL] deterministic ranking wrong%s", eol.c_str());
        return 8;
    }
    std::printf("[ OK ] typed selection, unresolved-critical, ranking%s", eol.c_str());

    // ---- budget law: protected overflow = BudgetInsufficient, no truncation ----
    Budgets tight;
    tight.task_payload_max_bytes       = 10; // below the protected set
    tight.supporting_share_max_percent = 25;
    const auto overflow =
        qiven::runtime::cognition::evaluate_selection(policy, task, tight);
    if (overflow.readiness != Readiness::BudgetInsufficient ||
        overflow.protected_included.size() != 2)
    {
        std::printf("[FAIL] protected overflow not BudgetInsufficient%s", eol.c_str());
        return 9;
    }
    // no unresolved rules → ReadyForPhase
    ActivationPolicy clean = policy;
    clean.rules            = { core, typed, wrong_phase, history, support, support2 };
    const auto ready       = qiven::runtime::cognition::evaluate_selection(clean, task, budgets);
    if (ready.readiness != Readiness::ReadyForPhase)
    {
        std::printf("[FAIL] clean selection not ready%s", eol.c_str());
        return 10;
    }
    std::printf("[ OK ] budget law and readiness%s", eol.c_str());

    // ---- envelope-axis vocabulary parsing: unknown/empty = nullopt (the
    // caller must fail visibly; silent coercion to a default is the
    // fail-visible-law violation class) ----
    using qiven::runtime::cognition::parse_task_phase;
    using qiven::runtime::cognition::parse_task_risk;
    if (!parse_task_phase("design").has_value() ||
        parse_task_phase("design").value() != qiven::runtime::cognition::TaskPhase::Design ||
        parse_task_phase("deploy").has_value() || parse_task_phase("").has_value() ||
        parse_task_phase("DESIGN").has_value() ||
        parse_task_risk("R3").value() != qiven::runtime::cognition::TaskRisk::R3 ||
        parse_task_risk("r2").has_value() || parse_task_risk("R9").has_value() ||
        parse_task_risk("").has_value())
    {
        std::printf("[FAIL] envelope axis vocabulary parsing wrong%s", eol.c_str());
        return 11;
    }
    std::printf("[ OK ] envelope axis vocabulary parses exact or nullopt%s", eol.c_str());

    // ---- path-prefix selection is component-boundary only: 'src/ipc'
    // must NOT match 'src/ipc2/other.cpp' ----
    {
        ActivationPolicy prefix_policy      = policy;
        ActivationRule prefix_rule          = make_rule("PFX-IPC", PriorityClass::P3Supporting);
        prefix_rule.selectors.phases        = { "all" };
        prefix_rule.selectors.path_prefixes = { "src/ipc" };
        prefix_policy.rules.push_back(prefix_rule);
        TaskDescriptor under;
        under.objective     = task.objective;
        under.repository    = task.repository;
        under.changed_paths = { "src/ipc/pipe.cpp" };
        under.phase         = task.phase;
        under.risk          = task.risk;
        const auto hit      = qiven::runtime::cognition::evaluate_selection(prefix_policy, under,
                                                                            budgets);
        TaskDescriptor sibling;
        sibling.objective     = task.objective;
        sibling.repository    = task.repository;
        sibling.changed_paths = { "src/ipc2/other.cpp" };
        sibling.phase         = task.phase;
        sibling.risk          = task.risk;
        const auto miss       = qiven::runtime::cognition::evaluate_selection(prefix_policy,
                                                                              sibling, budgets);
        const auto ranked_has =
            [](const qiven::runtime::cognition::SelectionResult& result) {
                return std::find_if(result.ranked_candidates.begin(), result.ranked_candidates.end(),
                                    [](const ActivationRule* rule) {
                                        return rule->rule_id == "PFX-IPC";
                                    }) != result.ranked_candidates.end();
            };
        if (!ranked_has(hit) || ranked_has(miss))
        {
            std::printf("[FAIL] path prefix match is not component-boundary%s", eol.c_str());
            return 12;
        }
    }
    std::printf("[ OK ] path prefix matches whole components only%s", eol.c_str());

    std::printf("SELECTION PASS%s", eol.c_str());
    return 0;
}
