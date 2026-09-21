#include <qiven/runtime/port/interception.hpp>

#include <qiven/hashing.hpp>

namespace qiven::runtime::port
{
qiven::Result<ObservedAction> materialize_proposal(const InterceptedProposal& proposal)
{
    using qiven::Error;
    using qiven::error_category;

    // §14: the surface must be COMPLETE — the control plane never guesses
    // a missing identity field
    if (proposal.adapter_name.empty() || proposal.operation.empty() || proposal.target.empty() ||
        proposal.session_token == 0 || proposal.actor_token == 0 || proposal.capability_id == 0 ||
        proposal.harness_action == 0)
    {
        return qiven::Result<ObservedAction>::fail(
            Error::make(error_category::invalid_argument, 50, "incomplete interception surface (ADL 14)"));
    }

    const AdapterInstanceId adapter { fnv1a64(proposal.adapter_name) };
    ObservedAction action = observe_action(adapter, HarnessSessionId { proposal.session_token },
                                           ActorInstanceId { proposal.actor_token },
                                           CapabilityId { proposal.capability_id }, proposal.operation,
                                           proposal.target, proposal.arguments);
    // the harness correlation identity rides with the observation's
    // correlation key at transaction begin; the observation itself is
    // immutable for the control attempt (§14)
    return qiven::Result<ObservedAction>(std::move(action));
}
} // namespace qiven::runtime::port
