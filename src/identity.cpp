#include <qiven/runtime/identity.hpp>

#include <chrono>

namespace qiven::runtime
{
namespace
{
u64 unix_ms_now() noexcept
{
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

void put_u64_le(std::array<std::byte, 16>& out, const usize offset, const u64 value) noexcept
{
    for (usize i = 0; i < 8; ++i)
    {
        out[offset + i] = static_cast<std::byte>(value >> (8 * i));
    }
}
} // namespace

u64 correlation_hash(const CorrelationKey& key) noexcept
{
    u64 seed = fnv1a64_offset_basis;
    seed ^= key.generation.value;
    seed *= fnv1a64_prime;
    seed ^= key.adapter.fnv;
    seed *= fnv1a64_prime;
    seed ^= key.session.value;
    seed *= fnv1a64_prime;
    seed ^= key.actor.value;
    seed *= fnv1a64_prime;
    seed ^= key.action.value;
    seed *= fnv1a64_prime;
    return seed;
}

SortableId128 SortableIdMinter::next()
{
    // UUIDv7 shape (RFC 9562): 48-bit unix-ms timestamp, then a
    // per-minter monotonic counter; the variant nibble marks the layout.
    // Uniqueness within one minter lifetime is by construction; ids are
    // sortable by mint time because the timestamp occupies the high bits.
    SortableId128 id;
    const u64 unix_ms = unix_ms_now();
    put_u64_le(id.bytes, 0, (unix_ms << 16) | 0x7000U | ((m_counter >> 20) & 0x0FFFU));
    put_u64_le(id.bytes, 8, m_counter++);
    id.bytes[8] = static_cast<std::byte>(0x80U | (static_cast<unsigned char>(id.bytes[8]) & 0x3FU));
    return id;
}

std::string render_id(IdKind kind, u64 bits)
{
    static constexpr const char* names[] = {
        "generation",
        "transaction",
        "adapter",
        "session",
        "actor",
        "capability",
        "evidence",
        "decision",
        "requirement",
        "effect-scope",
        "dispatch",
    };

    const auto index = static_cast<unsigned>(kind);
    std::string out;
    out.reserve(16 + 1 + 16);
    out.append(names[index]);
    out.push_back('-');
    char hex[16];
    to_hex_u64(bits, hex);
    out.append(hex, 16);
    return out;
}

std::string render_id(IdKind kind, const SortableId128& id)
{
    static constexpr const char* names[] = {
        "generation",
        "transaction",
        "adapter",
        "session",
        "actor",
        "capability",
        "evidence",
        "decision",
        "requirement",
        "effect-scope",
        "dispatch",
    };

    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(16 + 1 + 32);
    out.append(names[static_cast<unsigned>(kind)]);
    out.push_back('-');
    for (const std::byte b : id.bytes)
    {
        const auto v = static_cast<unsigned char>(b);
        out.push_back(digits[v >> 4]);
        out.push_back(digits[v & 0x0FU]);
    }
    return out;
}
} // namespace qiven::runtime

namespace std
{
std::size_t hash<qiven::runtime::TokenHash>::operator()(const qiven::runtime::TokenHash& token) const noexcept
{
    // bucketing only: equality compares the full 32-byte value
    const auto& bytes = token.value;
    return static_cast<std::size_t>(static_cast<qiven::u64>(bytes[0]) | (static_cast<qiven::u64>(bytes[8]) << 8) |
                                    (static_cast<qiven::u64>(bytes[16]) << 16) |
                                    (static_cast<qiven::u64>(bytes[24]) << 24));
}
} // namespace std
