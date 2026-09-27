#include <qiven/runtime/cognition/source_lock.hpp>

#include <qiven/hashing_sha256.hpp>

#include <algorithm>
#include <sstream>

namespace qiven::runtime::cognition
{
namespace
{
using LockOutcome = qiven::Result<SourceLock, LockError>;

std::string_view text_of(LockError error) noexcept
{
    switch (error)
    {
    case LockError::GitUnavailable:
        return "git plumbing unavailable";
    case LockError::RevisionUnresolved:
        return "ref did not resolve to the checkout's pinned commit";
    case LockError::DirtyCheckout:
        return "checkout is dirty (validated-local law: exact, clean revisions only)";
    case LockError::BoundExceeded:
        return "source lock bound exceeded";
    case LockError::PathOutsideFilter:
        return "resolved path escapes the declared filter";
    case LockError::MalformedEntry:
        return "unusable plumbing output";
    }
    return "unknown lock error";
}

[[nodiscard]] LockError git_error(const std::string& what)
{
    (void)what;
    return LockError::GitUnavailable;
}

// One bounded git invocation in the publisher's exact shape.
struct GitIo
{
    std::filesystem::path git_executable;
    std::filesystem::path checkout;
    const processx::ProcessRunner* runner;

    [[nodiscard]] qiven::Result<std::string, LockError> run(
        const std::vector<std::string>& args) const
    {
        processx::ProcessSpec spec;
        spec.executable = git_executable;
        spec.argv       = { "git", "-C", checkout.string() };
        spec.argv.insert(spec.argv.end(), args.begin(), args.end());
        spec.working_dir = checkout;
        spec.deadline_ms = 60'000;
        auto run         = runner->run(spec);
        if (!run.is_ok())
        {
            return qiven::Result<std::string, LockError>::fail(git_error(run.reason().message));
        }
        if (run.value().end == processx::ProcessRun::End::TimedOut)
        {
            return qiven::Result<std::string, LockError>::fail(git_error("deadline"));
        }
        if (run.value().exit_code != 0)
        {
            return qiven::Result<std::string, LockError>::fail(
                git_error("exit " + std::to_string(run.value().exit_code)));
        }
        return qiven::Result<std::string, LockError>(std::move(run.value().out));
    }
};

bool is_hex_oid(std::string_view text)
{
    if (text.size() != 40)
    {
        return false;
    }
    return text.find_first_not_of("0123456789abcdef") == std::string_view::npos;
}

std::string trim(std::string value)
{
    while (!value.empty() &&
           (value.back() == '\n' || value.back() == '\r' || value.back() == ' '))
    {
        value.pop_back();
    }
    usize begin = 0;
    while (begin < value.size() &&
           (value[begin] == '\n' || value[begin] == '\r' || value[begin] == ' '))
    {
        ++begin;
    }
    return value.substr(begin);
}

// Path-component filter match (publisher's selected_by shape; a declared
// file matches exactly, a declared directory by components — never a
// string prefix).
[[nodiscard]] bool selected_by(const std::string& path,
                               const std::vector<std::string>& filters)
{
    for (const auto& entry : filters)
    {
        if (path == entry)
        {
            return true;
        }
        if (path.size() > entry.size() && path.compare(0, entry.size(), entry) == 0 &&
            path[entry.size()] == '/')
        {
            return true;
        }
    }
    return false;
}

// Minimal deterministic JSON string escaping (paths and hex only in
// practice, but correctness first: ", \, control chars).
void json_escape(std::string& out, std::string_view text)
{
    out.push_back('"');
    for (const char ch : text)
    {
        switch (ch)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20)
            {
                char buffer[8];
                std::snprintf(buffer, sizeof(buffer), "\\u%04x",
                              static_cast<unsigned>(static_cast<unsigned char>(ch)));
                out += buffer;
            }
            else
            {
                out.push_back(ch);
            }
            break;
        }
    }
    out.push_back('"');
}

