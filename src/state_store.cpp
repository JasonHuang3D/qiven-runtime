#include <qiven/runtime/state_store.hpp>

#include <qiven/byte_builder.hpp>
#include <qiven/contracts.hpp>
#include <qiven/endian.hpp>
#include <qiven/memory/allocator.hpp>
#include <qiven/memory/system_allocator.hpp>

#include <algorithm>
#include <unordered_set>

namespace qiven::runtime
{
namespace
{
// RR-0: the local shift-loop put/get mechanics are retired; scalars ride
// the foundation codecs and accumulation rides the foundation ByteBuilder
// (representation unchanged — the golden state-image fixture pins it).
constexpr usize image_capacity_limit = 16 * 1024 * 1024;

[[nodiscard]] bool get_u32(std::span<const std::byte> bytes, usize& offset, u32& value) noexcept
{
    if (offset + 4 > bytes.size())
    {
        return false;
    }
    const auto decoded = qiven::decode_le_u32(bytes.subspan(offset, 4));
    if (!decoded)
    {
        return false;
    }
    value = *decoded;
    offset += 4;
    return true;
}

[[nodiscard]] bool get_u64(std::span<const std::byte> bytes, usize& offset, u64& value) noexcept
{
    if (offset + 8 > bytes.size())
    {
        return false;
    }
    const auto decoded = qiven::decode_le_u64(bytes.subspan(offset, 8));
    if (!decoded)
    {
        return false;
    }
    value = *decoded;
    offset += 8;
    return true;
}

constexpr u32 test_state_magic = 0x51535432; // "QST2" (test-only image; see header)
} // namespace

std::vector<std::byte> serialize_test_state_image(const std::unordered_set<TokenHash>& consumed,
                                                  const ReconciliationBarrier& barriers)
{
    // deterministic order: hashes and scopes are SORTED so the same state
    // serializes to identical bytes (a store image is content-addressable)
    std::vector<TokenHash> hashes(consumed.begin(), consumed.end());
    std::sort(hashes.begin(), hashes.end(),
              [](const TokenHash& a, const TokenHash& b) { return a.value < b.value; });

    qiven::memory::SystemAllocator allocator;
    auto out = qiven::ByteBuilder::try_create(qiven::memory::AllocatorRef { allocator },
                                              image_capacity_limit);
    QIVEN_ASSERT(out.has_value());
    (void)out->append_le_u32(test_state_magic);

    (void)out->append_le_u32(static_cast<u32>(hashes.size()));
    for (const TokenHash& hash : hashes)
    {
        (void)out->append({ hash.value.data(), hash.value.size() });
    }

    const std::vector<u64> scopes = barriers.active_scope_ids(); // sorted
    (void)out->append_le_u32(static_cast<u32>(scopes.size()));
    for (const u64 scope : scopes)
    {
        (void)out->append_le_u64(scope);
    }
    QIVEN_ASSERT(out->ok()); // bounded far beyond any test image
    return { out->bytes().begin(), out->bytes().end() };
}

std::optional<TestOnlyStateImage> deserialize_test_state_image(std::span<const std::byte> bytes)
{
    usize offset = 0;
    u32 magic    = 0;
    if (!get_u32(bytes, offset, magic) || magic != test_state_magic)
    {
        return std::nullopt;
    }

    TestOnlyStateImage image;
    u32 hash_count = 0;
    if (!get_u32(bytes, offset, hash_count))
    {
        return std::nullopt;
    }
    if (static_cast<usize>(hash_count) * 32 + offset > bytes.size())
    {
        return std::nullopt; // declared counts may not exceed the payload
    }
    image.consumed_token_hashes.reserve(hash_count);
    for (u32 i = 0; i < hash_count; ++i)
    {
        TokenHash hash;
        if (offset + hash.value.size() > bytes.size())
        {
            return std::nullopt;
        }
        for (usize j = 0; j < hash.value.size(); ++j)
        {
            hash.value[j] = bytes[offset + j];
        }
        offset += hash.value.size();
        image.consumed_token_hashes.push_back(hash);
    }

    u32 scope_count = 0;
    if (!get_u32(bytes, offset, scope_count))
    {
        return std::nullopt;
    }
    if (static_cast<usize>(scope_count) * 8 + offset > bytes.size())
    {
        return std::nullopt;
    }
    image.barrier_scopes.reserve(scope_count);
    for (u32 i = 0; i < scope_count; ++i)
    {
        u64 scope = 0;
        if (!get_u64(bytes, offset, scope))
        {
            return std::nullopt;
        }
        image.barrier_scopes.push_back(scope);
    }

    if (offset != bytes.size())
    {
        return std::nullopt; // trailing bytes = corrupt, never guessed
    }
    return image;
}

bool InMemoryTestStateStore::save(std::span<const std::byte> image)
{
    m_image.assign(image.begin(), image.end());
    m_has_image = true;
    return true;
}

std::optional<std::vector<std::byte>> InMemoryTestStateStore::load() const
{
    if (!m_has_image)
    {
        return std::nullopt;
    }
    return m_image;
}

void restore_barriers(ReconciliationBarrier& barriers, const TestOnlyStateImage& image)
{
    for (const u64 scope : image.barrier_scopes)
    {
        barriers.raise(EffectScope { scope }, ControlTransactionId { 0 });
    }
}

void seed_consumed_tokens(DecisionLedger& ledger, const TestOnlyStateImage& image)
{
    for (const TokenHash& hash : image.consumed_token_hashes)
    {
        ledger.seed_consumed(hash);
    }
}
} // namespace qiven::runtime
