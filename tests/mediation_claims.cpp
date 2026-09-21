#include <qiven/runtime/claims.hpp>

#include <qiven/contracts.hpp>

#include <cstdio>

namespace
{
using qiven::u64;
using qiven::runtime::CapabilityId;
using qiven::runtime::ClassGovernance;
using qiven::runtime::adapter::CapabilityDescriptor;
using qiven::runtime::adapter::OperationClass;
using qiven::runtime::profile::CapabilityUniverse;
using qiven::runtime::profile::DeploymentProfile;
using qiven::runtime::profile::MediationEntry;
using qiven::runtime::profile::MediationInventory;
using qiven::runtime::profile::MediationKind;

// const, not constexpr: fnv1a64(string_view) is runtime-only by law (the
// string_view overload reinterprets bytes and cannot be constant-evaluated)
const qiven::runtime::AdapterInstanceId adapter { qiven::fnv1a64("loopback") };

CapabilityDescriptor capability(u64 id, OperationClass operation_class)
{
    CapabilityDescriptor descriptor {};
    descriptor.id              = CapabilityId { id };
    descriptor.adapter         = adapter;
    descriptor.operation_class = operation_class;
    return descriptor;
}

DeploymentProfile profile_with(CapabilityUniverse universe, MediationInventory mediation)
{
    DeploymentProfile profile;
    profile.name         = "claims-analysis";
    profile.capabilities = std::move(universe);
    profile.mediation    = std::move(mediation);
    return profile;
}

const qiven::runtime::ClassGovernanceReport* find_report(const std::vector<qiven::runtime::ClassGovernanceReport>& reports,
                                                         OperationClass operation_class)
{
    for (const auto& report : reports)
    {
        if (report.operation_class == operation_class)
        {
            return &report;
        }
    }
    return nullptr;
}
} // namespace

int main()
{
    // C-07: a class whose producers are all mediated carries the
    // hard-enforcement claim
    {
        CapabilityUniverse universe;
        universe.capabilities.push_back(capability(1, OperationClass::Observe));
        universe.capabilities.push_back(capability(2, OperationClass::Observe));

        MediationInventory mediation;
        mediation.entries.push_back(MediationEntry { CapabilityId { 1 }, MediationKind::ActionInterception });
        mediation.entries.push_back(MediationEntry { CapabilityId { 2 }, MediationKind::ClaimChannelOnly });

        const auto reports = qiven::runtime::governance_claims(profile_with(std::move(universe), std::move(mediation)));
        QIVEN_VERIFY(reports.size() == 1);
        QIVEN_VERIFY(reports[0].governance == ClassGovernance::FullyGoverned);
        QIVEN_VERIFY(reports[0].unmediated_producers.empty());
    }

    // C-07: one unmediated producer demotes the whole class, and the
    // offender is named — the gap is explicit, never silent
    {
        CapabilityUniverse universe;
        universe.capabilities.push_back(capability(1, OperationClass::Observe));
        universe.capabilities.push_back(capability(2, OperationClass::Observe));
        universe.capabilities.push_back(capability(3, OperationClass::FileSystemWrite));

        MediationInventory mediation;
        mediation.entries.push_back(MediationEntry { CapabilityId { 1 }, MediationKind::ActionInterception });
        // capability 2 has NO mediation entry at all
        mediation.entries.push_back(MediationEntry { CapabilityId { 3 }, MediationKind::ActionInterception });

        const auto reports = qiven::runtime::governance_claims(profile_with(std::move(universe), std::move(mediation)));
        QIVEN_VERIFY(reports.size() == 2);

        const auto* observe = find_report(reports, OperationClass::Observe);
        QIVEN_VERIFY(observe != nullptr);
        QIVEN_VERIFY(observe->governance == ClassGovernance::NotFullyGoverned);
        QIVEN_VERIFY(observe->unmediated_producers.size() == 1);
        QIVEN_VERIFY(observe->unmediated_producers[0].value == 2);

        const auto* write = find_report(reports, OperationClass::FileSystemWrite);
        QIVEN_VERIFY(write != nullptr);
        QIVEN_VERIFY(write->governance == ClassGovernance::FullyGoverned);
    }

    // an explicit Unmediated entry is a declared gap, not coverage
    {
        CapabilityUniverse universe;
        universe.capabilities.push_back(capability(1, OperationClass::ProcessLaunch));

        MediationInventory mediation;
        mediation.entries.push_back(MediationEntry { CapabilityId { 1 }, MediationKind::Unmediated });

        const auto reports = qiven::runtime::governance_claims(profile_with(std::move(universe), std::move(mediation)));
        QIVEN_VERIFY(reports.size() == 1);
        QIVEN_VERIFY(reports[0].governance == ClassGovernance::NotFullyGoverned);
        QIVEN_VERIFY(reports[0].unmediated_producers.size() == 1);
    }

    // classes absent from the declared universe make no claim at all —
    // the universe is closed (ADL §9)
    {
        CapabilityUniverse universe;
        universe.capabilities.push_back(capability(1, OperationClass::Observe));

        MediationInventory mediation;
        mediation.entries.push_back(MediationEntry { CapabilityId { 1 }, MediationKind::ActionInterception });

        const auto reports = qiven::runtime::governance_claims(profile_with(std::move(universe), std::move(mediation)));
        QIVEN_VERIFY(reports.size() == 1);
        QIVEN_VERIFY(find_report(reports, OperationClass::NetworkAccess) == nullptr);
    }

    std::printf("[ OK ] mediation-claims\n");
    return 0;
}
