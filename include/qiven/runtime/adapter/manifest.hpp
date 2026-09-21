#pragma once

// ============================================================================
// adapter/manifest.hpp — the actual capability surface a harness adapter
// presents at RuntimeHost startup (component ADL §11/§12)
//
// The manifest is what the adapter SAYS it exposes; the DeploymentProfile is
// what was accepted. The handshake (adapter/handshake.hpp) compares the two
// and fails closed for affected operation classes on any disagreement. The
// honesty boundary is explicit: drift of an honest manifest is detected; a
// dishonest under-declaring manifest is not (ADL §11) — bounded by the
// adapter's position in the trust base and conformance evidence.
//
// Day-one (RCA-1) the channel is in-process: the loopback adapter registers
// directly with a fixed credential. The RegistrationRequest shape (manifest
// + credential validated by RuntimeHost) is the contract the real
// named-pipe/localhost transport will carry (design §8).
// ============================================================================

#include <qiven/runtime/identity.hpp>

#include <string>
#include <vector>

namespace qiven::runtime::adapter
{
// Closed operation-class taxonomy for the first landing. The class is the
// unit of fail-closed scope and of complete-mediation claims (C-07).
enum class OperationClass : u8
{
    Observe,
    FileSystemRead,
    FileSystemWrite,
    ProcessLaunch,
    NetworkAccess,
    ClaimChannel,
};

// One declared capability surface entry (ADL §9).
struct CapabilityDescriptor
{
    CapabilityId id {};
    AdapterInstanceId adapter {};
    OperationClass operation_class    = OperationClass::Observe;
    bool can_mutate_world             = false;
    bool can_publish_claims           = false;
    bool requires_execution_authority = false;

    [[nodiscard]] bool same_surface(const CapabilityDescriptor& other) const noexcept
    {
        return id.value == other.id.value && adapter.fnv == other.adapter.fnv &&
               operation_class == other.operation_class && can_mutate_world == other.can_mutate_world &&
               can_publish_claims == other.can_publish_claims &&
               requires_execution_authority == other.requires_execution_authority;
    }
};

// The adapter's actual capability manifest. identity is a content digest
// over the canonical manifest bytes (manifest_identity()); it is what a
// RuntimeGeneration binds (ADL §6 "adapter capability-manifest identities").
struct AdapterManifest
{
    std::string adapter_name;
    u32 manifest_version = 1;
    std::vector<CapabilityDescriptor> capabilities;

    // Day-one fixed loopback credential carried beside the manifest in a
    // real registration (design §8 adapter channel authentication).
    u64 credential_token = 0;
};

[[nodiscard]] ContentDigest manifest_identity(const AdapterManifest& manifest);

[[nodiscard]] AdapterInstanceId adapter_instance_id(const AdapterManifest& manifest);
} // namespace qiven::runtime::adapter
