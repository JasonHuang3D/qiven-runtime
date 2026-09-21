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

struct DecisionToken
{
    u64 fnv = 0; // fnv1a64 over the decision's bound facts; single-use
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
