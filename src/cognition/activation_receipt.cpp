#include <qiven/hashing_sha256.hpp>
#include <qiven/runtime/auth.hpp>
#include <qiven/runtime/cognition/activation_receipt.hpp>

#include <sqlite3.h>

namespace qiven::runtime::cognition
{
namespace
{
void json_escape(std::string& out, std::string_view text)
{
    out.push_back('"');
    for (const char ch : text)
    {
        if (ch == '"' || ch == '\\')
        {
            out.push_back('\\');
        }
        if (static_cast<unsigned char>(ch) >= 0x20)
        {
            out.push_back(ch);
        }
        else
        {
            out += ' ';
        }
    }
    out.push_back('"');
}

std::string sha256_hex(std::string_view bytes)
{
    qiven::SHA256Hasher hasher;
    hasher.update(reinterpret_cast<const std::byte*>(bytes.data()), bytes.size());
    const qiven::SHA256Digest digest = hasher.finish();
    static constexpr char hex[]      = "0123456789abcdef";
    std::string out;
    out.resize(digest.size() * 2);
    for (usize i = 0; i < digest.size(); ++i)
    {
        const auto b   = static_cast<unsigned char>(digest[i]);
        out[2 * i]     = hex[b >> 4];
        out[2 * i + 1] = hex[b & 0x0Fu];
    }
    return out;
}

std::string hex_of(const qiven::SHA256Digest& digest)
{
    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    out.resize(digest.size() * 2);
    for (usize i = 0; i < digest.size(); ++i)
    {
        const auto b   = static_cast<unsigned char>(digest[i]);
        out[2 * i]     = hex[b >> 4];
        out[2 * i + 1] = hex[b & 0x0Fu];
    }
    return out;
}
} // namespace

std::string_view receipt_error_text(ReceiptError error) noexcept
{
    switch (error)
    {
    case ReceiptError::Invalid:
        return "receipt facts invalid";
    case ReceiptError::Persistence:
        return "receipt journal unavailable";
    case ReceiptError::Unknown:
        return "receipt unknown";
    }
    return "receipt error";
}

std::string ContextActivationReceipt::canonical_json() const
{
    std::string out = "{\"schema\":\"qiven-context-activation-receipt-v1\",\"task_digest\":";
    json_escape(out, facts.task_digest);
    out += ",\"bundle_id\":";
    json_escape(out, facts.bundle_id);
    out += ",\"runtime_generation\":";
    json_escape(out, facts.runtime_generation_id);
    out += ",\"activation_generation\":";
    json_escape(out, facts.activation_generation);
    out += ",\"external_source_lock_sha256\":";
    json_escape(out, facts.external_source_lock_sha256);
    out += ",\"activation_policy_sha256\":";
    json_escape(out, facts.activation_policy_sha256);
    out += ",\"budget\":";
    out += std::to_string(facts.budget_bytes);
    out += ",\"consumer_profile\":";
    json_escape(out, facts.consumer_profile);
    out += ",\"evidence_expires_ms\":";
    out += std::to_string(facts.evidence_expires_ms);
    out.push_back('}');
    return out;
}

std::string ContextActivationReceipt::compute_id() const
{
    return sha256_hex(canonical_json());
}

ContextActivationReceipt issue_receipt(const ReceiptFacts& facts, u64 now_ms)
{
    ContextActivationReceipt receipt;
    receipt.facts        = facts;
    receipt.receipt_id   = receipt.compute_id();
    receipt.issued_at_ms = now_ms;
    qiven::SHA256Digest nonce {};
    qiven::runtime::auth::csrandom_fill(nonce);
    receipt.nonce_hex = hex_of(nonce);
    return receipt;
}

std::string_view receipt_verify_text(ReceiptVerify result) noexcept
{
    switch (result)
    {
    case ReceiptVerify::Valid:
        return "valid";
    case ReceiptVerify::TaskChanged:
        return "task changed";
    case ReceiptVerify::BundleChanged:
        return "bundle changed";
    case ReceiptVerify::GenerationChanged:
        return "generation changed";
    case ReceiptVerify::SourceLockChanged:
        return "source lock changed";
    case ReceiptVerify::PolicyChanged:
        return "policy changed";
    case ReceiptVerify::BudgetChanged:
        return "budget changed";
    case ReceiptVerify::ProfileChanged:
        return "consumer profile changed";
    case ReceiptVerify::EvidenceExpired:
        return "live evidence expired";
    case ReceiptVerify::IdMismatch:
        return "receipt id does not match its facts";
    }
    return "invalid";
}

ReceiptVerify verify_receipt(const ContextActivationReceipt& receipt, const ReceiptFacts& current,
                             u64 now_ms)
{
    // id must still bind its own facts (tamper axis before freshness)
    if (receipt.receipt_id != receipt.compute_id())
    {
        return ReceiptVerify::IdMismatch;
    }
    if (receipt.facts.task_digest != current.task_digest)
    {
        return ReceiptVerify::TaskChanged;
    }
    if (receipt.facts.bundle_id != current.bundle_id)
    {
        return ReceiptVerify::BundleChanged;
    }
    if (receipt.facts.runtime_generation_id != current.runtime_generation_id ||
        receipt.facts.activation_generation != current.activation_generation)
    {
        return ReceiptVerify::GenerationChanged;
    }
    if (receipt.facts.external_source_lock_sha256 != current.external_source_lock_sha256)
    {
        return ReceiptVerify::SourceLockChanged;
    }
    if (receipt.facts.activation_policy_sha256 != current.activation_policy_sha256)
    {
        return ReceiptVerify::PolicyChanged;
    }
    if (receipt.facts.budget_bytes != current.budget_bytes)
    {
        return ReceiptVerify::BudgetChanged;
    }
    if (receipt.facts.consumer_profile != current.consumer_profile)
    {
        return ReceiptVerify::ProfileChanged;
    }
    if (receipt.facts.evidence_expires_ms != 0 && now_ms > receipt.facts.evidence_expires_ms)
    {
        return ReceiptVerify::EvidenceExpired;
    }
    return ReceiptVerify::Valid;
}

// ---- sidecar journal -------------------------------------------------------
ActivationReceiptJournal::ActivationReceiptJournal(std::filesystem::path sidecar_root) :
m_root(std::move(sidecar_root))
{
}

qiven::Result<void, ReceiptError> ActivationReceiptJournal::persist(
    const ContextActivationReceipt& receipt) const
{
    std::filesystem::create_directories(m_root);
    sqlite3* db     = nullptr;
    const auto file = m_root / "receipts.sqlite3";
    if (sqlite3_open_v2(file.string().c_str(), &db,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX,
                        nullptr) != SQLITE_OK)
    {
        sqlite3_close_v2(db);
        return qiven::Result<void, ReceiptError>::fail(ReceiptError::Persistence);
    }
    char* error   = nullptr;
    const bool ok = sqlite3_exec(db,
                                 "CREATE TABLE IF NOT EXISTS receipts("
                                 "receipt_id TEXT PRIMARY KEY, facts_json TEXT NOT NULL, "
                                 "issued_at_ms INTEGER NOT NULL, nonce_hex TEXT NOT NULL);",
                                 nullptr, nullptr, &error) == SQLITE_OK;
    sqlite3_free(error);
    if (!ok)
    {
        sqlite3_close_v2(db);
        return qiven::Result<void, ReceiptError>::fail(ReceiptError::Persistence);
    }
    // WR-7 cutover (ADR-0058 decision 6): the envelope provenance column,
    // added lazily to pre-cutover journals (idempotent migration).
    bool has_provenance_column = false;
    sqlite3_stmt* columns       = nullptr;
    if (sqlite3_prepare_v2(db, "PRAGMA table_info(receipts)", -1, &columns, nullptr) == SQLITE_OK)
    {
        while (sqlite3_step(columns) == SQLITE_ROW)
        {
            const char* name = reinterpret_cast<const char*>(sqlite3_column_text(columns, 1));
            if (name != nullptr && std::string(name) == "workspace_generation")
            {
                has_provenance_column = true;
                break;
            }
        }
        sqlite3_finalize(columns);
    }
    if (!has_provenance_column)
    {
        char* alter_error  = nullptr;
        const bool altered = sqlite3_exec(db,
                                          "ALTER TABLE receipts ADD COLUMN workspace_generation "
                                          "TEXT NOT NULL DEFAULT '';",
                                          nullptr, nullptr, &alter_error) == SQLITE_OK;
        sqlite3_free(alter_error);
        if (!altered)
        {
            sqlite3_close_v2(db);
            return qiven::Result<void, ReceiptError>::fail(ReceiptError::Persistence);
        }
    }
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db,
                           "INSERT OR IGNORE INTO receipts(receipt_id, facts_json, issued_at_ms, "
                           "nonce_hex, workspace_generation) VALUES(?,?,?,?,?)",
                           -1, &stmt, nullptr) != SQLITE_OK)
    {
        sqlite3_close_v2(db);
        return qiven::Result<void, ReceiptError>::fail(ReceiptError::Persistence);
    }
    sqlite3_bind_text(stmt, 1, receipt.receipt_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, receipt.canonical_json().c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(receipt.issued_at_ms));
    sqlite3_bind_text(stmt, 4, receipt.nonce_hex.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, receipt.workspace_generation.c_str(), -1, SQLITE_TRANSIENT);
    const bool step = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    sqlite3_close_v2(db);
    if (!step)
    {
        return qiven::Result<void, ReceiptError>::fail(ReceiptError::Persistence);
    }
    return qiven::Result<void, ReceiptError>::ok();
}

