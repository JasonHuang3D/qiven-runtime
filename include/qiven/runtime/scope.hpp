#pragma once

// ============================================================================
// scope.hpp — ResourceScope: the explicit governed-resource coverage of a
// deployment profile (production-MVP architecture §3.2/§3.3, MVP-0)
//
// A hard enforcement claim is scoped to a tuple the runtime actually
// mediates: (Actor, OperationClass, ResourceScope). The scope is an
// EXPLICIT enumeration of governed paths — normalized Git-relative paths
// and/or normalized absolute roots — never an unconstrained disk-wide
// claim and never an ambiguous glob (§3.2). An action target outside the
// enumeration is NotCovered: the runtime returns NotGoverned rather than
// overstating its authority (§3.3, invariant 14).
// ============================================================================

#include <qiven/result.hpp>
#include <qiven/runtime/identity.hpp>
#include <qiven/types.hpp>

#include <string>
#include <vector>

namespace qiven::runtime
{
enum class ScopeCoverage : u8
{
    Covered,    // inside the explicit governed enumeration
    NotCovered, // outside: the runtime does not claim authority here
};

class ResourceScope
{
public:
    ResourceScope() = default;

    // The ONLY construction path: explicit paths, no globs. Normalization:
    // forward slashes, no trailing slash, no "..", no empty, no duplicates.
    // Fails closed (typed Result) on any ambiguous entry — an ambiguous
    // scope is a construction defect, never a silent partial claim.
    class Builder;

    [[nodiscard]] ScopeCoverage covers(const std::string& target) const noexcept;

    [[nodiscard]] const std::vector<std::string>& governed_paths() const noexcept
    {
        return m_paths;
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return m_paths.empty();
    }

    // Content digest over the sorted explicit enumeration: the value a
    // decision binds as resource_scope_digest (production-MVP §4
    // invariant 8; StaleScope freshness axis).
    [[nodiscard]] ContentDigest digest() const;

private:
    explicit ResourceScope(std::vector<std::string> paths) :
    m_paths(std::move(paths))
    {
    }

    std::vector<std::string> m_paths; // sorted, normalized, duplicate-free
};

class ResourceScope::Builder
{
public:
    // One governed path: Git-relative ("state/active-work.yaml") or
    // absolute root. A directory path governs everything strictly below it
    // by path components (never by string prefix: "state/current.md.md"
    // is not under "state/current.md").
    Builder& add_path(std::string path);

    [[nodiscard]] qiven::Result<ResourceScope> build();

private:
    std::vector<std::string> m_pending;
};

// Normalized directory-contains check: `root` governs `target` when root
// is a directory component-prefix of target, or equals it.
[[nodiscard]] bool path_contains(const std::string& root, const std::string& target) noexcept;
} // namespace qiven::runtime
