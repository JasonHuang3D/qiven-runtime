#include <qiven/runtime/adapter/handshake.hpp>

#include <algorithm>

namespace qiven::runtime::adapter
{
namespace
{
void add_fail_closed(std::vector<OperationClass>& classes, OperationClass added)
{
    if (std::find(classes.begin(), classes.end(), added) == classes.end())
    {
        classes.push_back(added);
    }
}
} // namespace

HandshakeResult admit_against_profile(const AdapterManifest& manifest, const profile::DeploymentProfile& accepted)
{
    HandshakeResult result;

    // Credential gate: the adapter must be an accepted binding carrying
    // this credential. An unknown adapter or credential is a whole-
    // registration rejection, not a per-class finding (ADL §8: identity
    // originates from accepted bindings only).
    const AdapterInstanceId instance = adapter_instance_id(manifest);
    bool credential_accepted         = false;
    for (const profile::ActorBinding& actor : accepted.actors.actors)
    {
        if (actor.adapter.fnv == instance.fnv && actor.credential_token == manifest.credential_token)
        {
            credential_accepted = true;
            break;
        }
    }
    if (!credential_accepted)
    {
        result.findings.push_back(HandshakeFinding { HandshakeIssue::UnknownAdapterCredential, CapabilityDescriptor {} });
        // Without an accepted binding no claim about this adapter holds at
        // all: every class its manifest exposes fails closed.
        for (const CapabilityDescriptor& capability : manifest.capabilities)
        {
            add_fail_closed(result.fail_closed_classes, capability.operation_class);
        }
        return result;
    }

    // Capability comparison in both directions against the profile's
    // declared surface FOR THIS ADAPTER (ADL §10: declared universe +
    // adapter manifest must agree).
    for (const CapabilityDescriptor& presented : manifest.capabilities)
    {
        if (presented.adapter.fnv != instance.fnv)
        {
            // A manifest vouching for another adapter's capability id is
            // new surface by construction.
            result.findings.push_back(HandshakeFinding { HandshakeIssue::UnrepresentedInProfile, presented });
            add_fail_closed(result.fail_closed_classes, presented.operation_class);
            continue;
        }

        const CapabilityDescriptor* declared = nullptr;
        for (const CapabilityDescriptor& candidate : accepted.capabilities.capabilities)
        {
            if (candidate.id.value == presented.id.value && candidate.adapter.fnv == instance.fnv)
            {
                declared = &candidate;
                break;
            }
        }

        if (declared == nullptr)
        {
            result.findings.push_back(HandshakeFinding { HandshakeIssue::UnrepresentedInProfile, presented });
            add_fail_closed(result.fail_closed_classes, presented.operation_class);
        }
        else if (!declared->same_surface(presented))
        {
            result.findings.push_back(HandshakeFinding { HandshakeIssue::SurfaceChanged, presented });
            add_fail_closed(result.fail_closed_classes, presented.operation_class);
        }
    }

    for (const CapabilityDescriptor& declared : accepted.capabilities.capabilities)
    {
        if (declared.adapter.fnv != instance.fnv)
        {
            continue;
        }
        const CapabilityDescriptor* presented = nullptr;
        for (const CapabilityDescriptor& candidate : manifest.capabilities)
        {
            if (candidate.id.value == declared.id.value)
            {
                presented = &candidate;
                break;
            }
        }
        if (presented == nullptr)
        {
            result.findings.push_back(HandshakeFinding { HandshakeIssue::AbsentFromManifest, declared });
            add_fail_closed(result.fail_closed_classes, declared.operation_class);
        }
    }

    result.admitted = result.findings.empty();
    return result;
}
} // namespace qiven::runtime::adapter
