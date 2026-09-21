#include <qiven/runtime/identity.hpp>

namespace qiven::runtime
{
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
} // namespace qiven::runtime
