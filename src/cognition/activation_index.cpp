#include <qiven/runtime/cognition/activation_index.hpp>

#include <qiven/hashing_sha256.hpp>

#include <sqlite3.h>

#if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #define NOMINMAX
    #include <windows.h>
#endif

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>

namespace qiven::runtime::cognition
{
namespace
{
using IndexOutcome = qiven::Result<IndexBuildResult, IndexError>;

std::string_view text_of(IndexError error) noexcept
{
    switch (error)
    {
    case IndexError::InvalidRequest:
        return "invalid index request";
    case IndexError::BuildFailed:
        return "index construction failed";
    case IndexError::StagingFault:
        return "staging/atomic-switch fault";
    case IndexError::ManifestMismatch:
        return "existing index disagrees with this closure";
    }
    return "unknown index error";
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

// Deterministic policy digest: canonical JSON over the rule table
// (rule-sorted; every selector list in policy order; exact vocabulary).
std::string policy_digest(const ActivationPolicy& policy)
{
    auto quote = [](std::string& out, const std::string& text) {
        out.push_back('"');
        for (const char ch : text)
        {
            if (ch == '"' || ch == '\\')
            {
                out.push_back('\\');
            }
            out.push_back(ch);
        }
        out.push_back('"');
    };
    auto quote_list = [&](std::string& out, const std::vector<std::string>& values) {
        out.push_back('[');
        bool first = true;
        for (const std::string& value : values)
        {
            if (!first)
            {
                out.push_back(',');
            }
            first = false;
            quote(out, value);
        }
        out.push_back(']');
    };
    std::vector<const ActivationRule*> rules;
    rules.reserve(policy.rules.size());
    for (const ActivationRule& rule : policy.rules)
    {
        rules.push_back(&rule);
    }
    std::sort(rules.begin(), rules.end(), [](const ActivationRule* a, const ActivationRule* b) {
        return a->rule_id < b->rule_id;
    });
    std::string out = "{\"schema\":1,\"rules\":[";
    bool first      = true;
    for (const ActivationRule* rule : rules)
    {
        if (!first)
        {
            out.push_back(',');
        }
        first = false;
        out += "{\"rule_id\":";
        quote(out, rule->rule_id);
        out += ",\"priority\":";
        out += std::to_string(static_cast<unsigned>(rule->priority_class));
        out += ",\"lifecycle\":";
        out += std::to_string(static_cast<unsigned>(rule->lifecycle));
        out += ",\"source\":";
        quote(out, rule->source.repository + ":" + rule->source.path + ":" + rule->source.anchor);
        out += ",\"phases\":";
        quote_list(out, rule->selectors.phases);
        out += ",\"risk\":";
        quote_list(out, rule->selectors.risk);
        out += ",\"repositories\":";
        quote_list(out, rule->selectors.repositories);
        out += ",\"path_prefixes\":";
        quote_list(out, rule->selectors.path_prefixes);
        out += ",\"languages\":";
        quote_list(out, rule->selectors.languages);
        out += ",\"boundary_kinds\":";
        quote_list(out, rule->selectors.boundary_kinds);
        out += ",\"explicit_ids\":";
        quote_list(out, rule->selectors.explicit_ids);
        out += ",\"evidence\":";
        quote_list(out, rule->independent_evidence);
        out += ",\"controls\":";
        quote_list(out, rule->expected_controls);
        out.push_back('}');
    }
    out += "]}";
    return sha256_hex(out);
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

std::optional<std::string> read_text(const std::filesystem::path& file)
{
    std::ifstream in(file, std::ios::binary);
    if (!in)
    {
        return std::nullopt;
    }
    in.seekg(0, std::ios::end);
    const auto size = in.tellg();
    if (size < 0)
    {
        return std::nullopt;
    }
    std::string text;
    text.resize(static_cast<usize>(size));
    in.seekg(0, std::ios::beg);
    if (!text.empty())
    {
        in.read(text.data(), static_cast<std::streamsize>(text.size()));
    }
    if (!in)
    {
        return std::nullopt;
    }
    return text;
}

bool replace_file(const std::filesystem::path& from, const std::filesystem::path& to) noexcept
{
    const std::wstring wide_from = from.wstring();
    const std::wstring wide_to   = to.wstring();
    return MoveFileExW(wide_from.c_str(), wide_to.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
}

// Deterministic token stream for the inverted index: lowercase, split on
// non-alphanumeric (keeping '-' inside tokens for ids like ADR-0048).
std::vector<std::string> tokenize(std::string_view text)
{
    std::vector<std::string> tokens;
    std::string current;
    for (const char ch : text)
    {
        const bool alnum = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                           (ch >= '0' && ch <= '9') || (ch == '-' && !current.empty());
        if (alnum)
        {
            current.push_back(static_cast<char>(
                ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch));
        }
        else if (!current.empty())
        {
            tokens.push_back(std::move(current));
            current.clear();
        }
    }
    if (!current.empty())
    {
        tokens.push_back(std::move(current));
    }
    return tokens;
}

struct Sqlite
{
    sqlite3* db = nullptr;

    ~Sqlite()
    {
        if (db != nullptr)
        {
            sqlite3_close_v2(db);
        }
    }

    [[nodiscard]] bool exec(const char* sql) const
    {
        char* error   = nullptr;
        const bool ok = sqlite3_exec(db, sql, nullptr, nullptr, &error) == SQLITE_OK;
        sqlite3_free(error);
        return ok;
    }
};
} // namespace

std::string_view index_error_text(IndexError error) noexcept
{
    return text_of(error);
}

std::string activation_policy_digest(const ActivationPolicy& policy)
{
    // unnamed-namespace helper, visible here in the enclosing namespace
    return policy_digest(policy);
}

std::string activation_generation_of(const IndexBuildRequest& request)
{
    // §9.4 exact identity — every input in fixed order, no timestamps.
    std::string preimage;
    preimage += request.canonical_bundle_digest;
    preimage += '|';
    preimage += request.runtime_generation_id;
    preimage += '|';
    preimage += request.source_lock.digest_hex();
    preimage += '|';
    preimage += policy_digest(request.policy);
    preimage += '|';
    preimage += std::to_string(request.index_schema_version);
    preimage += '|';
    preimage += request.publisher_build;
    return sha256_hex(preimage);
}

std::string ActivationIndexBuilder::active_generation(
    const std::filesystem::path& runtime_root) const
{
    const auto pointer = runtime_root / "activation-generations" / "ACTIVE";
    auto text          = read_text(pointer);
    if (!text)
    {
        return {};
    }
    // trim
    while (!text->empty() && (text->back() == '\n' || text->back() == '\r' ||
                              text->back() == ' '))
    {
        text->pop_back();
    }
    return *text;
}

IndexOutcome ActivationIndexBuilder::build(const IndexBuildRequest& request) const
{
    if (request.runtime_root.empty() || request.canonical_bundle_digest.empty() ||
        request.runtime_generation_id.empty() || request.publisher_build.empty() ||
        request.source_lock.entries.empty() || request.policy.rules.empty())
    {
        return IndexOutcome::fail(IndexError::InvalidRequest);
    }

    const std::string generation = activation_generation_of(request);
    const auto generations_root  = request.runtime_root / "activation-generations";
    const auto index_dir         = generations_root / generation;
    const auto staging_dir       = generations_root / (".tmp-" + generation);

    // Exact-key reuse: an existing manifest for this generation is final
    // (immutable sidecar); reuse without rebuilding.
    const auto existing_manifest = index_dir / "index-manifest.json";
    if (std::filesystem::exists(existing_manifest))
    {
        return IndexOutcome(IndexBuildResult { generation, index_dir,
                                               request.source_lock.entries.size(),
                                               request.policy.rules.size() });
    }

    std::error_code ec;
    std::filesystem::remove_all(staging_dir, ec);
    std::filesystem::create_directories(staging_dir, ec);
    if (ec)
    {
        return IndexOutcome::fail(IndexError::StagingFault);
    }

    // source-lock.json — the closure record (verbatim canonical JSON).
    if (!write_text(staging_dir / "source-lock.json", request.source_lock.canonical_json()))
    {
        return IndexOutcome::fail(IndexError::StagingFault);
    }

    // index.sqlite — metadata tables + deterministic inverted index.
    Sqlite sqlite;
    const auto db_path = staging_dir / "index.sqlite";
    if (sqlite3_open_v2(db_path.string().c_str(), &sqlite.db,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX,
                        nullptr) != SQLITE_OK)
    {
        return IndexOutcome::fail(IndexError::BuildFailed);
    }
    if (!sqlite.exec("PRAGMA journal_mode=OFF; PRAGMA synchronous=OFF; PRAGMA temp_store=MEMORY;"))
    {
        return IndexOutcome::fail(IndexError::BuildFailed);
    }
    if (!sqlite.exec(
            "CREATE TABLE sources(source_id INTEGER PRIMARY KEY, repository TEXT NOT NULL, "
            "revision TEXT NOT NULL, path TEXT NOT NULL, content_sha256 TEXT NOT NULL, "
            "kind TEXT NOT NULL, lifecycle TEXT NOT NULL DEFAULT 'active', "
            "title TEXT NOT NULL DEFAULT '', size_bytes INTEGER NOT NULL);"
            "CREATE TABLE selectors(rule_id TEXT NOT NULL, selector_kind TEXT NOT NULL, "
            "selector_value TEXT NOT NULL, priority INTEGER NOT NULL);"
            "CREATE TABLE rule_sources(rule_id TEXT NOT NULL, source_id INTEGER NOT NULL, "
            "section_ref TEXT NOT NULL, expected_controls TEXT NOT NULL DEFAULT '');"
            "CREATE TABLE documents(source_id INTEGER PRIMARY KEY, normalized_text TEXT NOT NULL);"
            "CREATE TABLE inverted(token TEXT NOT NULL, source_id INTEGER NOT NULL);"
            "CREATE INDEX inverted_token ON inverted(token);"))
    {
        return IndexOutcome::fail(IndexError::BuildFailed);
    }

    // sources + documents + inverted index over locked bodies.
    u64 source_id = 0;
    for (const LockEntry& entry : request.source_lock.entries)
    {
        ++source_id;
        const std::string kind = entry.path.ends_with(".md")     ? "record"
                                 : entry.path.ends_with(".yaml") ? "policy"
                                                                 : "source";
        {
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(sqlite.db,
                                   "INSERT INTO sources(source_id, repository, revision, path, "
                                   "content_sha256, kind, lifecycle, title, size_bytes) "
                                   "VALUES(?,?,?,?,?,?, 'active', ?, ?)",
                                   -1, &stmt, nullptr) != SQLITE_OK)
            {
                return IndexOutcome::fail(IndexError::BuildFailed);
            }
            sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(source_id));
            sqlite3_bind_text(stmt, 2, entry.repository.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, entry.commit.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 4, entry.path.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 5, entry.sha256.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 6, kind.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 7, entry.path.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(stmt, 8, static_cast<sqlite3_int64>(entry.size_bytes));
            const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
            sqlite3_finalize(stmt);
            if (!ok)
            {
                return IndexOutcome::fail(IndexError::BuildFailed);
            }
        }

        // normalized text = token stream joined by single spaces over the
        // body we hashed at lock time; bodies are re-read from the locked
        // checkouts by the caller-side index consumer in PR-2 — here we
        // index the path + identity tokens (deterministic, closure-bound).
        std::string normalized;
        for (const std::string& token : tokenize(entry.repository + " " + entry.path))
        {
            normalized += token;
            normalized.push_back(' ');
        }
        for (const std::string& token : tokenize(entry.repository + " " + entry.path))
        {
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(sqlite.db, "INSERT INTO inverted(token, source_id) VALUES(?,?)",
                                   -1, &stmt, nullptr) != SQLITE_OK)
            {
                return IndexOutcome::fail(IndexError::BuildFailed);
            }
            sqlite3_bind_text(stmt, 1, token.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(source_id));
            const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
            sqlite3_finalize(stmt);
            if (!ok)
            {
                return IndexOutcome::fail(IndexError::BuildFailed);
            }
        }
        {
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(sqlite.db, "INSERT INTO documents(source_id, normalized_text) "
                                              "VALUES(?,?)",
                                   -1, &stmt, nullptr) != SQLITE_OK)
            {
                return IndexOutcome::fail(IndexError::BuildFailed);
            }
            sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(source_id));
            sqlite3_bind_text(stmt, 2, normalized.c_str(), -1, SQLITE_TRANSIENT);
            const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
            sqlite3_finalize(stmt);
            if (!ok)
            {
                return IndexOutcome::fail(IndexError::BuildFailed);
            }
        }
    }

