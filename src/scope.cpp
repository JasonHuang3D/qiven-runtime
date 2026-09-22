#include <qiven/runtime/scope.hpp>

#include <algorithm>

namespace qiven::runtime
{
namespace
{
// Normalization: backslashes to forward slashes, no trailing slash, no
// "." or ".." components, non-empty. Returns false for ambiguous input.
bool normalize_path(std::string& path)
{
    if (path.empty())
    {
        return false;
    }
    std::replace(path.begin(), path.end(), '\\', '/');
    if (path.find("*") != std::string::npos || path.find("?") != std::string::npos)
    {
        return false; // globs are forbidden: the enumeration is explicit (§3.2)
    }
    while (path.size() > 1 && path.back() == '/')
    {
        path.pop_back();
    }
    if (path.empty() || path == "." || path == "..")
    {
        return false;
    }
    usize start = 0;
    while (start < path.size())
    {
        const usize slash = path.find('/', start);
        const usize end   = slash == std::string::npos ? path.size() : slash;
        if (end == start || (end - start == 1 && path[start] == '.') ||
            (end - start == 2 && path[start] == '.' && path[start + 1] == '.'))
        {
            return false; // empty / dot / parent component: ambiguous
        }
        if (slash == std::string::npos)
        {
            break;
        }
        start = slash + 1;
    }
    return true;
}
} // namespace

bool path_contains(const std::string& root, const std::string& target) noexcept
{
    if (root == target)
    {
        return true;
    }
    if (target.size() <= root.size())
    {
        return false;
    }
    return target.compare(0, root.size(), root) == 0 && target[root.size()] == '/';
}

ResourceScope::Builder& ResourceScope::Builder::add_path(std::string path)
{
    m_pending.push_back(std::move(path));
    return *this;
}

qiven::Result<ResourceScope> ResourceScope::Builder::build()
{
    using qiven::Error;
    using qiven::error_category;

    for (std::string& path : m_pending)
    {
        if (!normalize_path(path))
        {
            return qiven::Result<ResourceScope>::fail(
                Error::make(error_category::invalid_argument, 40,
                            "governed scope entries must be explicit normalized paths without globs"));
        }
    }
    std::sort(m_pending.begin(), m_pending.end());
    for (usize i = 1; i < m_pending.size(); ++i)
    {
        if (m_pending[i] == m_pending[i - 1])
        {
            return qiven::Result<ResourceScope>::fail(
                Error::make(error_category::invalid_argument, 41, "duplicate governed scope entry"));
        }
    }
    return qiven::Result<ResourceScope>(ResourceScope { std::move(m_pending) });
}

ScopeCoverage ResourceScope::covers(const std::string& target) const noexcept
{
    std::string normalized = target;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    for (const std::string& path : m_paths)
    {
        if (path_contains(path, normalized))
        {
            return ScopeCoverage::Covered;
        }
    }
    return ScopeCoverage::NotCovered;
}

ContentDigest ResourceScope::digest() const
{
    // canonical little-endian preimage over the sorted explicit paths
    std::vector<std::byte> preimage;
    const auto put_u64 = [&preimage](const u64 value) {
        for (int i = 0; i < 8; ++i)
        {
            preimage.push_back(static_cast<std::byte>(value >> (8 * i)));
        }
    };
    put_u64(m_paths.size());
    for (const std::string& path : m_paths)
    {
        put_u64(path.size());
        for (const char ch : path)
        {
            preimage.push_back(static_cast<std::byte>(ch));
        }
    }
    return ContentDigest { sha256(preimage.data(), preimage.size()) };
}
} // namespace qiven::runtime
