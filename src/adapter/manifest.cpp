#include <qiven/runtime/adapter/manifest.hpp>

#include <qiven/byte_builder.hpp>
#include <qiven/contracts.hpp>
#include <qiven/memory/allocator.hpp>
#include <qiven/memory/system_allocator.hpp>

#include <algorithm>
#include <array>

namespace qiven::runtime::adapter
{
namespace
{
// RR-0: the local shift-loop put mechanics are retired; accumulation
// rides the foundation ByteBuilder (representation unchanged — the golden
// manifest fixture pins it).
constexpr qiven::usize preimage_capacity_limit = 16 * 1024 * 1024;

void put_bytes(qiven::ByteBuilder& out, const std::string& text)
{
    // per-format representation decision: u64 length prefix + raw bytes
    (void)out.append_le_u64(static_cast<qiven::u64>(text.size()));
    (void)out.append({ reinterpret_cast<const std::byte*>(text.data()), text.size() });
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
    qiven::memory::SystemAllocator allocator;
    auto preimage = qiven::ByteBuilder::try_create(qiven::memory::AllocatorRef { allocator },
                                                   preimage_capacity_limit);
    QIVEN_ASSERT(preimage.has_value());
    put_bytes(*preimage, manifest.adapter_name);
    (void)preimage->append_le_u32(manifest.manifest_version);
    (void)preimage->append_le_u64(static_cast<qiven::u64>(manifest.capabilities.size()));
    for (const CapabilityDescriptor& capability : manifest.capabilities)
    {
        (void)preimage->append_le_u64(capability.id.value);
        (void)preimage->append_le_u64(capability.adapter.fnv);
        (void)preimage->append_le_u32(static_cast<qiven::u32>(capability.operation_class));
        (void)preimage->append_le_u32(
            (capability.can_mutate_world ? 1U : 0U) |
            (capability.can_publish_claims ? 2U : 0U) |
            (capability.requires_execution_authority ? 4U : 0U));
    }
    QIVEN_ASSERT(preimage->ok()); // bounded far beyond any manifest enumeration

    return ContentDigest { sha256(preimage->bytes().data(), preimage->bytes().size()) };
}
} // namespace qiven::runtime::adapter
