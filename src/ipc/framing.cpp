#include <qiven/runtime/ipc/framing.hpp>

#include <qiven/endian.hpp>
#include <qiven/error.hpp>
#include <qiven/hashing.hpp>

#include <array>
#include <cstring>
#include <utility>

namespace qiven::runtime::ipc
{
namespace
{
// RR-0: the local string-based put/get shift loops are retired; the frame
// header is encoded/decoded through the foundation codecs (layout and
// widths unchanged — the golden frame fixture pins them).
constexpr usize header_len   = 4 + 2 + 2 + 8 + 8 + 8; // fields before the MAC
constexpr usize mac_len      = 32;
constexpr usize frame_prefix = header_len + mac_len;

[[nodiscard]] u32 get_u32(const char* p) noexcept
{
    const auto value =
        qiven::decode_le_u32({ reinterpret_cast<const std::byte*>(p), 4 });
    return value.value_or(0);
}

[[nodiscard]] u64 get_u64(const char* p) noexcept
{
    const auto value =
        qiven::decode_le_u64({ reinterpret_cast<const std::byte*>(p), 8 });
    return value.value_or(0);
}

qiven::Error denial(i32 code, std::string_view detail)
{
    return qiven::Error::make(code == err_auth || code == err_replay || code == err_frame
                                  ? qiven::error_category::invalid_argument
                                  : qiven::error_category::none,
                              code, "ipc: " + std::string(detail));
}

std::size_t hash_nonce(const std::array<std::byte, 16>& nonce) noexcept
{
    qiven::u64 hash = qiven::fnv1a64_offset_basis;
    for (const std::byte b : nonce)
    {
        hash ^= static_cast<qiven::u64>(b);
        hash *= qiven::fnv1a64_prime;
    }
    return static_cast<std::size_t>(hash);
}
} // namespace

FrameCodec::FrameCodec(auth::SecretKey key) :
m_key(key)
{
}

std::string FrameCodec::encode(const FrameHeader& header, std::string_view body) const
{
    std::array<std::byte, header_len> prefix {};
    (void)qiven::encode_le_u32(header.magic, { prefix.data(), 4 });
    (void)qiven::encode_le_u32(
        static_cast<u32>(header.proto) | (static_cast<u32>(header.flags) << 16),
        { prefix.data() + 4, 4 });
    (void)qiven::encode_le_u64(static_cast<u64>(body.size()), { prefix.data() + 8, 8 });
    (void)qiven::encode_le_u64(header.request_id, { prefix.data() + 16, 8 });
    (void)qiven::encode_le_u64(header.connection_seq, { prefix.data() + 24, 8 });

    std::string out;
    out.reserve(frame_prefix + body.size());
    out.append(reinterpret_cast<const char*>(prefix.data()), prefix.size());
    // the length field ALWAYS describes the actual body (encoded above)

    std::string mac_input = out;
    mac_input.append(body);
    const SHA256Digest mac =
        auth::hmac_sha256(m_key, { reinterpret_cast<const std::byte*>(mac_input.data()),
                                   mac_input.size() });
    out.append(reinterpret_cast<const char*>(mac.data()), mac.size());
    out.append(body);
    return out;
}

qiven::Result<VerifiedFrame> FrameCodec::decode(std::string_view bytes) const
{
    using DecodeResult = qiven::Result<VerifiedFrame>;
    if (bytes.size() < frame_prefix || bytes.size() > max_frame_bytes)
    {
        return DecodeResult::fail(denial(err_frame, "frame size outside bounds"));
    }
    const char* data = bytes.data();
    FrameHeader header;
    header.magic          = get_u32(data + 0);
    const u32 proto_flags = get_u32(data + 4);
    header.proto          = static_cast<u16>(proto_flags & 0xFFFFu);
    header.flags          = static_cast<u16>(proto_flags >> 16);
    header.body_len       = get_u64(data + 8);
    header.request_id     = get_u64(data + 16);
    header.connection_seq = get_u64(data + 24);

    if (header.magic != frame_magic)
    {
        return DecodeResult::fail(denial(err_frame, "bad frame magic"));
    }
    if (header.proto != protocol_version)
    {
        return DecodeResult::fail(denial(err_frame, "unsupported protocol version"));
    }
    if (header.body_len != bytes.size() - frame_prefix)
    {
        return DecodeResult::fail(denial(err_frame, "body length disagrees with frame"));
    }
    if (header.body_len > max_body_bytes)
    {
        return DecodeResult::fail(denial(err_frame, "body exceeds the 1 MiB cap"));
    }

    // MAC over header-without-MAC + body.
    std::string mac_input(bytes.substr(0, header_len));
    mac_input.append(bytes.substr(frame_prefix));
    const SHA256Digest expected = auth::hmac_sha256(
        m_key, { reinterpret_cast<const std::byte*>(mac_input.data()), mac_input.size() });
    const std::span<const std::byte> provided {
        reinterpret_cast<const std::byte*>(data + header_len), mac_len
    };
    if (!auth::constant_time_equal(provided,
                                   { reinterpret_cast<const std::byte*>(expected.data()),
                                     expected.size() }))
    {
        return DecodeResult::fail(denial(err_auth, "HMAC verification failed"));
    }

    VerifiedFrame frame;
    frame.header = header;
    frame.body.assign(bytes.substr(frame_prefix));
    return DecodeResult(std::move(frame));
}

bool ReplayGuard::accept(u64 connection_seq, std::span<const std::byte> nonce, u64 timestamp_ms,
                         u64 now_ms)
{
    if (connection_seq <= m_last_seq)
    {
        return false; // seq must strictly increase per connection
    }
    // The peer timestamp must sit inside the window of BOTH clocks: the
    // live clock and the journal-informed wall clock (D-8 clamp — a
    // rolled-back wall clock must not widen the replay window).
    const u64 reference = m_wall_ms != 0 ? m_wall_ms : now_ms;
    const u64 drift     = timestamp_ms > reference ? timestamp_ms - reference
                                                   : reference - timestamp_ms;
    if (drift > window_ms)
    {
        return false;
    }
    if (nonce.size() != 16)
    {
        return false;
    }
    std::array<std::byte, 16> value {};
    std::memcpy(value.data(), nonce.data(), 16);
    const std::size_t hash = hash_nonce(value);
    if (m_seen.find(hash) != m_seen.end())
    {
        return false; // replayed nonce
    }
    m_recent.push_back(value);
    m_seen.insert(hash);
    if (m_recent.size() > nonce_cache)
    {
        m_seen.erase(hash_nonce(m_recent.front()));
        m_recent.pop_front();
    }
    m_last_seq = connection_seq;
    return true;
}
} // namespace qiven::runtime::ipc
