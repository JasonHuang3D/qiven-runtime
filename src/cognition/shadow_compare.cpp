#include <qiven/runtime/cognition/shadow_compare.hpp>

#include <qiven/hashing_sha256.hpp>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string_view>

namespace qiven::runtime::cognition
{
namespace
{
// --- minimal strict JSON value parser (objects/arrays/strings/numbers/
// bools/null; duplicate keys rejected; \uXXXX escapes are NOT supported
// and REJECT the document fail-closed — the workspace lock's producer
// emits ensure_ascii=False (raw UTF-8), so an escape means the document
// did not come from the producer and must never be mangled silently) ----
struct Json
{
    enum class Kind : u8
    {
        Object,
        Array,
        String,
        Number,
        Boolean,
        Null,
    };
    Kind kind = Kind::Null;
    std::string text; // String payload (unescaped) / Number literal
    bool boolean = false;
    std::vector<Json> items;                          // Array
    std::vector<std::pair<std::string, Json>> fields; // Object (insertion order)
};

std::optional<Json> parse_json(std::string_view bytes)
{
    std::size_t at                             = 0;
    std::function<std::optional<Json>()> value = [&]() -> std::optional<Json> {
        while (at < bytes.size() && (bytes[at] == ' ' || bytes[at] == '\t' || bytes[at] == '\n' || bytes[at] == '\r'))
        {
            ++at;
        }
        if (at >= bytes.size())
        {
            return std::nullopt;
        }
        Json out;
        const char head = bytes[at];
        if (head == '{')
        {
            out.kind = Json::Kind::Object;
            ++at;
            while (at < bytes.size() && (bytes[at] == ' ' || bytes[at] == '\t' || bytes[at] == '\n' || bytes[at] == '\r'))
            {
                ++at;
            }
            if (at < bytes.size() && bytes[at] == '}')
            {
                ++at;
                return out;
            }
            while (true)
            {
                while (at < bytes.size() && (bytes[at] == ' ' || bytes[at] == '\t' || bytes[at] == '\n' || bytes[at] == '\r'))
                {
                    ++at;
                }
                if (at >= bytes.size() || bytes[at] != '"')
                {
                    return std::nullopt;
                }
                auto key = value();
                if (!key || key->kind != Json::Kind::String)
                {
                    return std::nullopt;
                }
                for (const auto& field : out.fields)
                {
                    if (field.first == key->text)
                    {
                        return std::nullopt; // duplicate key: fail closed
                    }
                }
                while (at < bytes.size() && bytes[at] != ':')
                {
                    ++at;
                }
                if (at >= bytes.size())
                {
                    return std::nullopt;
                }
                ++at;
                auto item = value();
                if (!item)
                {
                    return std::nullopt;
                }
                out.fields.emplace_back(key->text, std::move(*item));
                while (at < bytes.size() && (bytes[at] == ' ' || bytes[at] == '\t' || bytes[at] == '\n' || bytes[at] == '\r'))
                {
                    ++at;
                }
                if (at < bytes.size() && bytes[at] == ',')
                {
                    ++at;
                    continue;
                }
                if (at < bytes.size() && bytes[at] == '}')
                {
                    ++at;
                    return out;
                }
                return std::nullopt;
            }
        }
        if (head == '[')
        {
            out.kind = Json::Kind::Array;
            ++at;
            while (at < bytes.size() && (bytes[at] == ' ' || bytes[at] == '\t' || bytes[at] == '\n' || bytes[at] == '\r'))
            {
                ++at;
            }
            if (at < bytes.size() && bytes[at] == ']')
            {
                ++at;
                return out;
            }
            while (true)
            {
                auto item = value();
                if (!item)
                {
                    return std::nullopt;
                }
                out.items.push_back(std::move(*item));
                while (at < bytes.size() && (bytes[at] == ' ' || bytes[at] == '\t' || bytes[at] == '\n' || bytes[at] == '\r'))
                {
                    ++at;
                }
                if (at < bytes.size() && bytes[at] == ',')
                {
                    ++at;
                    continue;
                }
                if (at < bytes.size() && bytes[at] == ']')
                {
                    ++at;
                    return out;
                }
                return std::nullopt;
            }
        }
        if (head == '"')
        {
            out.kind = Json::Kind::String;
            ++at;
            while (at < bytes.size() && bytes[at] != '"')
            {
                if (bytes[at] == '\\' && at + 1 < bytes.size())
                {
                    const char escaped = bytes[at + 1];
                    switch (escaped)
                    {
                    case 'n': out.text.push_back('\n'); break;
                    case 't': out.text.push_back('\t'); break;
                    case 'r': out.text.push_back('\r'); break;
                    case 'b': out.text.push_back('\b'); break;
                    case 'f': out.text.push_back('\f'); break;
                    case 'u': return std::nullopt;               // \uXXXX unsupported: reject fail-closed
                    default: out.text.push_back(escaped); break; // " \ / passthrough
                    }
                    at += 2;
                    continue;
                }
                out.text.push_back(bytes[at]);
                ++at;
            }
            if (at >= bytes.size())
            {
                return std::nullopt;
            }
            ++at;
            return out;
        }
        if (head == 't' || head == 'f')
        {
            out.kind = Json::Kind::Boolean;
            if (bytes.substr(at, 4) == "true")
            {
                out.boolean = true;
                at += 4;
                return out;
            }
            if (bytes.substr(at, 5) == "false")
            {
                at += 5;
                return out;
            }
            return std::nullopt;
        }
        if (head == 'n')
        {
            if (bytes.substr(at, 4) == "null")
            {
                at += 4;
                return out;
            }
            return std::nullopt;
        }
        out.kind = Json::Kind::Number;
        while (at < bytes.size() && ((bytes[at] >= '0' && bytes[at] <= '9') || bytes[at] == '-' || bytes[at] == '+' ||
                                     bytes[at] == '.' || bytes[at] == 'e' || bytes[at] == 'E'))
        {
            out.text.push_back(bytes[at]);
            ++at;
        }
        if (out.text.empty())
        {
            return std::nullopt;
        }
        return out;
    };
    auto parsed = value();
    if (!parsed)
    {
        return std::nullopt;
    }
    while (at < bytes.size() && (bytes[at] == ' ' || bytes[at] == '\t' || bytes[at] == '\n' || bytes[at] == '\r'))
    {
        ++at;
    }
    if (at != bytes.size())
    {
        return std::nullopt;
    }
    return parsed;
}

const Json* find(const Json& object, std::string_view key)
{
    if (object.kind != Json::Kind::Object)
    {
        return nullptr;
    }
    for (const auto& field : object.fields)
    {
        if (field.first == key)
        {
            return &field.second;
        }
    }
    return nullptr;
}
} // namespace

WorkspaceLockState parse_workspace_lock(std::string_view bytes)
{
    WorkspaceLockState state;
    auto parsed = parse_json(bytes);
    if (!parsed)
    {
        return state;
    }
    if (const Json* generation = find(*parsed, "generation"); generation && generation->kind == Json::Kind::String)
    {
        state.generation = generation->text;
    }
    const Json* nodes = find(*parsed, "nodes");
    if (!nodes || nodes->kind != Json::Kind::Object)
    {
        return state;
    }
    for (const auto& [repository, node] : nodes->fields)
    {
        if (node.kind != Json::Kind::Object)
        {
            continue;
        }
        WorkspaceNodeState node_state;
        node_state.present = true;
        if (const Json* commit = find(node, "commit"); commit && commit->kind == Json::Kind::String)
        {
            node_state.commit = commit->text;
        }
        if (const Json* tree = find(node, "tree"); tree && tree->kind == Json::Kind::String)
        {
            node_state.tree = tree->text;
        }
        if (const Json* declaration = find(node, "declaration"); declaration &&
                                                                 declaration->kind == Json::Kind::Object)
        {
            if (const Json* shadow = find(*declaration, "shadow_only"); shadow && shadow->kind == Json::Kind::Boolean)
            {
                node_state.shadow_only = shadow->boolean;
            }
        }
        state.nodes.emplace(repository, std::move(node_state));
    }
    return state;
}

std::string_view shadow_divergence_text(ShadowDivergence divergence) noexcept
{
    switch (divergence)
    {
    case ShadowDivergence::None: return "none";
    case ShadowDivergence::StaleNode: return "StaleNode";
    case ShadowDivergence::NodeMissing: return "NodeMissing";
    case ShadowDivergence::FilterSetMismatch: return "FilterSetMismatch";
    case ShadowDivergence::ContentConflict: return "ContentIdentityConflict";
    }
    return "none";
}

bool ShadowCompareResult::all_equal() const
{
    for (const ShadowRepoResult& repo : repositories)
    {
        if (repo.divergence != ShadowDivergence::None)
        {
            return false;
        }
    }
    return !repositories.empty();
}

bool ShadowCompareResult::cutover_grade() const
{
    return all_equal() &&
           std::all_of(repositories.begin(), repositories.end(),
                       [](const ShadowRepoResult& repo) { return !repo.shadow_only_node; });
}

namespace
{
void compare_one(const SourceLock& lock, const WorkspaceLockState& workspace,
                 const ShadowFilterSets* filters_used, const ShadowFilterSets* filters_at_node,
                 ShadowCompareResult& result)
{
    result.workspace_generation = workspace.generation;
    result.source_lock_sha256   = lock.digest_hex();

    // per repository: the distinct (commit, root tree) recorded in the lock
    std::map<std::string, std::pair<std::string, std::string>> locked;
    std::set<std::string> conflicted;
    for (const LockEntry& entry : lock.entries)
    {
        auto& slot = locked[entry.repository];
        if (slot.first.empty())
        {
            slot = { entry.commit, entry.tree_oid };
            continue;
        }
        // one distinct (commit, tree) per repository per lock is a
        // structural invariant; a violation is loud (a future lock
        // revision recording per-directory trees would break tier-1
        // comparison silently otherwise)
        if (slot.first != entry.commit || slot.second != entry.tree_oid)
        {
            conflicted.insert(entry.repository);
            ShadowRepoResult conflict;
            conflict.repository  = entry.repository;
            conflict.lock_commit = slot.first;
            conflict.lock_tree   = slot.second;
            conflict.node_commit = workspace.nodes.count(entry.repository)
                                       ? workspace.nodes.at(entry.repository).commit
                                       : std::string();
            conflict.divergence  = ShadowDivergence::ContentConflict;
            conflict.detail      = "lock records multiple (commit, tree) pairs for one repository";
            result.repositories.push_back(std::move(conflict));
            slot = { entry.commit, entry.tree_oid };
        }
    }

    for (const auto& [repository, pair] : locked)
    {
        if (conflicted.count(repository) != 0)
        {
            continue; // already reported as ContentConflict (one row per repository)
        }
        ShadowRepoResult row;
        row.repository  = repository;
        row.lock_commit = pair.first;
        row.lock_tree   = pair.second;
        auto node       = workspace.nodes.find(repository);
        if (node == workspace.nodes.end() || !node->second.present)
        {
            row.divergence = ShadowDivergence::NodeMissing;
            row.detail     = "corpus repository has no workspace lock node";
            result.repositories.push_back(std::move(row));
            continue;
        }
        row.node_commit      = node->second.commit;
        row.node_tree        = node->second.tree;
        row.shadow_only_node = node->second.shadow_only;
        if (pair.first != node->second.commit || pair.second != node->second.tree)
        {
            row.divergence = ShadowDivergence::StaleNode;
            row.detail     = "workspace node commit/tree != the locked closure revision";
        }
        else if (filters_used != nullptr && filters_at_node != nullptr)
        {
            // tier 2: the filter dimension (per-file digests follow from
            // (commit, root tree) + filters by construction)
            if (!filters_at_node->carrier_table_present)
            {
                row.divergence = ShadowDivergence::FilterSetMismatch;
                row.detail     = "corpus table absent at the workspace node's carrier revision";
            }
            else
            {
                const auto used_it = filters_used->filters.find(repository);
                const auto node_it = filters_at_node->filters.find(repository);
                if (node_it == filters_at_node->filters.end())
                {
                    row.divergence = ShadowDivergence::FilterSetMismatch;
                    row.detail     = "corpus table at the node revision does not cover this repository";
                }
                else if (used_it == filters_used->filters.end() || used_it->second != node_it->second)
                {
                    row.divergence = ShadowDivergence::FilterSetMismatch;
                    row.detail     = "path filters differ between the lock-driving table and the node revision";
                }
            }
        }
        result.repositories.push_back(std::move(row));
    }
}
} // namespace

ShadowCompareResult shadow_compare(const SourceLock& lock, const WorkspaceLockState& workspace)
{
    ShadowCompareResult result;
    compare_one(lock, workspace, nullptr, nullptr, result);
    return result;
}

ShadowCompareResult shadow_compare(const SourceLock& lock, const WorkspaceLockState& workspace,
                                   const ShadowFilterSets& filters_used,
                                   const ShadowFilterSets& filters_at_node)
{
    ShadowCompareResult result;
    compare_one(lock, workspace, &filters_used, &filters_at_node, result);
    return result;
}

namespace
{
std::string json_escape(std::string_view text)
{
    std::string out;
    for (const char ch : text)
    {
        switch (ch)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\t': out += "\\t"; break;
        case '\r': out += "\\r"; break;
        default: out.push_back(ch); break;
        }
    }
    return out;
}
} // namespace

