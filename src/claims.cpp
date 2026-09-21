#include <qiven/runtime/claims.hpp>

#include <algorithm>

namespace qiven::runtime
{
std::vector<ClassGovernanceReport> governance_claims(const profile::DeploymentProfile& profile)
{
    std::vector<ClassGovernanceReport> reports;

    const auto report_for = [&reports](adapter::OperationClass operation_class) -> ClassGovernanceReport& {
        for (ClassGovernanceReport& report : reports)
        {
            if (report.operation_class == operation_class)
            {
                return report;
            }
        }
        reports.push_back(ClassGovernanceReport { operation_class, ClassGovernance::FullyGoverned, {} });
        return reports.back();
    };

    for (const adapter::CapabilityDescriptor& capability : profile.capabilities.capabilities)
    {
        ClassGovernanceReport& report = report_for(capability.operation_class);

        const profile::MediationEntry* entry = nullptr;
        for (const profile::MediationEntry& candidate : profile.mediation.entries)
        {
            if (candidate.capability.value == capability.id.value)
            {
                entry = &candidate;
                break;
            }
        }

        const bool mediated = entry != nullptr && entry->kind != profile::MediationKind::Unmediated;
        if (!mediated)
        {
            report.governance = ClassGovernance::NotFullyGoverned;
            report.unmediated_producers.push_back(capability.id);
        }
    }

    return reports;
}
} // namespace qiven::runtime
