// ============================================================================
// task_bundle + activation_receipt — CA-1 PR-2 gates (design §1;
// acceptance Profile A rows 1/2/3/7 partial: determinism, binding,
// invalidation axes, journal round-trip)
// ============================================================================

#include <qiven/runtime/cognition/activation_receipt.hpp>
#include <qiven/runtime/cognition/task_bundle.hpp>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace
{
using qiven::runtime::cognition::ActivationPolicy;
using qiven::runtime::cognition::ActivationRule;
using qiven::runtime::cognition::Budgets;
using qiven::runtime::cognition::PriorityClass;
using qiven::runtime::cognition::Readiness;
using qiven::runtime::cognition::SelectionResult;
using qiven::runtime::cognition::TaskDescriptor;

ActivationRule rule_of(const std::string& id, PriorityClass cls)
{
    ActivationRule rule;
    rule.rule_id           = id;
    rule.priority_class    = cls;
    rule.source            = { "qiven-context", "memory/records/" + id + ".md", "body" };
    rule.expected_controls = { "control for " + id };
    return rule;
}

TaskDescriptor sample_task()
{
    qiven::runtime::cognition::TaskEnvelope envelope;
    envelope.objective  = "implement the wire contract per ADR-0024";
    envelope.repository = "qiven-runtime";
    envelope.revision   = "46dabf0";
    envelope.phase      = qiven::runtime::cognition::TaskPhase::Implementation;
    envelope.risk       = qiven::runtime::cognition::TaskRisk::R2;
    return qiven::runtime::cognition::normalize_task(envelope);
}

SelectionResult sample_selection(const ActivationPolicy& policy, const TaskDescriptor& task)
{
    Budgets budgets;
    return qiven::runtime::cognition::evaluate_selection(policy, task, budgets);
}
} // namespace

