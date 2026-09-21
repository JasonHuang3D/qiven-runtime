#include <qiven/runtime/state_store.hpp>

#include <algorithm>
#include <unordered_set>

namespace qiven::runtime
{
namespace
{
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

[[nodiscard]] bool get_u32(std::span<const std::byte> bytes, usize& offset, u32& value) noexcept
{
    if (offset + 4 > bytes.size())
    {
        return false;
    }
    value = 0;
    for (int i = 0; i < 4; ++i)
    {
        value |= static_cast<u32>(bytes[offset + static_cast<usize>(i)]) << (8 * i);
    }
    offset += 4;
    return true;
}

[[nodiscard]] bool get_u64(std::span<const std::byte> bytes, usize& offset, u64& value) noexcept
{
    if (offset + 8 > bytes.size())
    {
        return false;
    }
    value = 0;
    for (int i = 0; i < 8; ++i)
    {
        value |= static_cast<u64>(bytes[offset + static_cast<usize>(i)]) << (8 * i);
    }
    offset += 8;
    return true;
}

constexpr u32 control_state_magic = 0x51535431; // "QST1"
} // namespace

std::vector<std::byte> serialize_control_state(const std::unordered_set<u64>& consumed_tokens,
                                               const ReconciliationBarrier& barriers)
{
    // deterministic order: tokens and scopes are SORTED so the same state
    // serializes to identical bytes (a store image is content-addressable)
    std::vector<u64> tokens(consumed_tokens.begin(), consumed_tokens.end());
    std::sort(tokens.begin(), tokens.end());

    std::vector<std::byte> out;
    put_u32(out, control_state_magic);

    put_u32(out, static_cast<u32>(tokens.size()));
    for (const u64 token : tokens)
    {
        put_u64(out, token);
    }

    const std::vector<u64> scopes = barriers.active_scope_ids(); // sorted
    put_u32(out, static_cast<u32>(scopes.size()));
    for (const u64 scope : scopes)
    {
        put_u64(out, scope);
    }
    return out;
}

std::optional<ControlStateImage> deserialize_control_state(std::span<const std::byte> bytes)
{
    usize offset = 0;
    u32 magic    = 0;
    if (!get_u32(bytes, offset, magic) || magic != control_state_magic)
    {
        return std::nullopt;
    }

    ControlStateImage image;
    u32 token_count = 0;
    if (!get_u32(bytes, offset, token_count))
    {
        return std::nullopt;
    }
    if (static_cast<usize>(token_count) * 8 + offset > bytes.size())
    {
        return std::nullopt; // declared counts may not exceed the payload
    }
    image.consumed_tokens.reserve(token_count);
    for (u32 i = 0; i < token_count; ++i)
    {
        u64 token = 0;
        if (!get_u64(bytes, offset, token))
        {
            return std::nullopt;
        }
        image.consumed_tokens.push_back(token);
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

bool InMemoryRuntimeStateStore::save(std::span<const std::byte> image)
{
    m_image.assign(image.begin(), image.end());
    m_has_image = true;
    return true;
}

std::optional<std::vector<std::byte>> InMemoryRuntimeStateStore::load() const
{
    if (!m_has_image)
    {
        return std::nullopt;
    }
    return m_image;
}

void restore_barriers(ReconciliationBarrier& barriers, const ControlStateImage& image)
{
    for (const u64 scope : image.barrier_scopes)
    {
        barriers.raise(EffectScope { scope }, ControlTransactionId { 0 });
    }
}

void seed_consumed_tokens(DecisionLedger& ledger, const ControlStateImage& image)
{
    for (const u64 token : image.consumed_tokens)
    {
        ledger.seed_consumed(token);
    }
}
} // namespace qiven::runtime
