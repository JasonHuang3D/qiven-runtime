#include <qiven/runtime/observed_action.hpp>

namespace qiven::runtime
{
ObservedAction observe_action(AdapterInstanceId adapter,
                              HarnessSessionId session,
                              ActorInstanceId actor,
                              CapabilityId capability,
                              std::string operation,
                              std::string target,
                              std::span<const std::byte> arguments)
{
    ObservedAction action;
    action.adapter    = adapter;
    action.session    = session;
    action.actor      = actor;
    action.capability = capability;
    action.operation  = std::move(operation);
    action.target     = std::move(target);
    action.argument_blob.assign(arguments.begin(), arguments.end());
    action.argument_digest = ContentDigest { sha256(arguments.data(), arguments.size()) };
    return action;
}

CorrelationKey correlation_key(const RuntimeGenerationId& generation, const ObservedAction& action,
                               const HarnessActionId& harness_action) noexcept
{
    CorrelationKey key;
    key.generation = generation;
    key.adapter    = action.adapter;
    key.session    = action.session;
    key.actor      = action.actor;
    key.action     = harness_action;
    return key;
}
} // namespace qiven::runtime
