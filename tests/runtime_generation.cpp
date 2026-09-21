#include <qiven/runtime/generation.hpp>

#include <qiven/contracts.hpp>

#include <cstdio>
#include <type_traits>
#include <utility>

namespace
{
using qiven::u64;
using qiven::runtime::AdapterInstanceId;
using qiven::runtime::CapabilityId;
using qiven::runtime::GenerationMinter;
using qiven::runtime::ProfileRevision;

qiven::runtime::adapter::AdapterManifest loopback_manifest()
{
    using qiven::runtime::adapter::AdapterManifest;
    using qiven::runtime::adapter::CapabilityDescriptor;
    using qiven::runtime::adapter::OperationClass;

    AdapterManifest manifest;
    manifest.adapter_name     = "loopback";
    manifest.credential_token = 0xC0DE'FEEDull;

    qiven::runtime::adapter::CapabilityDescriptor observe {};
    observe.id              = CapabilityId { 1 };
    observe.adapter         = qiven::runtime::adapter::adapter_instance_id(manifest);
    observe.operation_class = OperationClass::Observe;

    manifest.capabilities.push_back(observe);
    return manifest;
}

qiven::runtime::profile::DeploymentProfile accepted_profile(u64 revision_value)
{
    using namespace qiven::runtime::profile;
    using qiven::runtime::adapter::OperationClass;

    GovernedActorSet actors;
    actors.revision = 1;
    ActorBinding loopback_actor {};
    loopback_actor.adapter          = qiven::runtime::adapter::adapter_instance_id(loopback_manifest());
    loopback_actor.session_token    = 1;
    loopback_actor.credential_token = 0xC0DE'FEEDull;
    actors.actors.push_back(loopback_actor);

    CapabilityUniverse universe;
    universe.revision = 1;
    qiven::runtime::adapter::CapabilityDescriptor observe {};
    observe.id              = CapabilityId { 1 };
    observe.adapter         = loopback_actor.adapter;
    observe.operation_class = OperationClass::Observe;
    universe.capabilities.push_back(observe);

    MediationInventory mediation;
    mediation.revision = 1;
    mediation.entries.push_back(MediationEntry { CapabilityId { 1 }, MediationKind::ActionInterception });

    auto built = ProfileBuilder {}
                     .set_name("loopback-acceptance")
                     .set_revision(ProfileRevision { revision_value })
                     .set_actor_set(std::move(actors))
                     .set_capability_universe(std::move(universe))
                     .set_mediation_inventory(std::move(mediation))
                     .set_claims(ClaimChannelCoverage { true, false })
                     .set_control_version(1)
                     .set_conformance_evidence("rca-1-local-gate")
                     .build();

    // the builder consumes the foundation Result vocabulary (OBL-D3F7B2)
    QIVEN_VERIFY(built.is_ok());
    return std::move(built).value();
}
} // namespace

// the generation carries the frozen draft Snapshot type by value (design §5)
static_assert(std::is_same_v<decltype(qiven::runtime::RuntimeGeneration {}.minimum_cognition), qiven::context::Snapshot>);