int main()
{
    const std::string eol(1, char(10));
    const auto workroot = std::filesystem::path(QIVEN_RUNTIME_TEST_WORKROOT);

    ActivationPolicy policy;
    policy.schema_version            = 1;
    policy.rules                     = { rule_of("CORE", PriorityClass::P0Core),
                                         rule_of("PROT", PriorityClass::P1Protected),
                                         rule_of("SUP", PriorityClass::P3Supporting) };
    policy.rules[0].selectors.phases = { "all" };
    policy.rules[1].selectors.phases = { "implementation" };
    policy.rules[2].selectors.phases = { "all" };

    const TaskDescriptor task = sample_task();
    const auto selection      = sample_selection(policy, task);
    if (selection.readiness != Readiness::ReadyForPhase ||
        selection.protected_included.size() != 2)
    {
        std::printf("[FAIL] selection precondition wrong%s", eol.c_str());
        return 1;
    }

    qiven::runtime::cognition::TaskBundleRequest request;
    request.task                        = task;
    request.selection                   = selection;
    request.budgets                     = Budgets {};
    request.activation_generation       = "aa11";
    request.runtime_generation_id       = "gen-1";
    request.external_source_lock_sha256 = std::string(64, 'f');
    request.activation_policy_sha256    = std::string(64, 'e');
    request.runtime_root                = workroot / "bundles";

    const qiven::runtime::cognition::TaskBundlePublisher publisher;

    // ---- A.1 determinism: identical closure/task/budget → identical bytes ----
    auto first = publisher.publish(request);
    if (!first.is_ok())
    {
        std::printf("[FAIL] publish failed: %s%s",
                    qiven::runtime::cognition::bundle_build_error_text(first.reason()).data(),
                    eol.c_str());
        return 2;
    }
    auto second = publisher.publish(request);
    if (!second.is_ok() || second.value().bundle_id != first.value().bundle_id ||
        second.value().manifest_json != first.value().manifest_json)
    {
        std::printf("[FAIL] bundle not deterministic%s", eol.c_str());
        return 3;
    }
    if (first.value().manifest_json.find("\"activation_generation\":\"aa11\"") ==
            std::string::npos ||
        first.value().manifest_json.find("\"runtime_generation\":\"gen-1\"") ==
            std::string::npos ||
        first.value().manifest_json.find("\"external_source_lock_sha256\":\"" +
                                         std::string(64, 'f') + "\"") == std::string::npos)
    {
        std::printf("[FAIL] bundle bindings missing%s", eol.c_str());
        return 4;
    }
    std::printf("[ OK ] bundle deterministic with full bindings%s", eol.c_str());

    // ---- A.3 changed task → different bundle; changed lock → different bundle ----
    auto changed_task           = request;
    changed_task.task.objective = "different objective";
    auto other_bundle           = publisher.publish(changed_task);
    if (!other_bundle.is_ok() ||
        other_bundle.value().bundle_id == first.value().bundle_id)
    {
        std::printf("[FAIL] task change did not move the bundle id%s", eol.c_str());
        return 5;
    }
    auto moved_lock                        = request;
    moved_lock.external_source_lock_sha256 = std::string(64, 'g');
    if (publisher.publish(moved_lock).value().bundle_id == first.value().bundle_id)
    {
        std::printf("[FAIL] source-lock change did not move the bundle id%s", eol.c_str());
        return 6;
    }
    std::printf("[ OK ] task and lock changes move the bundle id%s", eol.c_str());

    // ---- budget overflow typed ----
    auto overflow                = request;
    overflow.selection.readiness = Readiness::BudgetInsufficient;
    if (publisher.publish(overflow).is_ok())
    {
        std::printf("[FAIL] budget overflow accepted%s", eol.c_str());
        return 7;
    }
    std::printf("[ OK ] budget overflow typed-rejected%s", eol.c_str());

    // ---- receipts: issue/verify/invalidations/journal ----
    qiven::runtime::cognition::ReceiptFacts facts;
    facts.task_digest                 = task.digest_hex();
    facts.bundle_id                   = first.value().bundle_id;
    facts.runtime_generation_id       = "gen-1";
    facts.activation_generation       = "aa11";
    facts.external_source_lock_sha256 = std::string(64, 'f');
    facts.activation_policy_sha256    = std::string(64, 'e');
    facts.budget_bytes                = 65536;
    facts.consumer_profile            = "zcode-jason";
    facts.evidence_expires_ms         = 2'000'000;

    const auto receipt = qiven::runtime::cognition::issue_receipt(facts, 1'000'000);
    if (receipt.compute_id() != receipt.receipt_id)
    {
        std::printf("[FAIL] receipt id does not bind its facts%s", eol.c_str());
        return 8;
    }
    using qiven::runtime::cognition::ReceiptVerify;
    using qiven::runtime::cognition::verify_receipt;
    if (verify_receipt(receipt, facts, 1'500'000) != ReceiptVerify::Valid)
    {
        std::printf("[FAIL] receipt not valid against identical facts%s", eol.c_str());
        return 9;
    }
    if (verify_receipt(receipt, facts, 2'500'000) != ReceiptVerify::EvidenceExpired)
    {
        std::printf("[FAIL] evidence expiry not detected%s", eol.c_str());
        return 10;
    }
    auto moved                  = facts;
    moved.activation_generation = "bb22";
    if (verify_receipt(receipt, moved, 1'500'000) != ReceiptVerify::GenerationChanged)
    {
        std::printf("[FAIL] activation-generation change not typed%s", eol.c_str());
        return 11;
    }
    moved                             = facts;
    moved.external_source_lock_sha256 = std::string(64, 'g');
    if (verify_receipt(receipt, moved, 1'500'000) != ReceiptVerify::SourceLockChanged)
    {
        std::printf("[FAIL] source-lock change not typed%s", eol.c_str());
        return 12;
    }
    moved             = facts;
    moved.task_digest = "other";
    if (verify_receipt(receipt, moved, 1'500'000) != ReceiptVerify::TaskChanged)
    {
        std::printf("[FAIL] task change not typed%s", eol.c_str());
        return 13;
    }
    // tamper: facts no longer match the id
    auto tampered               = receipt;
    tampered.facts.budget_bytes = 1;
    if (verify_receipt(tampered, facts, 1'500'000) != ReceiptVerify::IdMismatch)
    {
        std::printf("[FAIL] tampered receipt not detected%s", eol.c_str());
        return 14;
    }
    std::printf("[ OK ] receipt invalidation matrix typed%s", eol.c_str());

    // ---- journal round-trip + determinism-of-facts across issuances ----
    const qiven::runtime::cognition::ActivationReceiptJournal journal(workroot / "journal");
    if (!journal.persist(receipt).is_ok())
    {
        std::printf("[FAIL] persist failed%s", eol.c_str());
        return 15;
    }
    auto loaded = journal.load(receipt.receipt_id);
    if (!loaded.is_ok() || !loaded.value().has_value())
    {
        std::printf("[FAIL] load failed%s", eol.c_str());
        return 16;
    }
    if (loaded.value()->facts.budget_bytes != facts.budget_bytes ||
        loaded.value()->facts.activation_generation != facts.activation_generation ||
        loaded.value()->receipt_id != receipt.receipt_id)
    {
        std::printf("[FAIL] round-trip facts wrong%s", eol.c_str());
        return 17;
    }
    const auto reissued = qiven::runtime::cognition::issue_receipt(facts, 1'000'001);
    if (reissued.receipt_id != receipt.receipt_id)
    {
        std::printf("[FAIL] receipt id not deterministic in facts%s", eol.c_str());
        return 18;
    }
    if (reissued.nonce_hex == receipt.nonce_hex)
    {
        std::printf("[FAIL] nonce not per-issuance%s", eol.c_str());
        return 19;
    }
    auto unknown = journal.load("nonexistent");
    if (!unknown.is_ok() || unknown.value().has_value())
    {
        std::printf("[FAIL] unknown receipt not empty%s", eol.c_str());
        return 20;
    }
    std::printf("[ OK ] journal round-trip, id deterministic, nonce per-issuance%s",
                eol.c_str());

    // ---- budget axis in the cache key: different resolved budgets are
    // DIFFERENT bundles (TCA §17.3 — the cache key covers the budget;
    // no aliasing to the first-cached artifact) ----
    {
        qiven::runtime::cognition::TaskBundleRequest narrower = request;
        narrower.requested_budget_bytes                       = 60000; // < the 65536 default → resolves to 60000
        auto small                                            = publisher.publish(narrower);
        if (!small.is_ok())
        {
            std::printf("[FAIL] narrower-budget publish failed%s", eol.c_str());
            return 21;
        }
        if (small.value().bundle_id == first.value().bundle_id)
        {
            std::printf("[FAIL] budget change aliased to the same bundle id%s", eol.c_str());
            return 22;
        }
        // an above-cap request resolves to the cap → identical to default
        qiven::runtime::cognition::TaskBundleRequest wider = request;
        wider.requested_budget_bytes                       = 999999;
        auto capped                                        = publisher.publish(wider);
        if (!capped.is_ok() || capped.value().bundle_id != first.value().bundle_id)
        {
            std::printf("[FAIL] above-cap request not clamped to the policy cap%s",
                        eol.c_str());
            return 23;
        }
        std::printf("[ OK ] budget keys the bundle identity; above-cap clamps to the cap%s",
                    eol.c_str());
    }

    std::printf("TASK-BUNDLE-RECEIPT PASS%s", eol.c_str());
    return 0;
}
