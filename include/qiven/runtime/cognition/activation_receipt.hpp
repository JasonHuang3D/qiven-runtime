#pragma once

// ============================================================================
// cognition/activation_receipt.hpp — CA-1 ContextActivationReceipt
// (design §1; TCA-ARCH §12)
//
// Binds EVERY fact activation depends on: task digest, bundle id, the
// execution RuntimeGeneration, the ActivationGeneration, the external
// source lock digest, the policy digest, the budget, the consumer
// profile, and freshness facts. The CANONICAL receipt payload carries no
// timestamp or random value (deterministic); issuance time and nonce
// live only in the envelope (§11.2). Any bound fact changing invalidates
// the receipt (verify fails typed); replay across a changed task,
// generation, lock, policy, or renderer is impossible by construction.
// Persistence: the activation sidecar's own sqlite journal (receipts
// table; the RuntimeJournal records control facts only — CA-4 absorbs).
// ============================================================================

#include <qiven/result.hpp>
#include <qiven/types.hpp>

#include <filesystem>
#include <string>

namespace qiven::runtime::cognition
{
enum class ReceiptError : u8
{
    Invalid     = 1, // bound facts do not match (stale/tampered/replayed)
    Persistence = 2, // journal unavailable
    Unknown     = 3,
};

[[nodiscard]] std::string_view receipt_error_text(ReceiptError error) noexcept;

struct ReceiptFacts
{
    std::string task_digest;
    std::string bundle_id;
    std::string runtime_generation_id;
    std::string activation_generation;
    std::string external_source_lock_sha256;
    std::string activation_policy_sha256;
    u64 budget_bytes = 0;
    std::string consumer_profile;
    u64 evidence_expires_ms = 0; // 0 = no live-evidence freshness bound
};

struct ContextActivationReceipt
{
    ReceiptFacts facts;     // canonical (deterministic payload input)
    std::string receipt_id; // sha256 over the canonical facts JSON
    u64 issued_at_ms = 0;   // envelope only — never in the digest
    std::string nonce_hex;  // envelope only (CSPRNG per issuance)
    // WR-7 cutover (ADR-0058 decision 6): the parent WorkspaceGeneration
    // the activation was selected under — envelope ONLY, never in
    // canonical_json/compute_id (provenance, never a content input).
    std::string workspace_generation;

    // Deterministic canonical identity over the bound facts.
    [[nodiscard]] std::string canonical_json() const;
    // sha256 over canonical_json — the receipt_id recomputation basis.
    [[nodiscard]] std::string compute_id() const;
};

// Issue a receipt: canonical facts + envelope (issued_at injected; nonce
// CSPRNG). Deterministic in facts; envelope fields differ per issuance.
[[nodiscard]] ContextActivationReceipt issue_receipt(const ReceiptFacts& facts, u64 now_ms);

enum class ReceiptVerify : u8
{
    Valid             = 0,
    TaskChanged       = 1,
    BundleChanged     = 2,
    GenerationChanged = 3, // either generation axis moved
    SourceLockChanged = 4,
    PolicyChanged     = 5,
    BudgetChanged     = 6,
    ProfileChanged    = 7,
    EvidenceExpired   = 8,
    IdMismatch        = 9,
};

[[nodiscard]] std::string_view receipt_verify_text(ReceiptVerify result) noexcept;

// Verify a receipt against the CURRENT facts: every axis must match
// exactly; a mismatch names the first changed axis (typed, never a bare
// boolean). Evidence expiry checked against now_ms when bound.
[[nodiscard]] ReceiptVerify verify_receipt(const ContextActivationReceipt& receipt,
                                           const ReceiptFacts& current, u64 now_ms);

class ActivationReceiptJournal
{
public:
    explicit ActivationReceiptJournal(std::filesystem::path sidecar_root);

    // Persist one issuance (idempotent on receipt_id).
    [[nodiscard]] qiven::Result<void, ReceiptError> persist(
        const ContextActivationReceipt& receipt) const;

    // Load by receipt id (nullopt = unknown).
    [[nodiscard]] qiven::Result<std::optional<ContextActivationReceipt>, ReceiptError> load(
        const std::string& receipt_id) const;

private:
    std::filesystem::path m_root;
};
} // namespace qiven::runtime::cognition
