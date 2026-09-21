#include <qiven/runtime/profile.hpp>

#include <algorithm>

namespace qiven::runtime::profile
{
ProfileBuilder& ProfileBuilder::set_name(std::string name)
{
    m_partial.name = std::move(name);
    return *this;
}

ProfileBuilder& ProfileBuilder::set_revision(ProfileRevision revision)
{
    m_partial.revision = revision;
    return *this;
}

ProfileBuilder& ProfileBuilder::set_actor_set(GovernedActorSet actors)
{
    m_partial.actors = std::move(actors);
    return *this;
}

ProfileBuilder& ProfileBuilder::set_capability_universe(CapabilityUniverse capabilities)
{
    m_partial.capabilities = std::move(capabilities);
    return *this;
}

ProfileBuilder& ProfileBuilder::set_mediation_inventory(MediationInventory mediation)
{
    m_partial.mediation = std::move(mediation);
    return *this;
}

ProfileBuilder& ProfileBuilder::set_claims(ClaimChannelCoverage claims)
{
    m_partial.claims = claims;
    return *this;
}

ProfileBuilder& ProfileBuilder::set_control_version(u64 version)
{
    m_partial.control_version = version;
    return *this;
}

ProfileBuilder& ProfileBuilder::set_conformance_evidence(std::string reference)
{
    m_partial.conformance_evidence = std::move(reference);
    return *this;
}

qiven::Result<DeploymentProfile> ProfileBuilder::build()
{
    using qiven::Error;
    using qiven::error_category;

    if (m_partial.name.empty() || m_partial.revision.value == 0)
    {
        return qiven::Result<DeploymentProfile>::fail(
            Error::make(error_category::invalid_argument, 1, "profile requires a name and a non-zero revision"));
    }
    if (m_partial.conformance_evidence.empty())
    {
        return qiven::Result<DeploymentProfile>::fail(
            Error::make(error_category::invalid_argument, 2, "an accepted profile must cite conformance evidence"));
    }

    for (const ActorBinding& actor : m_partial.actors.actors)
    {
        const usize same = static_cast<usize>(std::count_if(m_partial.actors.actors.begin(),
                                                            m_partial.actors.actors.end(),
                                                            [&actor](const ActorBinding& other) {
                                                                return other.adapter.fnv == actor.adapter.fnv &&
                                                                       other.session_token == actor.session_token;
                                                            }));
        if (same != 1)
        {
            return qiven::Result<DeploymentProfile>::fail(
                Error::make(error_category::invalid_argument, 3, "duplicate actor binding in governed actor set"));
        }
    }

    for (const adapter::CapabilityDescriptor& capability : m_partial.capabilities.capabilities)
    {
        const usize same = static_cast<usize>(std::count_if(m_partial.capabilities.capabilities.begin(),
                                                            m_partial.capabilities.capabilities.end(),
                                                            [&capability](const adapter::CapabilityDescriptor& other) {
                                                                return other.id.value == capability.id.value;
                                                            }));
        if (same != 1)
        {
            return qiven::Result<DeploymentProfile>::fail(
                Error::make(error_category::invalid_argument, 4, "duplicate capability id in the universe"));
        }
    }

    for (const MediationEntry& entry : m_partial.mediation.entries)
    {
        const bool declared = std::any_of(m_partial.capabilities.capabilities.begin(),
                                          m_partial.capabilities.capabilities.end(),
                                          [&entry](const adapter::CapabilityDescriptor& capability) {
                                              return capability.id.value == entry.capability.value;
                                          });
        if (!declared)
        {
            return qiven::Result<DeploymentProfile>::fail(
                Error::make(error_category::invalid_argument, 5, "mediation entry references an undeclared capability"));
        }
    }

    if (m_partial.claims.free_text)
    {
        return qiven::Result<DeploymentProfile>::fail(Error::make(
            error_category::invalid_argument, 6, "the first landing claims tool-mediated channels only (C-23)"));
    }

    return qiven::Result<DeploymentProfile>(std::move(m_partial));
}
} // namespace qiven::runtime::profile
