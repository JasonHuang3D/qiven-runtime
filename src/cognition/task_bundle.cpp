#include <qiven/runtime/cognition/task_bundle.hpp>

#include <qiven/hashing_sha256.hpp>

#include <algorithm>
#include <fstream>

#if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #define NOMINMAX
    #include <windows.h>
#endif

namespace qiven::runtime::cognition
{
namespace
{
using BundleOutcome = qiven::Result<TaskBundleResult, BundleBuildError>;

std::string_view text_of(BundleBuildError error) noexcept
{
    switch (error)
    {
    case BundleBuildError::EmptySelection:
        return "selection is empty";
    case BundleBuildError::BudgetExceeded:
        return "protected material exceeds the task budget";
    case BundleBuildError::CompactCoreExceeded:
        return "compact core exceeds its budget";
    case BundleBuildError::PublishFault:
        return "publication fault";
    }
    return "unknown bundle error";
}

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

bool write_text(const std::filesystem::path& file, std::string_view bytes)
{
    std::filesystem::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        return false;
    }
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

bool replace_file(const std::filesystem::path& from, const std::filesystem::path& to) noexcept
{
    const std::wstring wide_from = from.wstring();
    const std::wstring wide_to   = to.wstring();
    return MoveFileExW(wide_from.c_str(), wide_to.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
}
} // namespace

std::string_view bundle_build_error_text(BundleBuildError error) noexcept
{
    return text_of(error);
}

BundleOutcome TaskBundlePublisher::publish(const TaskBundleRequest& request) const
{
    if (request.selection.protected_included.empty() &&
        request.selection.ranked_candidates.empty() && request.selection.unresolved.empty())
    {
        return BundleOutcome::fail(BundleBuildError::EmptySelection);
    }
    if (request.selection.readiness == Readiness::BudgetInsufficient)
    {
        return BundleOutcome::fail(BundleBuildError::BudgetExceeded);
    }

    // ---- canonical payload (deterministic; no timestamp/nonce/volatile) ----
    // Protected entries in policy order first (stability), then ranked
    // candidates in deterministic rank order, then unresolved criticals.
    std::string payload = "{\"schema\":\"qiven-task-cognition-bundle-v1\",\"task\":";
    payload += request.task.canonical_json();

    payload += ",\"bindings\":{\"activation_generation\":";
    json_escape(payload, request.activation_generation);
    payload += ",\"runtime_generation\":";
    json_escape(payload, request.runtime_generation_id);
    payload += ",\"external_source_lock_sha256\":";
    json_escape(payload, request.external_source_lock_sha256);
    payload += ",\"activation_policy_sha256\":";
    json_escape(payload, request.activation_policy_sha256);
    payload += ",\"budget\":";
    payload +=
        std::to_string(request.requested_budget_bytes > 0
                           ? request.requested_budget_bytes
                           : request.budgets.task_payload_max_bytes);
    payload += "}";

    auto append_rule = [&payload](const ActivationRule& rule, const char* section) {
        payload += ",{\"rule_id\":";
        json_escape(payload, rule.rule_id);
        payload += ",\"priority\":";
        payload += std::to_string(static_cast<unsigned>(rule.priority_class));
        payload += ",\"source\":";
        json_escape(payload, rule.source.repository + ":" + rule.source.path + ":" +
                                 rule.source.anchor);
        payload += ",\"controls\":";
        json_escape(payload, [controls = &rule.expected_controls]() {
            std::string joined;
            for (const std::string& control : *controls)
            {
                joined += control;
                joined.push_back('\n');
            }
            return joined;
        }());
        payload += "}";
        (void)section;
    };

    payload += ",\"protected\":[";
    bool first = true;
    for (const ActivationRule* rule : request.selection.protected_included)
    {
        if (!first)
        {
            payload.push_back(',');
        }
        first = false;
        append_rule(*rule, "protected");
    }
    payload += "],\"unresolved\":[";
    first = true;
    for (const ActivationRule* rule : request.selection.unresolved)
    {
        if (!first)
        {
            payload.push_back(',');
        }
        first = false;
        append_rule(*rule, "unresolved");
    }
    payload += "],\"candidates\":[";
    first = true;
    for (const ActivationRule* rule : request.selection.ranked_candidates)
    {
        if (!first)
        {
            payload.push_back(',');
        }
        first = false;
        append_rule(*rule, "candidate");
    }
    payload += "]}";

    const u64 payload_bytes = payload.size();
    if (payload_bytes > (request.requested_budget_bytes > 0
                             ? request.requested_budget_bytes
                             : request.budgets.task_payload_max_bytes))
    {
        return BundleOutcome::fail(BundleBuildError::BudgetExceeded);
    }

    // ---- identity: bundle_id derived from every canonical input ----
    std::string identity_input = request.task.digest_hex();
    identity_input += '|';
    identity_input += request.activation_generation;
    identity_input += '|';
    identity_input += request.runtime_generation_id;
    identity_input += '|';
    identity_input += request.external_source_lock_sha256;
    identity_input += '|';
    identity_input += request.activation_policy_sha256;
    const std::string bundle_id = sha256_hex(identity_input);

    // ---- atomic publish ----
    const auto bundles_root = request.runtime_root / "task-bundles";
    const auto bundle_dir   = bundles_root / bundle_id;
    const auto staging      = bundles_root / (".tmp-" + bundle_id);
    std::error_code ec;
    std::filesystem::remove_all(staging, ec);
    std::filesystem::create_directories(staging, ec);
    if (ec)
    {
        return BundleOutcome::fail(BundleBuildError::PublishFault);
    }
    if (!write_text(staging / "bundle.json", payload))
    {
        return BundleOutcome::fail(BundleBuildError::PublishFault);
    }
    // exact-key reuse: an identical bundle is already final
    if (!std::filesystem::exists(bundle_dir))
    {
        std::filesystem::rename(staging, bundle_dir, ec);
        if (ec)
        {
            return BundleOutcome::fail(BundleBuildError::PublishFault);
        }
    }
    else
    {
        std::filesystem::remove_all(staging, ec);
    }
    // the ACTIVE-task pointer is NOT switched here: per-task bundles are
    // addressed by receipt, not by a global pointer (§11.2 immutability)
    (void)replace_file;

    return BundleOutcome(TaskBundleResult {
        bundle_id, bundle_dir, payload, payload_bytes,
        request.selection.protected_included.size(), request.selection.ranked_candidates.size() });
}
} // namespace qiven::runtime::cognition
