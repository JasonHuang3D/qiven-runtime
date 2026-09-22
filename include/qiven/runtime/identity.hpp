#pragma once

// ============================================================================
// identity.hpp — strong identity types of the control plane
//
// No raw strings cross control-plane APIs: identities are distinct value
// types (component ADL §4/design §3). Three families:
//   - monotonic per-host counters (generation, transaction);
//   - fnv1a64-derived stable identities (adapter instance) — identity
//     hashing only, never an authorization token (production-MVP §10.3);
//   - 128-bit sortable identifiers (UUIDv7-shaped) for the authorization
//     surfaces where collision resistance is a security property
//     (decision, dispatch; production-MVP §10.3).
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
    Dispatch,
};

// 128-bit sortable identity (production-MVP §10.3: UUIDv7 or an
// equivalent sortable 128-bit identifier). Layout: unix epoch
// milliseconds in the high 64 bits (RFC 9562 time field shape), a
// per-minter monotonic counter in the low 64 bits with the variant
// nibble set — sortable by mint time, unique within one minter
// lifetime, and never derived from payload content.
struct SortableId128
{
    std::array<std::byte, 16> bytes {};

    [[nodiscard]] bool operator==(const SortableId128& other) const noexcept
    {
        return bytes == other.bytes;
    }
    [[nodiscard]] bool operator!=(const SortableId128& other) const noexcept
    {
        return !(*this == other);
    }
    [[nodiscard]] bool operator<(const SortableId128& other) const noexcept
    {
        return bytes < other.bytes;
    }
};

// Renders "<kind>-<32 hex>" of the full 128-bit value (log identity).
[[nodiscard]] std::string render_id(IdKind kind, const SortableId128& id);

// Mints UUIDv7-shaped sortable ids. One minter per installing host
// process; ids never repeat within one minter lifetime.
class SortableIdMinter
{
public:
    SortableIdMinter()                                   = default;
    SortableIdMinter(const SortableIdMinter&)            = delete;
    SortableIdMinter& operator=(const SortableIdMinter&) = delete;

    [[nodiscard]] SortableId128 next();

private:
    u64 m_counter = 0;
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

struct HarnessSessionId
{
    u64 value = 0; // harness-assigned session identity
};

struct ActorInstanceId
{
    u64 value = 0; // originates ONLY from an accepted adapter/session binding
};

struct HarnessActionId
{
    u64 value = 0; // adapter-assigned identity of one exact harness action
};

struct CapabilityId
{
    u64 value = 0;
};

struct EvidenceId
{
    u64 fnv = 0; // fnv1a64 over the receipt's bound identity fields
};

// Decision identity and single-use token: see decision.hpp. The token is
// cryptographic (HMAC-SHA256 binding + CSPRNG nonce); the ledger stores
// only TokenHash (SHA-256 over the token value) — the plaintext token
// exists only in the immediate issue/use path (production-MVP §10.3).
using DecisionId = SortableId128;

struct DecisionToken
{
    std::array<std::byte, 32> mac {};   // HMAC-SHA256 over the canonical binding
    std::array<std::byte, 16> nonce {}; // CSPRNG per minting

    [[nodiscard]] bool operator==(const DecisionToken& other) const noexcept
    {
        return mac == other.mac && nonce == other.nonce;
    }
};

struct TokenHash
{
    SHA256Digest value {};

    [[nodiscard]] bool operator==(const TokenHash& other) const noexcept
    {
        return value == other.value;
    }
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

// The external-boundary correlation key (ADL §17): a five-tuple binding one
// exact harness action to its control identity. Immutable, equality-
// comparable, hashable. For one exact harness action the key maps to one
// ControlTransactionId and stays stable through interception, decision,
// dispatch, observation and reconciliation (C-18). There is no global
// "currentAction" — correlation is per-action, never ambient.
struct CorrelationKey
{
    RuntimeGenerationId generation {};
    AdapterInstanceId adapter {};
    HarnessSessionId session {};
    ActorInstanceId actor {};
    HarnessActionId action {};

    [[nodiscard]] bool operator==(const CorrelationKey& other) const noexcept
    {
        return generation.value == other.generation.value && adapter.fnv == other.adapter.fnv &&
               session.value == other.session.value && actor.value == other.actor.value &&
               action.value == other.action.value;
    }
    [[nodiscard]] bool operator!=(const CorrelationKey& other) const noexcept
    {
        return !(*this == other);
    }
};

// Deterministic non-cryptographic correlation identity (fnv1a64 over the
// five tuple fields; identity hashing, not integrity).
[[nodiscard]] u64 correlation_hash(const CorrelationKey& key) noexcept;
} // namespace qiven::runtime

template <>
struct std::hash<qiven::runtime::CorrelationKey>
{
    [[nodiscard]] std::size_t operator()(const qiven::runtime::CorrelationKey& key) const noexcept
    {
        return static_cast<std::size_t>(qiven::runtime::correlation_hash(key));
    }
};

template <>
struct std::hash<qiven::runtime::TokenHash>
{
    [[nodiscard]] std::size_t operator()(const qiven::runtime::TokenHash& token) const noexcept;
};

template <>
struct std::hash<qiven::runtime::SortableId128>
{
    [[nodiscard]] std::size_t operator()(const qiven::runtime::SortableId128& id) const noexcept
    {
        // fnv1a64 over the 16 id bytes: bucketing only — equality always
        // compares the full value (§10.3: a 64-bit hash is not an identity)
        qiven::u64 hash = qiven::fnv1a64_offset_basis;
        for (const std::byte b : id.bytes)
        {
            hash ^= static_cast<qiven::u64>(b);
            hash *= qiven::fnv1a64_prime;
        }
        return static_cast<std::size_t>(hash);
    }
};