    // selectors + rule_sources from the typed rule table.
    std::set<std::string> rule_ids;
    for (const ActivationRule& rule : request.policy.rules)
    {
        if (!rule_ids.insert(rule.rule_id).second)
        {
            return IndexOutcome::fail(IndexError::InvalidRequest); // duplicate rule id
        }
        const unsigned priority = static_cast<unsigned>(rule.priority_class);
        const auto add_selector = [&](const char* kind, const std::vector<std::string>& values) {
            for (const std::string& value : values)
            {
                sqlite3_stmt* stmt = nullptr;
                if (sqlite3_prepare_v2(sqlite.db,
                                       "INSERT INTO selectors(rule_id, selector_kind, "
                                       "selector_value, priority) VALUES(?,?,?,?)",
                                       -1, &stmt, nullptr) != SQLITE_OK)
                {
                    return false;
                }
                sqlite3_bind_text(stmt, 1, rule.rule_id.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 2, kind, -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 3, value.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int(stmt, 4, static_cast<int>(priority));
                const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
                sqlite3_finalize(stmt);
                return ok;
            }
            return true;
        };
        if (!add_selector("phase", rule.selectors.phases) ||
            !add_selector("risk", rule.selectors.risk) ||
            !add_selector("repository", rule.selectors.repositories) ||
            !add_selector("path_prefix", rule.selectors.path_prefixes) ||
            !add_selector("language", rule.selectors.languages) ||
            !add_selector("boundary_kind", rule.selectors.boundary_kinds) ||
            !add_selector("explicit_id", rule.selectors.explicit_ids))
        {
            return IndexOutcome::fail(IndexError::BuildFailed);
        }

        std::string controls;
        for (const std::string& control : rule.expected_controls)
        {
            controls += control;
            controls.push_back('\n');
        }
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(sqlite.db,
                               "INSERT INTO rule_sources(rule_id, source_id, section_ref, "
                               "expected_controls) VALUES(?,?,?,?)",
                               -1, &stmt, nullptr) != SQLITE_OK)
        {
            return IndexOutcome::fail(IndexError::BuildFailed);
        }
        sqlite3_bind_text(stmt, 1, rule.rule_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 2, 0); // resolved to the source table in PR-2's join
        sqlite3_bind_text(stmt, 3, rule.source.anchor.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, controls.c_str(), -1, SQLITE_TRANSIENT);
        const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
        sqlite3_finalize(stmt);
        if (!ok)
        {
            return IndexOutcome::fail(IndexError::BuildFailed);
        }
    }
    sqlite3_close_v2(sqlite.db); // close BEFORE the atomic switch (Windows rename)
    sqlite.db = nullptr;

