#include <qiven/runtime/adapter/manifest.hpp>

#include <algorithm>
#include <array>

namespace qiven::runtime::adapter
{
namespace
{
// Canonical little-endian preimage writers for the manifest digest. Fixed
// widths, host-endian independent (representation law, foundation.md §10).
void put_u32(std::vector<std::byte>& out, u32 value)
{
    for (int i = 0; i < 4; ++i)
    {
        out.push_back(static_cast<std::byte>(value >> (8 * i)));
    }
}

void put_u64(std::vector<std::byte>& out, u64 value)
{
    for (int i = 0; i < 8; ++i)
    {
        out.push_back(static_cast<std::byte>(value >> (8 * i)));
    }
}

void put_bytes(std::vector<std::byte>& out, const std::string& text)
{
    put_u64(out, static_cast<u64>(text.size()));
    for (const char ch : text)
    {
        out.push_back(static_cast<std::byte>(ch));
    }
}
} // namespace

AdapterInstanceId adapter_instance_id(const AdapterManifest& manifest)
{
    return AdapterInstanceId { fnv1a64(manifest.adapter_name) };
}

ContentDigest manifest_identity(const AdapterManifest& manifest)
{
    // Deterministic canonical preimage: adapter identity + version +
    // capability count + each capability's exact declared surface, in
    // manifest order. Two manifests with identical content hash identically
    // regardless of allocation history.
    std::vector<std::byte> preimage;
    put_bytes(preimage, manifest.adapter_name);
    put_u32(preimage, manifest.manifest_version);
    put_u64(preimage, static_cast<u64>(manifest.capabilities.size()));
    for (const CapabilityDescriptor& capability : manifest.capabilities)
    {
        put_u64(preimage, capability.id.value);
        put_u64(preimage, capability.adapter.fnv);
        put_u32(preimage, static_cast<u32>(capability.operation_class));
        put_u32(preimage,
                (capability.can_mutate_world ? 1U : 0U) | (capability.can_publish_claims ? 2U : 0U) |
                    (capability.requires_execution_authority ? 4U : 0U));
    }

    return ContentDigest { sha256(preimage.data(), preimage.size()) };
}
} // namespace qiven::runtime::adapter
