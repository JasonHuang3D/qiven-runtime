#include <qiven/runtime/adapter/loopback.hpp>

namespace qiven::runtime::adapter
{
LoopbackAdapter::LoopbackAdapter(std::string name, u64 credential_token)
{
    m_manifest.adapter_name     = std::move(name);
    m_manifest.manifest_version = 1;
    m_manifest.credential_token = credential_token;

    CapabilityDescriptor observe {};
    observe.id              = CapabilityId { 1 };
    observe.adapter         = adapter_instance_id(m_manifest);
    observe.operation_class = OperationClass::Observe;
    m_manifest.capabilities.push_back(observe);

    m_window = "in-process loopback: synchronous submit-to-observation, no transport window";
}

qiven::Result<ObservedAction> LoopbackAdapter::submit_proposal(const port::InterceptedProposal& proposal)
{
    auto action = materialize_proposal(proposal);
    if (action.is_ok())
    {
        // the adapter identity in the materialized observation must be the
        // ADAPTER'S OWN declared identity (§14 adapter identity) — a
        // proposal claiming another adapter's name is a defect
        if (action.value().adapter.fnv != adapter_instance_id(m_manifest).fnv)
        {
            return qiven::Result<ObservedAction>::fail(
                qiven::Error::make(qiven::error_category::invalid_argument, 51,
                                   "proposal adapter identity disagrees with the adapter's manifest"));
        }
    }
    return action;
}

u64 LoopbackAdapter::notify_activation(const port::ActivationEvent& event)
{
    static_cast<void>(event); // activation is preparation only (§13);
                              // correctness never depends on it
    ++m_activations;
    return m_next_injection_event++;
}

port::ClaimChannelResult LoopbackAdapter::route_claim(const port::OutboundClaim& claim)
{
    if (claim.channel == port::ClaimChannel::ToolMediated)
    {
        return port::ClaimChannelResult::Governed; // through interception,
                                                   // the ordinary path
    }
    return port::ClaimChannelResult::NotGoverned; // §67: no silent Allow
}

bool LoopbackAdapter::observation_recorded(u64 harness_action) const noexcept
{
    return m_observed.contains(harness_action);
}

void LoopbackAdapter::record_observation(u64 harness_action)
{
    m_observed.insert(harness_action);
}
} // namespace qiven::runtime::adapter
