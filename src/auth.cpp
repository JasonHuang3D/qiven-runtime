#include <qiven/runtime/auth.hpp>

#include <random>

namespace qiven::runtime::auth
{
namespace
{
// FIPS 198-1: key padded/hashed to the block size, inner and outer
// prefixes fixed. One SHA-256 block is 64 bytes; the SecretKey is 32 so
// the pad path is the only one exercised, but the hash path is kept for
// correctness.
constexpr usize block_size = 64;

void xor_pad(std::array<std::byte, block_size>& pad, const std::byte value, const SecretKey& key)
{
    for (usize i = 0; i < block_size; ++i)
    {
        const auto key_byte = i < key.size() ? static_cast<unsigned char>(key[i]) : 0U;
        pad[i]              = static_cast<std::byte>(key_byte ^ static_cast<unsigned char>(value));
    }
}
} // namespace

void csrandom_fill(std::span<std::byte> out)
{
    // MSVC std::random_device is rand_s (RtlGenRandom): the platform
    // CSPRNG. See the header's platform note before porting.
    std::random_device device;
    for (usize i = 0; i < out.size(); i += 4)
    {
        const u32 word   = device();
        const usize take = out.size() - i < 4 ? out.size() - i : 4;
        for (usize j = 0; j < take; ++j)
        {
            out[i + j] = static_cast<std::byte>(word >> (8 * j));
        }
    }
}

SecretKey csrandom_secret()
{
    SecretKey key {};
    csrandom_fill(key);
    return key;
}

SHA256Digest hmac_sha256(const SecretKey& key, std::span<const std::byte> message)
{
    std::array<std::byte, block_size> inner_pad {};
    std::array<std::byte, block_size> outer_pad {};
    xor_pad(inner_pad, std::byte { 0x36 }, key);
    xor_pad(outer_pad, std::byte { 0x5C }, key);

    SHA256Hasher inner;
    inner.update(inner_pad.data(), inner_pad.size());
    inner.update(message.data(), message.size());
    const SHA256Digest inner_digest = inner.finish();

    SHA256Hasher outer;
    outer.update(outer_pad.data(), outer_pad.size());
    outer.update(inner_digest.data(), inner_digest.size());
    return outer.finish();
}

bool constant_time_equal(std::span<const std::byte> a, std::span<const std::byte> b) noexcept
{
    if (a.size() != b.size())
    {
        return false;
    }
    unsigned char diff = 0;
    for (usize i = 0; i < a.size(); ++i)
    {
        diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    }
    return diff == 0;
}
} // namespace qiven::runtime::auth
