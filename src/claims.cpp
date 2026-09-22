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

ScopeGovernanceReport scope_governance(const profile::DeploymentProfile& profile,
                                       const ResourceScope& scope,
                                       const std::string& target)
{
    ScopeGovernanceReport report;
    report.scope_declared  = !scope.empty();
    report.target_coverage = scope.covers(target);

    // write-class mediation: every FileSystemWrite producer in the
    // universe must carry a real interception path for a hard claim on
    // the scope (§3.3)
    report.write_classes_mediated                    = true;
    const std::vector<ClassGovernanceReport> classes = governance_claims(profile);
    for (const ClassGovernanceReport& class_report : classes)
    {
        if (class_report.operation_class == adapter::OperationClass::FileSystemWrite &&
            class_report.governance != ClassGovernance::FullyGoverned)
        {
            report.write_classes_mediated = false;
        }
    }
    return report;
}
} // namespace qiven::runtime
