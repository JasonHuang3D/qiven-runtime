#pragma once

// ============================================================================
// claims.hpp — complete-mediation claim analysis (component ADL §9/§10,
// C-07; production-MVP architecture §3.2/§3.3, MVP-0)
//
// Hard enforcement is claimed ONLY for operation classes whose producing
// capabilities are all mediated within the declared CapabilityUniverse.
// This pure function computes, from an accepted profile alone, which
// classes carry a valid hard-enforcement claim and which do not — the
// honest claim surface the runtime may state. A class with any unmediated
// producer is reported NotFullyGoverned with the offending producers
// named; it is never silently claimed.
//
// MVP-0 adds the resource-scope dimension (§3.3): a hard claim holds for
// the tuple (Actor, OperationClass, ResourceScope) actually mediated. The
// scope report names the coverage honestly — an empty scope claims
// nothing, and targets outside the enumeration are NotCovered
// (NotGoverned), never silently governed.
// ============================================================================

#include <qiven/runtime/adapter/manifest.hpp>
#include <qiven/runtime/profile.hpp>
#include <qiven/runtime/scope.hpp>

#include <vector>

namespace qiven::runtime
{
enum class ClassGovernance : u8
{
    FullyGoverned,
    NotFullyGoverned,
};

struct ClassGovernanceReport
{
    adapter::OperationClass operation_class = adapter::OperationClass::Observe;
    ClassGovernance governance              = ClassGovernance::NotFullyGoverned;

    // Capabilities of this class without a real interception path; empty
    // exactly when governance == FullyGoverned.
    std::vector<CapabilityId> unmediated_producers;
};

// Compute the governance claim for every operation class present in the
// profile's capability universe.
[[nodiscard]] std::vector<ClassGovernanceReport> governance_claims(const profile::DeploymentProfile& profile);

// Resource-scope dimension of the claim (§3.3): whether the mediated
// operation classes and the explicit governed enumeration together
// support a hard write-mediation claim, and what the enumeration covers
// for one concrete action target.
struct ScopeGovernanceReport
{
    bool scope_declared           = false;                     // a non-empty explicit enumeration exists
    bool write_classes_mediated   = false;                     // every FileSystemWrite-class producer is mediated
    ScopeCoverage target_coverage = ScopeCoverage::NotCovered; // for the queried target
};

[[nodiscard]] ScopeGovernanceReport scope_governance(const profile::DeploymentProfile& profile,
                                                     const ResourceScope& scope,
                                                     const std::string& target);
} // namespace qiven::runtime
