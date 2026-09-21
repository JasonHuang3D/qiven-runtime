#pragma once

// ============================================================================
// generation.hpp — RuntimeGeneration: the immutable configuration epoch a
// host operates under (component ADL §6, C-09)
//
// A generation binds every revision the control plane must agree on:
// profile revision (carrying actor-set, capability-universe and mediation
// revisions), adapter manifest identities, resolver-registry and classifier
// revisions, control version, and the pinned minimum cognition floor.
// A profile or resolver change NEVER edits an active generation in place —
// it creates a new generation, and unconsumed decisions from the old
// generation become stale (ADL §6; staleness enforcement lands with
// DecisionBinding, RCA-7).
//
// The minimum cognition floor is the frozen draft Snapshot value
// (qiven::context::Snapshot); the runtime never redefines its semantics —
// the draft is consumed as a frozen semantic library (layer contract §2).
// ============================================================================

#include <qiven/runtime/adapter/manifest.hpp>
#include <qiven/runtime/identity.hpp>
#include <qiven/runtime/profile.hpp>

#include <qiven/context/cognition.hpp>

#include <vector>

namespace qiven::runtime
{
struct RuntimeGeneration
{
    RuntimeGenerationId id {};
    profile::DeploymentProfile profile;

    // Manifest identities admitted into this generation (C-08: the
    // handshake ran against this profile before minting).
    std::vector<adapter::AdapterManifest> admitted_adapters;

    // Pinned cognition floor at startup (design §5); revision-level
    // invalidation semantics land with PinnedCognition (RCA-3).
    qiven::context::Snapshot minimum_cognition;
};

// Per-host monotonic generation minter. Minting is the ONLY way a
// generation comes to exist; ids never repeat within one host lifetime.
class GenerationMinter
{
public:
    GenerationMinter() = default;

    GenerationMinter(const GenerationMinter&)            = delete;
    GenerationMinter& operator=(const GenerationMinter&) = delete;

    // Mint a generation binding the given profile and the manifests that
    // were admitted against it. Admitted manifests are copied by value:
    // the generation owns its binding and is immutable once returned.
    [[nodiscard]] RuntimeGeneration mint(profile::DeploymentProfile profile,
                                         std::vector<adapter::AdapterManifest> admitted_adapters,
                                         qiven::context::Snapshot minimum_cognition);

    [[nodiscard]] u64 last_id_value() const noexcept
    {
        return m_next_value - 1;
    }

private:
    u64 m_next_value = 1;
};
} // namespace qiven::runtime