std::string shadow_compare_receipt_json(const ShadowCompareResult& result)
{
    std::string out = "{\"schema\":\"qiven-tca-shadow-compare-v1\",";
    out += "\"workspace_generation\":\"" + json_escape(result.workspace_generation) + "\",";
    out += "\"external_source_lock_sha256\":\"" + json_escape(result.source_lock_sha256) + "\",";
    out += "\"source_lock_binding\":\"" + json_escape(result.workspace_lock_binding) + "\",";
    out += "\"all_equal\":" + std::string(result.all_equal() ? "true" : "false") + ",";
    out += "\"cutover_grade\":" + std::string(result.cutover_grade() ? "true" : "false") + ",";
    out += "\"repositories\":[";
    bool first = true;
    for (const ShadowRepoResult& repo : result.repositories)
    {
        if (!first)
        {
            out += ",";
        }
        first = false;
        out += "{\"repository\":\"" + json_escape(repo.repository) + "\",";
        out += "\"lock_commit\":\"" + json_escape(repo.lock_commit) + "\",";
        out += "\"lock_tree\":\"" + json_escape(repo.lock_tree) + "\",";
        out += "\"node_commit\":\"" + json_escape(repo.node_commit) + "\",";
        out += "\"node_tree\":\"" + json_escape(repo.node_tree) + "\",";
        out += "\"divergence\":\"" + std::string(shadow_divergence_text(repo.divergence)) + "\",";
        out += "\"shadow_only_node\":" + std::string(repo.shadow_only_node ? "true" : "false") + ",";
        out += "\"detail\":\"" + json_escape(repo.detail) + "\"}";
    }
    out += "]}";
    return out;
}
} // namespace qiven::runtime::cognition