std::vector<std::string> split_lines(const std::string& text)
{
    std::vector<std::string> lines;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line))
    {
        while (!line.empty() && (line.back() == '\r'))
        {
            line.pop_back();
        }
        if (!line.empty())
        {
            lines.push_back(line);
        }
    }
    return lines;
}

struct TreeRow
{
    std::string oid;
    std::string path;
    u64 size = 0;
};

// ls-tree -r -l <tree>: "<mode> <type> <oid>\t<size>\t<path>" with -l.
[[nodiscard]] qiven::Result<std::vector<TreeRow>, LockError> list_tree(const GitIo& io,
                                                                       const std::string& tree_oid)
{
    auto listing = io.run({ "ls-tree", "-r", "-l", tree_oid });
    if (!listing.is_ok())
    {
        return qiven::Result<std::vector<TreeRow>, LockError>::fail(listing.reason());
    }
    std::vector<TreeRow> rows;
    for (const std::string& line : split_lines(listing.value()))
    {
        const usize tab = line.find('\t');
        if (tab == std::string::npos || tab + 1 >= line.size())
        {
            return qiven::Result<std::vector<TreeRow>, LockError>::fail(LockError::MalformedEntry);
        }
        const std::string meta = line.substr(0, tab);
        const std::string path = line.substr(tab + 1);
        // meta: "<mode> <type> <oid> <size>"
        std::istringstream meta_stream(meta);
        std::string mode;
        std::string type;
        std::string oid;
        unsigned long long size = 0;
        meta_stream >> mode >> type >> oid >> size;
        if (type != "blob" || oid.size() != 40)
        {
            continue; // submodule/gitlink rows are not corpus content
        }
        if (!meta_stream)
        {
            return qiven::Result<std::vector<TreeRow>, LockError>::fail(LockError::MalformedEntry);
        }
        rows.push_back(TreeRow { oid, path, static_cast<u64>(size) });
    }
    return qiven::Result<std::vector<TreeRow>, LockError>(std::move(rows));
}
} // namespace

std::string_view lock_error_text(LockError error) noexcept
{
    return text_of(error);
}

std::string SourceLock::canonical_json() const
{
    // Deterministic: entries pre-sorted by (repository, path); object
    // keys in fixed order; no insignificant whitespace; no timestamps.
    std::string out = "{\"schema\":\"qiven-source-lock-v1\",\"entries\":[";
    bool first      = true;
    for (const LockEntry& entry : entries)
    {
        if (!first)
        {
            out.push_back(',');
        }
        first = false;
        out += "{\"repository\":";
        json_escape(out, entry.repository);
        out += ",\"commit\":";
        json_escape(out, entry.commit);
        out += ",\"tree\":";
        json_escape(out, entry.tree_oid);
        out += ",\"path\":";
        json_escape(out, entry.path);
        out += ",\"sha256\":";
        json_escape(out, entry.sha256);
        out += ",\"size\":";
        out += std::to_string(entry.size_bytes);
        out.push_back('}');
    }
    out += "]}";
    return out;
}

