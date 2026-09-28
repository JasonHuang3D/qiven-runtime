#include <qiven/runtime/cognition/activation_service.hpp>

#include <algorithm>

namespace qiven::runtime::cognition
{
namespace
{
using ActivationOutcomeResult = qiven::Result<ActivationOutcome, ActivationError>;
}

std::string_view activation_error_text(ActivationError error) noexcept
{
    switch (error)
    {
    case ActivationError::BundlePublishFailed:
        return "task bundle publication failed";
    case ActivationError::ReceiptPersistFailed:
        return "receipt persistence failed";
    case ActivationError::EmptySelection:
        return "selection is empty";
    case ActivationError::BudgetInsufficient:
        return "protected material exceeds the task budget";
    case ActivationError::UnknownConsumerProfile:
        return "consumer profile is not enumerated in cognition-core";
    }
    return "activation error";
}

ActivationOutcomeResult ActivationService::activate(const ActivationRequest& request) const
{
    // 0. consumer-profile admission (design decision 11): the profile is
    // validated against the core's enumerated profiles - an unlisted
    // profile must fail visibly, never ride into the receipt's canonical
    // facts and digest.
    if (std::find(request.core.consumer_profiles.begin(), request.core.consumer_profiles.end(),
                  request.consumer_profile) == request.core.consumer_profiles.end())
    {
        return ActivationOutcomeResult::fail(ActivationError::UnknownConsumerProfile);
    }

    // 1. fixed normalization (condition-blind; judgment fields stay empty)
    TaskDescriptor task = normalize_task(request.envelope);

    // 2. typed selection under the core budgets
    Budgets budgets;
    budgets.compact_core_max_bytes       = request.core.compact_core_max_bytes;
    budgets.task_payload_max_bytes       = request.core.task_payload_max_bytes;
    budgets.inlined_body_max_bytes       = request.core.inlined_body_max_bytes;
    budgets.supporting_share_max_percent = request.core.supporting_share_max_percent;
    SelectionResult selection            = evaluate_selection(request.policy, task, budgets,
                                                              request.requested_budget_bytes);
    if (selection.readiness == Readiness::BudgetInsufficient)
    {
        return ActivationOutcomeResult::fail(ActivationError::BudgetInsufficient);
    }
    if (selection.protected_included.empty() && selection.unresolved.empty() &&
        selection.ranked_candidates.empty())
    {
        return ActivationOutcomeResult::fail(ActivationError::EmptySelection);
    }

    // 3. canonical bundle publication (deterministic; binds every axis)
    TaskBundleRequest bundle_request;
    bundle_request.task                        = task;
    bundle_request.selection                   = selection;
    bundle_request.budgets                     = budgets;
    bundle_request.requested_budget_bytes      = request.requested_budget_bytes;
    bundle_request.activation_generation       = request.activation_generation;
    bundle_request.runtime_generation_id       = request.runtime_generation_id;
    bundle_request.external_source_lock_sha256 = request.external_source_lock_sha256;
    bundle_request.activation_policy_sha256    = request.activation_policy_sha256;
    bundle_request.runtime_root                = request.runtime_root;
    const TaskBundlePublisher publisher;
    auto bundle = publisher.publish(bundle_request);
    if (!bundle.is_ok())
    {
        return ActivationOutcomeResult::fail(ActivationError::BundlePublishFailed);
    }

    // 4. receipt issuance + persistence (envelope-only time/nonce)
    ReceiptFacts facts;
    facts.task_digest                      = task.digest_hex();
    facts.bundle_id                        = bundle.value().bundle_id;
    facts.runtime_generation_id            = request.runtime_generation_id;
    facts.activation_generation            = request.activation_generation;
    facts.external_source_lock_sha256      = request.external_source_lock_sha256;
    facts.activation_policy_sha256         = request.activation_policy_sha256;
    facts.budget_bytes                     = request.requested_budget_bytes > 0
                                                 ? (request.requested_budget_bytes <
                                        budgets.task_payload_max_bytes
                                                        ? request.requested_budget_bytes
                                                        : budgets.task_payload_max_bytes)
                                                 : budgets.task_payload_max_bytes;
    facts.consumer_profile                 = request.consumer_profile;
    facts.evidence_expires_ms              = 0; // v1: no external live-evidence binding (declared)
    ContextActivationReceipt receipt = issue_receipt(facts, request.now_ms);
    // WR-7 cutover: the parent WorkspaceGeneration rides the receipt
    // envelope (provenance-only; never in canonical_json/compute_id).
    receipt.workspace_generation = request.workspace_generation;

    const ActivationReceiptJournal journal(request.runtime_root / "receipts");
    if (!journal.persist(receipt).is_ok())
    {
        return ActivationOutcomeResult::fail(ActivationError::ReceiptPersistFailed);
    }

    ActivationOutcome outcome;
    outcome.task      = std::move(task);
    outcome.selection = std::move(selection);
    outcome.bundle    = std::move(bundle.value());
    outcome.receipt   = std::move(receipt);
    return ActivationOutcomeResult(std::move(outcome));
}
} // namespace qiven::runtime::cognition
