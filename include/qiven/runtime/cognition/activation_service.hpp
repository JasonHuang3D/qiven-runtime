#pragma once

// ============================================================================
// cognition/activation_service.hpp — CA-1 shared activation core
// (design §1: the one core both the one-shot CLI and the future resident
// host worker use)
//
// activate(): normalize the envelope → evaluate the typed policy →
// publish the TaskCognitionBundle → issue + persist the receipt, over a
// PRE-BUILT activation index (the caller pins the bundle/index; this
// service never touches canonical repositories). Deterministic in every
// canonical output; envelope-only fields (issued_at, nonce) differ per
// call by law.
// ============================================================================

#include <qiven/result.hpp>
#include <qiven/runtime/cognition/activation_index.hpp>
#include <qiven/runtime/cognition/activation_policy.hpp>
#include <qiven/runtime/cognition/activation_receipt.hpp>
#include <qiven/runtime/cognition/selector_eval.hpp>
#include <qiven/runtime/cognition/task_bundle.hpp>
#include <qiven/runtime/cognition/task_descriptor.hpp>

#include <string>

namespace qiven::runtime::cognition
{
enum class ActivationError : u8
{
    BundlePublishFailed    = 1,
    ReceiptPersistFailed   = 2,
    EmptySelection         = 3,
    BudgetInsufficient     = 4,
    UnknownConsumerProfile = 5,
};

[[nodiscard]] std::string_view activation_error_text(ActivationError error) noexcept;

struct ActivationRequest
{
    TaskEnvelope envelope;
    CognitionCore core;                // budgets + registry facts
    ActivationPolicy policy;           // typed rule table
    std::string activation_generation; // from the built index
    std::string runtime_generation_id; // execution generation
    std::string external_source_lock_sha256;
    std::string activation_policy_sha256;
    std::string consumer_profile;
    // WR-7 cutover (ADR-0058 decision 6): the parent WorkspaceGeneration,
    // validated by the caller against the index sidecar's provenance
    // record — copied into the receipt ENVELOPE only (never canonical).
    std::string workspace_generation;
    std::filesystem::path runtime_root; // sidecar home (task-bundles/, receipts)
    u64 requested_budget_bytes = 0;     // ≤0 → policy default
    u64 now_ms                 = 0;     // injected clock (envelope only)
};

struct ActivationOutcome
{
    TaskDescriptor task;
    SelectionResult selection;
    TaskBundleResult bundle;
    ContextActivationReceipt receipt;
};

class ActivationService
{
public:
    [[nodiscard]] qiven::Result<ActivationOutcome, ActivationError> activate(
        const ActivationRequest& request) const;
};
} // namespace qiven::runtime::cognition