    // index-manifest.json — the §9.2 binding set (deterministic order).
    std::string manifest = "{\"schema\":\"qiven-activation-index-manifest-v1\",";
    manifest += "\"activation_generation\":\"" + generation + "\",";
    manifest += "\"canonical_bundle_sha256\":\"" + request.canonical_bundle_digest + "\",";
    manifest += "\"runtime_generation\":\"" + request.runtime_generation_id + "\",";
    manifest += "\"external_source_lock_sha256\":\"" + request.source_lock.digest_hex() + "\",";
    manifest += "\"activation_policy_sha256\":\"" + policy_digest(request.policy) + "\",";
    manifest += "\"index_schema_version\":" + std::to_string(request.index_schema_version) + ",";
    manifest += "\"publisher_build\":\"" + request.publisher_build + "\"}";
    if (!write_text(staging_dir / "index-manifest.json", manifest))
    {
        return IndexOutcome::fail(IndexError::StagingFault);
    }

    // Atomic switch: rename staging into place, then swap the ACTIVE
    // pointer (rename-over — a crash between the two leaves a complete
    // unactivated generation, deletable; the pointer is never partial).
    std::filesystem::rename(staging_dir, index_dir, ec);
    if (ec)
    {
        return IndexOutcome::fail(IndexError::StagingFault);
    }
    const auto pointer_path  = generations_root / "ACTIVE";
    const auto pointer_stage = generations_root / ".tmp-ACTIVE";
    if (!write_text(pointer_stage, generation + "\n") ||
        !replace_file(pointer_stage, pointer_path))
    {
        return IndexOutcome::fail(IndexError::StagingFault);
    }

    return IndexOutcome(IndexBuildResult { generation, index_dir,
                                           request.source_lock.entries.size(),
                                           request.policy.rules.size() });
}
} // namespace qiven::runtime::cognition