std::string SourceLock::digest_hex() const
{
    const std::string json = canonical_json();
    qiven::SHA256Hasher hasher;
    hasher.update(reinterpret_cast<const std::byte*>(json.data()), json.size());
    const qiven::SHA256Digest digest = hasher.finalize();
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

qiven::Result<SourceLock, LockError> SourceLockBuilder::build(const SourceLockRequest& request) const
{
    if (request.runner == nullptr || request.git_executable.empty() || request.repositories.empty())
    {
        return LockOutcome::fail(LockError::GitUnavailable);
    }

    SourceLock lock;
    u64 total_bytes = 0;

    for (const LockRepository& repo : request.repositories)
    {
        if (repo.checkout.empty() || repo.ref.empty() || repo.path_filters.empty() ||
            repo.repository.empty())
        {
            return LockOutcome::fail(LockError::MalformedEntry);
        }
        const GitIo io { request.git_executable, repo.checkout, request.runner };

        // Exact source identity: the ref resolves to a commit AND that
        // commit is the checkout's HEAD (pinned-to-exact-revision law).
        auto commit = io.run({ "rev-parse", repo.ref + "^{commit}" });
        if (!commit.is_ok())
        {
            return LockOutcome::fail(LockError::RevisionUnresolved);
        }
        const std::string commit_oid = trim(commit.value());
        if (!is_hex_oid(commit_oid))
        {
            return LockOutcome::fail(LockError::RevisionUnresolved);
        }
        auto head = io.run({ "rev-parse", "HEAD" });
        if (!head.is_ok() || trim(head.value()) != commit_oid)
        {
            return LockOutcome::fail(LockError::RevisionUnresolved);
        }

        // Validated local: the working tree must be clean at that commit.
        auto status = io.run({ "status", "--porcelain" });
        if (!status.is_ok())
        {
            return LockOutcome::fail(LockError::GitUnavailable);
        }
        if (!trim(status.value()).empty())
        {
            return LockOutcome::fail(LockError::DirtyCheckout);
        }

        auto tree = io.run({ "rev-parse", commit_oid + "^{tree}" });
        if (!tree.is_ok())
        {
            return LockOutcome::fail(LockError::RevisionUnresolved);
        }
        const std::string tree_oid = trim(tree.value());
        if (!is_hex_oid(tree_oid))
        {
            return LockOutcome::fail(LockError::RevisionUnresolved);
        }

        auto rows = list_tree(io, tree_oid);
        if (!rows.is_ok())
        {
            return LockOutcome::fail(rows.reason());
        }
        for (const TreeRow& row : rows.value())
        {
            if (!selected_by(row.path, repo.path_filters))
            {
                continue;
            }
            if (row.size > max_locked_file_bytes || total_bytes + row.size > max_locked_total_bytes)
            {
                return LockOutcome::fail(LockError::BoundExceeded);
            }

            // Content sha256 over the exact blob (never the git oid).
            auto blob = io.run({ "cat-file", "blob", row.oid });
            if (!blob.is_ok())
            {
                return LockOutcome::fail(LockError::GitUnavailable);
            }
            if (blob.value().size() != row.size)
            {
                return LockOutcome::fail(LockError::MalformedEntry);
            }
            qiven::SHA256Hasher hasher;
            hasher.update(reinterpret_cast<const std::byte*>(blob.value().data()),
                          blob.value().size());
            const qiven::SHA256Digest digest = hasher.finalize();
            static constexpr char hex[]      = "0123456789abcdef";
            std::string content;
            content.resize(digest.size() * 2);
            for (usize i = 0; i < digest.size(); ++i)
            {
                const auto b       = static_cast<unsigned char>(digest[i]);
                content[2 * i]     = hex[b >> 4];
                content[2 * i + 1] = hex[b & 0x0Fu];
            }

            LockEntry entry;
            entry.repository = repo.repository;
            entry.commit     = commit_oid;
            entry.tree_oid   = tree_oid;
            entry.path       = row.path;
            entry.sha256     = content;
            entry.size_bytes = row.size;
            lock.entries.push_back(std::move(entry));
            total_bytes += row.size;
            if (lock.entries.size() > max_lock_entries)
            {
                return LockOutcome::fail(LockError::BoundExceeded);
            }
        }
    }

    // Canonical order + duplicate elimination (overlapping filters).
    std::sort(lock.entries.begin(), lock.entries.end(),
              [](const LockEntry& a, const LockEntry& b) {
                  if (a.repository != b.repository)
                  {
                      return a.repository < b.repository;
                  }
                  return a.path < b.path;
              });
    lock.entries.erase(std::unique(lock.entries.begin(), lock.entries.end(),
                                   [](const LockEntry& a, const LockEntry& b) {
                                       return a.repository == b.repository && a.path == b.path;
                                   }),
                       lock.entries.end());
    return LockOutcome(std::move(lock));
}
} // namespace qiven::runtime::cognition