qiven::Result<std::optional<ContextActivationReceipt>, ReceiptError>
ActivationReceiptJournal::load(const std::string& receipt_id) const
{
    const auto file = m_root / "receipts.sqlite3";
    if (!std::filesystem::exists(file))
    {
        return qiven::Result<std::optional<ContextActivationReceipt>, ReceiptError>(
            std::optional<ContextActivationReceipt> {});
    }
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(file.string().c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
    {
        sqlite3_close_v2(db);
        return qiven::Result<std::optional<ContextActivationReceipt>, ReceiptError>::fail(
            ReceiptError::Persistence);
    }
    sqlite3_stmt* stmt = nullptr;
    bool provenance_column = true;
    if (sqlite3_prepare_v2(db,
                           "SELECT facts_json, issued_at_ms, nonce_hex, workspace_generation "
                           "FROM receipts WHERE receipt_id = ?",
                           -1, &stmt, nullptr) != SQLITE_OK)
    {
        // pre-cutover journal without the provenance column: fall back to
        // the legacy 4-column read (provenance honestly empty — those
        // receipts were issued before the WR-7 cutover recorded one)
        provenance_column = false;
        if (sqlite3_prepare_v2(db,
                               "SELECT facts_json, issued_at_ms, nonce_hex FROM receipts WHERE "
                               "receipt_id = ?",
                               -1, &stmt, nullptr) != SQLITE_OK)
        {
            sqlite3_close_v2(db);
            return qiven::Result<std::optional<ContextActivationReceipt>, ReceiptError>::fail(
                ReceiptError::Persistence);
        }
    }
    sqlite3_bind_text(stmt, 1, receipt_id.c_str(), -1, SQLITE_TRANSIENT);
    const int step = sqlite3_step(stmt);
    if (step != SQLITE_ROW)
    {
        sqlite3_finalize(stmt);
        sqlite3_close_v2(db);
        if (step == SQLITE_DONE)
        {
            return qiven::Result<std::optional<ContextActivationReceipt>, ReceiptError>(
                std::optional<ContextActivationReceipt> {});
        }
        return qiven::Result<std::optional<ContextActivationReceipt>, ReceiptError>::fail(
            ReceiptError::Persistence);
    }

    // Rehydrate the facts from the canonical JSON (field-by-field exact
    // extraction; unknown shapes fail closed as Persistence).
    const std::string facts_json = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    ContextActivationReceipt receipt;
    receipt.issued_at_ms = static_cast<u64>(sqlite3_column_int64(stmt, 1));
    receipt.nonce_hex    = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    if (provenance_column && sqlite3_column_type(stmt, 3) != SQLITE_NULL)
    {
        const char* provenance = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        if (provenance != nullptr)
        {
            receipt.workspace_generation = provenance;
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close_v2(db);

    auto field = [&](const char* key) {
        const std::string needle = std::string("\"") + key + "\":\"";
        const usize at           = facts_json.find(needle);
        if (at == std::string::npos)
        {
            return std::string();
        }
        const usize begin = at + needle.size();
        const usize end   = facts_json.find('"', begin);
        return end == std::string::npos ? std::string() : facts_json.substr(begin, end - begin);
    };
    auto number = [&](const char* key) {
        const std::string needle = std::string("\"") + key + "\":";
        const usize at           = facts_json.find(needle);
        if (at == std::string::npos)
        {
            return u64 { 0 };
        }
        return static_cast<u64>(std::strtoull(facts_json.c_str() + at + needle.size(), nullptr, 10));
    };
    receipt.facts.task_digest                 = field("task_digest");
    receipt.facts.bundle_id                   = field("bundle_id");
    receipt.facts.runtime_generation_id       = field("runtime_generation");
    receipt.facts.activation_generation       = field("activation_generation");
    receipt.facts.external_source_lock_sha256 = field("external_source_lock_sha256");
    receipt.facts.activation_policy_sha256    = field("activation_policy_sha256");
    receipt.facts.budget_bytes                = number("budget");
    receipt.facts.consumer_profile            = field("consumer_profile");
    receipt.facts.evidence_expires_ms         = number("evidence_expires_ms");
    receipt.receipt_id                        = receipt_id;

    if (receipt.compute_id() != receipt_id)
    {
        return qiven::Result<std::optional<ContextActivationReceipt>, ReceiptError>::fail(
            ReceiptError::Invalid);
    }
    return qiven::Result<std::optional<ContextActivationReceipt>, ReceiptError>(
        std::optional<ContextActivationReceipt> { std::move(receipt) });
}
} // namespace qiven::runtime::cognition
