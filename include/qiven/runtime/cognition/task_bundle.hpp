#pragma once

// ============================================================================
// cognition/task_bundle.hpp — CA-1 TaskCognitionBundle assembly and
// publication (design §1; TCA-ARCH §11)
//
// Assembles the immutable per-task bundle from a SelectionResult under
// the §15.1 budgets and publishes it atomically under
// <runtime_root>/task-bundles/<digest>/. Canonical bytes carry NO
// timestamp, nonce, or volatile path (§11.2): bundle_id is derived
// without self-reference; issuance time/nonce live only in the receipt.
// The manifest binds task digest, BOTH generations (execution +
// activation), the external source lock digest, the policy digest, and
// the budget — every fact a receipt later invalidates on.
// ============================================================================

#include <qiven/result.hpp>
#include <qiven/runtime/cognition/activation_policy.hpp>
#include <qiven/runtime/cognition/selector_eval.hpp>
#include <qiven/runtime/cognition/task_descriptor.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace qiven::runtime::cognition
{
enum class BundleBuildError : u8
{
    EmptySelection      = 1,
    BudgetExceeded      = 2, // protected material beyond the task budget
    CompactCoreExceeded = 3,
    PublishFault        = 4,
};

[[nodiscard]] std::string_view bundle_build_error_text(BundleBuildError error) noexcept;

struct TaskBundleRequest
{
    TaskDescriptor task;
    SelectionResult selection;
    Budgets budgets;
    u64 requested_budget_bytes = 0; // ≤0 → policy default
    std::string activation_generation;
    std::string runtime_generation_id;
    std::string external_source_lock_sha256;
    std::string activation_policy_sha256;
    std::filesystem::path runtime_root;
};

struct TaskBundleResult
{
    std::string bundle_id;            // deterministic content identity
    std::filesystem::path bundle_dir; // .../task-bundles/<bundle_id>
    std::string manifest_json;        // the canonical manifest bytes
    u64 payload_bytes   = 0;
    u64 protected_count = 0;
    u64 candidate_count = 0;
};

class TaskBundlePublisher
{
public:
    [[nodiscard]] qiven::Result<TaskBundleResult, BundleBuildError> publish(
        const TaskBundleRequest& request) const;
};
} // namespace qiven::runtime::cognition
