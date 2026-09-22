#pragma once

// ============================================================================
// ipc/framing.hpp — authenticated length-prefixed IPC frames
// (MVP-3 batch design section 3.1; ARCH section 12.1; cpp-design section 10)
//
// Wire format (little-endian, fixed order):
//   [u32 magic 'QVR1'][u16 proto=1][u16 flags][u64 body_len]
//   [u64 request_id][u64 connection_seq][32-byte HMAC-SHA256][body bytes]
//
// The HMAC key is the DPAPI-protected installation secret; the MAC covers
// the header (excluding the MAC field itself) concatenated with the body.
// Every violation — bad magic, unknown protocol version, oversize body,
// MAC mismatch, sequence regression, replayed nonce, timestamp outside
// the window — is a typed denial (62/63/64), never a silent accept.
//
// ReplayGuard validates timestamps against the JOURNAL wall clock
// (cpp-design review note 3: the window clamps to the D-8 regression
// policy — a peer timestamp is never trusted over journal time).
// ============================================================================

#include <qiven/result.hpp>
#include <qiven/runtime/auth.hpp>
#include <qiven/types.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>

namespace qiven::runtime::ipc
{
inline constexpr u32 frame_magic      = 0x51565231; // "QVR1"
inline constexpr u16 protocol_version = 1;
inline constexpr u64 max_body_bytes   = 1024 * 1024;
inline constexpr u64 max_frame_bytes  = max_body_bytes + 64;

inline constexpr i32 err_host_recovering = 61;
inline constexpr i32 err_auth            = 62;
inline constexpr i32 err_replay          = 63;
inline constexpr i32 err_frame           = 64;
inline constexpr i32 err_deadline        = 65;

struct FrameHeader
{
    u32 magic          = frame_magic;
    u16 proto          = protocol_version;
    u16 flags          = 0;
    u64 body_len       = 0;
    u64 request_id     = 0;
    u64 connection_seq = 1; // strictly increasing per connection
};

struct VerifiedFrame
{
    FrameHeader header;
    std::string body;
};

class FrameCodec
{
public:
    explicit FrameCodec(auth::SecretKey key);

    // Deterministic encode (MAC computed over header+body).
    [[nodiscard]] std::string encode(const FrameHeader& header, std::string_view body) const;

    // Total verification: size cap, magic, protocol, MAC. Payload-level
    // replay/seq checks belong to ReplayGuard (they need connection state).
    [[nodiscard]] qiven::Result<VerifiedFrame> decode(std::string_view bytes) const;

    [[nodiscard]] const auth::SecretKey& key() const noexcept
    {
        return m_key;
    }

private:
    auth::SecretKey m_key {};
};

// Bounded per-connection replay protection: strictly increasing
// connection sequence, last-256-nonce LRU, ±120 s timestamp window
// against the journal-informed wall clock.
class ReplayGuard
{
public:
    static constexpr u64 window_ms     = 120'000;
    static constexpr usize nonce_cache = 256;

    // Journal wall clock (D-8): updated at boot and per request batch.
    void note_wall_clock(u64 now_ms) noexcept
    {
        m_wall_ms = now_ms;
    }

    // Accepts and RECORDS, or rejects: seq regression, duplicate nonce,
    // timestamp outside the window (checked against the journal clock).
    [[nodiscard]] bool accept(u64 connection_seq, std::span<const std::byte> nonce,
                              u64 timestamp_ms, u64 now_ms);

private:
    std::deque<std::array<std::byte, 16>> m_recent;
    std::unordered_set<std::size_t> m_seen;
    u64 m_last_seq = 0;
    u64 m_wall_ms  = 0;
};

[[nodiscard]] inline std::span<const std::byte> as_bytes(std::string_view text) noexcept
{
    return { reinterpret_cast<const std::byte*>(text.data()), text.size() };
}
} // namespace qiven::runtime::ipc
