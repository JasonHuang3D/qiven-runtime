#include <qiven/runtime/requirement.hpp>

namespace qiven::runtime
{
const RequirementInstance* RequirementSet::find(const RequirementIdentity& identity) const noexcept
{
    for (const RequirementInstance& instance : m_instances)
    {
        if (instance.identity == identity)
        {
            return &instance;
        }
    }
    return nullptr;
}

void RequirementSet::merge(RequirementInstance instance)
{
    for (RequirementInstance& existing : m_instances)
    {
        if (existing.identity == instance.identity)
        {
            // provenance merge: the requirement stays ONE instance and
            // remembers every intent that produced it (ADL §26)
            for (const u32 origin : instance.origin_intents)
            {
                bool known = false;
                for (const u32 existing_origin : existing.origin_intents)
                {
                    if (existing_origin == origin)
                    {
                        known = true;
                        break;
                    }
                }
                if (!known)
                {
                    existing.origin_intents.push_back(origin);
                }
            }
            return;
        }
    }
    m_instances.push_back(std::move(instance));
}

RequirementSet derive_requirement_set(const qiven::context::Snapshot& snapshot, const IntentSet& intents)
{
    RequirementSet set;
    const std::span<const IntentClassification> members = intents.members();

    for (u32 index = 0; index < members.size(); ++index)
    {
        // the frozen v4 semantics own derivation; the runtime only unions
        // with provenance
        const std::vector<qiven::context::PreparedRequirement> derived =
            qiven::context::derive_requirements(snapshot, members[index].intent);

        for (const qiven::context::PreparedRequirement& prepared : derived)
        {
            RequirementInstance instance;
            instance.identity.kind     = prepared.requirement.kind;
            instance.identity.subject  = prepared.requirement.subject;
            instance.identity.boundary = prepared.boundary;
            instance.identity.blocking = prepared.requirement.blocking;
            instance.spec              = prepared.requirement;
            instance.origin_intents.push_back(index);
            set.merge(std::move(instance));
        }
    }
    return set;
}
} // namespace qiven::runtime
