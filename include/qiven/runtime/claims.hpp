#pragma once

// ============================================================================
// claims.hpp — complete-mediation claim analysis (component ADL §9/§10,
// C-07)
//
// Hard enforcement is claimed ONLY for operation classes whose producing
// capabilities are all mediated within the declared CapabilityUniverse.
// This pure function computes, from an accepted profile alone, which
// classes carry a valid hard-enforcement claim and which do not — the
// honest claim surface the runtime may state. A class with any unmediated
// producer is reported NotFullyGoverned with the offending producers
// named; it is never silently claimed.
// ============================================================================

#include <qiven/runtime/adapter/manifest.hpp>
#include <qiven/runtime/profile.hpp>

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
} // namespace qiven::runtime
