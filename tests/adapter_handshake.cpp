#include <qiven/runtime/adapter/handshake.hpp>

#include <qiven/contracts.hpp>

#include <cstdio>
#include <utility>

namespace
{
using qiven::u64;
using qiven::runtime::CapabilityId;
using qiven::runtime::ProfileRevision;
using qiven::runtime::adapter::CapabilityDescriptor;
using qiven::runtime::adapter::OperationClass;
using qiven::runtime::profile::ActorBinding;
using qiven::runtime::profile::CapabilityUniverse;
using qiven::runtime::profile::ClaimChannelCoverage;
using qiven::runtime::profile::DeploymentProfile;
using qiven::runtime::profile::GovernedActorSet;
using qiven::runtime::profile::MediationEntry;
using qiven::runtime::profile::MediationInventory;
using qiven::runtime::profile::MediationKind;
using qiven::runtime::profile::ProfileBuilder;

constexpr u64 loopback_credential = 0xC0DE'FEEDull;

// fnv1a64 over the stable adapter name — identical to
// adapter_instance_id(manifest) once the manifest exists (asserted below).
const qiven::runtime::AdapterInstanceId loopback_adapter { qiven::fnv1a64("loopback") };

CapabilityDescriptor observe_capability(u64 id, bool mutate = false)
{
    CapabilityDescriptor descriptor {};
    descriptor.id               = CapabilityId { id };
    descriptor.adapter          = loopback_adapter;
    descriptor.operation_class  = OperationClass::Observe;
    descriptor.can_mutate_world = mutate;
    return descriptor;
}

qiven::runtime::adapter::AdapterManifest base_manifest()
{
    qiven::runtime::adapter::AdapterManifest manifest;
    manifest.adapter_name     = "loopback";
    manifest.credential_token = loopback_credential;
    manifest.capabilities.push_back(observe_capability(1));
    return manifest;
}

DeploymentProfile accepted_profile()
{
    GovernedActorSet actors;
    actors.revision = 1;
    ActorBinding binding {};
    binding.adapter          = qiven::runtime::adapter::adapter_instance_id(base_manifest());
    binding.session_token    = 1;
    binding.credential_token = loopback_credential;
    actors.actors.push_back(binding);

    CapabilityUniverse universe;
    universe.revision = 1;
    universe.capabilities.push_back(observe_capability(1));

    MediationInventory mediation;
    mediation.revision = 1;
    mediation.entries.push_back(MediationEntry { CapabilityId { 1 }, MediationKind::ActionInterception });

    auto built = ProfileBuilder {}
                     .set_name("handshake-acceptance")
                     .set_revision(ProfileRevision { 3 })
                     .set_actor_set(std::move(actors))
                     .set_capability_universe(std::move(universe))
                     .set_mediation_inventory(std::move(mediation))
                     .set_claims(ClaimChannelCoverage { true, false })
                     .set_control_version(1)
                     .set_conformance_evidence("rca-1-local-gate")
                     .build();
    QIVEN_VERIFY(built.is_ok());
    return std::move(built).value();
}
} // namespace

int main()
{
    // the derived adapter identity agrees with the public helper
    QIVEN_VERIFY(qiven::runtime::adapter::adapter_instance_id(base_manifest()).fnv == loopback_adapter.fnv);

    // exact agreement admits the registration
    {
        const auto result = qiven::runtime::adapter::admit_against_profile(base_manifest(), accepted_profile());
        QIVEN_VERIFY(result.admitted);
        QIVEN_VERIFY(result.findings.empty());
        QIVEN_VERIFY(result.fail_closed_classes.empty());
    }

    // C-08: a new capability surface the profile does not represent fails
    // closed for its class; the host may not silently continue
    {
        auto manifest = base_manifest();
        CapabilityDescriptor rogue {};
        rogue.id               = CapabilityId { 99 };
        rogue.adapter          = qiven::runtime::adapter::adapter_instance_id(manifest);
        rogue.operation_class  = OperationClass::ProcessLaunch;
        rogue.can_mutate_world = true;
        manifest.capabilities.push_back(rogue);

        const auto result = qiven::runtime::adapter::admit_against_profile(manifest, accepted_profile());
        QIVEN_VERIFY(!result.admitted);
        QIVEN_VERIFY(result.findings.size() == 1);
        QIVEN_VERIFY(result.findings[0].issue == qiven::runtime::adapter::HandshakeIssue::UnrepresentedInProfile);
        QIVEN_VERIFY(result.fail_closed_classes.size() == 1);
        QIVEN_VERIFY(result.fail_closed_classes[0] == OperationClass::ProcessLaunch);
    }

    // C-08: same capability id with a changed surface (a mutation path
    // appeared) fails closed even though the id was declared
    {
        auto manifest                             = base_manifest();
        manifest.capabilities[0].can_mutate_world = true;

        const auto result = qiven::runtime::adapter::admit_against_profile(manifest, accepted_profile());
        QIVEN_VERIFY(!result.admitted);
        QIVEN_VERIFY(result.findings[0].issue == qiven::runtime::adapter::HandshakeIssue::SurfaceChanged);
        QIVEN_VERIFY(result.fail_closed_classes[0] == OperationClass::Observe);
    }

    // C-08 + ADL §10 "must agree": a profile-declared capability the
    // manifest no longer carries is disagreement, not a graceful shrink
    {
        auto manifest = base_manifest();
        manifest.capabilities.clear();

        const auto result = qiven::runtime::adapter::admit_against_profile(manifest, accepted_profile());
        QIVEN_VERIFY(!result.admitted);
        QIVEN_VERIFY(result.findings[0].issue == qiven::runtime::adapter::HandshakeIssue::AbsentFromManifest);
        QIVEN_VERIFY(result.fail_closed_classes[0] == OperationClass::Observe);
    }

    // an unknown adapter or credential rejects the whole registration;
    // every class the manifest exposes fails closed (identity comes only
    // from accepted bindings, ADL §8)
    {
        auto stranger             = base_manifest();
        stranger.credential_token = 0x1234ull;
        stranger.capabilities.push_back(observe_capability(2));

        const auto result = qiven::runtime::adapter::admit_against_profile(stranger, accepted_profile());
        QIVEN_VERIFY(!result.admitted);
        QIVEN_VERIFY(result.findings.size() == 1);
        QIVEN_VERIFY(result.findings[0].issue == qiven::runtime::adapter::HandshakeIssue::UnknownAdapterCredential);
        QIVEN_VERIFY(result.fail_closed_classes.size() == 1); // deduplicated Observe
        QIVEN_VERIFY(result.fail_closed_classes[0] == OperationClass::Observe);
    }

    // a manifest vouching for another adapter's binding is new surface
    {
        auto manifest                    = base_manifest();
        manifest.capabilities[0].adapter = qiven::runtime::AdapterInstanceId { 0xBADull };

        const auto result = qiven::runtime::adapter::admit_against_profile(manifest, accepted_profile());
        QIVEN_VERIFY(!result.admitted);
        QIVEN_VERIFY(result.findings[0].issue == qiven::runtime::adapter::HandshakeIssue::UnrepresentedInProfile);
    }

    std::printf("[ OK ] adapter-handshake\n");
    return 0;
}
