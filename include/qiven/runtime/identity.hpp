#pragma once

// ============================================================================
// identity.hpp — strong identity types of the control plane
//
// No raw strings cross control-plane APIs: identities are distinct value
// types (component ADL §4/design §3). Two families:
//   - monotonic per-host counters (generation, transaction);
//   - fnv1a64-derived stable identities (adapter instance).
// Integrity binding uses ContentDigest over foundation SHA-256 (layer
// contract §3.6); fnv1a64 remains the non-cryptographic identity primitive
// and is never a substitute for a digest.
// ============================================================================

#include <qiven/hashing.hpp>
#include <qiven/hashing_sha256.hpp>
#include <qiven/types.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace qiven::runtime
{
enum class IdKind : u8
{
    Generation,
    Transaction,
    Adapter,
    Session,
    Actor,
    Capability,
    Evidence,
    Decision,
    Requirement,
    EffectScope,
};

struct RuntimeGenerationId
{
    u64 value = 0; // monotonic per host
};

struct ControlTransactionId
{
    u64 value = 0; // monotonic per host
};

struct AdapterInstanceId
{
    u64 fnv = 0; // fnv1a64 over the stable adapter identity
};

struct CapabilityId
{
    u64 value = 0;
};

struct ProfileRevision
{
    u64 value = 0; // monotonic profile lineage
};

// Integrity-grade content binding (decisions, evidence, manifests).
struct ContentDigest
{
    SHA256Digest sha256 {};

    [[nodiscard]] bool operator==(const ContentDigest& other) const noexcept
    {
        return sha256 == other.sha256;
    }
    [[nodiscard]] bool operator!=(const ContentDigest& other) const noexcept
    {
        return !(*this == other);
    }
};

// Human-facing log rendering: "<kind>-<16 hex>" of the low 64 identity bits.
[[nodiscard]] std::string render_id(IdKind kind, u64 bits);

[[nodiscard]] inline std::string to_hex(const ContentDigest& digest)
{
    return to_hex_sha256(digest.sha256);
}
} // namespace qiven::runtime