int main()
{
    // C-09: a profile change creates a NEW generation; the old generation
    // is never edited in place and its value snapshot is unaffected
    {
        GenerationMinter minter;

        auto profile_p5            = accepted_profile(5);
        const auto profile_p5_copy = profile_p5; // what G17 saw

        qiven::runtime::RuntimeGeneration g17 = minter.mint(std::move(profile_p5), { loopback_manifest() }, {});
        QIVEN_VERIFY(g17.id.value == 1);
        QIVEN_VERIFY(g17.profile.revision.value == 5);

        // change the profile -> new generation, same minter, next id
        auto profile_p6                       = accepted_profile(6);
        qiven::runtime::RuntimeGeneration g18 = minter.mint(std::move(profile_p6), { loopback_manifest() }, {});
        QIVEN_VERIFY(g18.id.value == 2);
        QIVEN_VERIFY(g18.profile.revision.value == 6);

        // G17 still binds exactly what it saw when minted
        QIVEN_VERIFY(g17.id.value == 1);
        QIVEN_VERIFY(g17.profile.revision.value == 5);
        QIVEN_VERIFY(g17.profile.revision.value == profile_p5_copy.revision.value);
        QIVEN_VERIFY(g17.admitted_adapters.size() == 1);
        QIVEN_VERIFY(g17.admitted_adapters[0].adapter_name == "loopback");

        // ids are monotonic and never repeat within the host lifetime
        qiven::runtime::RuntimeGeneration g19 = minter.mint(accepted_profile(7), {}, {});
        QIVEN_VERIFY(g19.id.value == 3);
        QIVEN_VERIFY(minter.last_id_value() == 3);
    }

    // a minted generation owns its binding: later mutation of the source
    // profile object does not reach the generation (value semantics)
    {
        GenerationMinter minter;
        auto profile                                 = accepted_profile(9);
        qiven::runtime::RuntimeGeneration generation = minter.mint(profile, {}, {});
        profile.name                                 = "mutated-after-mint";
        QIVEN_VERIFY(generation.profile.name == "loopback-acceptance");
        QIVEN_VERIFY(profile.name == "mutated-after-mint");
    }

    // the minter is non-copyable: one identity authority per host
    {
        static_assert(!std::is_copy_constructible_v<GenerationMinter>);
        static_assert(!std::is_copy_assignable_v<GenerationMinter>);
    }

    // manifest identity: identical content digests identically; any
    // surface change changes the identity a generation binds
    {
        using qiven::runtime::adapter::manifest_identity;

        const auto left = loopback_manifest();
        auto right      = loopback_manifest();
        QIVEN_VERIFY(manifest_identity(left) == manifest_identity(right));

        right.capabilities[0].can_mutate_world = true;
        QIVEN_VERIFY(manifest_identity(left) != manifest_identity(right));
    }

    // id rendering: kind-prefixed fixed-width hex
    {
        using qiven::runtime::IdKind;
        using qiven::runtime::render_id;
        QIVEN_VERIFY(render_id(IdKind::Generation, 0xDEADBEEFull) == "generation-00000000deadbeef");
        QIVEN_VERIFY(render_id(IdKind::Adapter, 1ull) == "adapter-0000000000000001");
    }

    // builder validation fails closed on incoherent accepted configuration
    {
        using namespace qiven::runtime::profile;
        using qiven::error_category;

        auto base = [] {
            return ProfileBuilder {}
                .set_name("validation")
                .set_revision(ProfileRevision { 2 })
                .set_conformance_evidence("evidence");
        };

        // duplicate capability ids are rejected
        {
            CapabilityUniverse universe;
            universe.capabilities.push_back({ CapabilityId { 7 }, {}, qiven::runtime::adapter::OperationClass::Observe, false, false, false });
            universe.capabilities.push_back({ CapabilityId { 7 }, {}, qiven::runtime::adapter::OperationClass::Observe, false, false, false });
            auto result = base().set_capability_universe(std::move(universe)).build();
            QIVEN_VERIFY(!result.is_ok());
            QIVEN_VERIFY(result.reason().category == error_category::invalid_argument);
            QIVEN_VERIFY(result.reason().code == 4);
        }

        // mediation referencing an undeclared capability is rejected
        {
            MediationInventory mediation;
            mediation.entries.push_back(MediationEntry { CapabilityId { 42 }, MediationKind::ActionInterception });
            auto result = base().set_mediation_inventory(std::move(mediation)).build();
            QIVEN_VERIFY(!result.is_ok());
            QIVEN_VERIFY(result.reason().code == 5);
        }

        // free-text claim coverage is rejected at this landing (C-23)
        {
            auto result = base().set_claims(ClaimChannelCoverage { true, true }).build();
            QIVEN_VERIFY(!result.is_ok());
            QIVEN_VERIFY(result.reason().code == 6);
        }

        // a profile without conformance evidence is rejected
        {
            auto result = ProfileBuilder {}.set_name("x").set_revision(ProfileRevision { 1 }).build();
            QIVEN_VERIFY(!result.is_ok());
            QIVEN_VERIFY(result.reason().code == 2);
        }
    }

    std::printf("[ OK ] runtime-generation\n");
    return 0;
}
