#pragma once

// ============================================================================
// auth.hpp — cryptographic primitives for authorization tokens
// (production-MVP architecture §10.3, MVP-0)
//
// The decision token is HMAC-SHA256 over the canonical binding preimage
// under an installation secret, domain-separated by a CSPRNG nonce; the
// ledger stores only the SHA-256 of the token value. A 64-bit hash is
// never an authorization token (§10.3).
//
// Platform note (recorded, not silent): the CSPRNG source is
// std::random_device, which on the MSVC/Windows toolchain this repository
// pins (qiven-toolchain-win) is rand_s (RtlGenRandom) — cryptographically
// secure. A non-Windows port MUST substitute a verified CSPRNG before
// these primitives guard real effects; that port lands with MVP-3 IPC.
// Home: runtime-local for MVP-0; graduates to qiven-foundation when a
// second consumer exists (ADR-0024 downward-ownership rule).
// ============================================================================

#include <qiven/hashing_sha256.hpp>
#include <qiven/types.hpp>

#include <array>
#include <cstddef>
#include <span>

namespace qiven::runtime::auth
{
// 256-bit installation secret for decision-token HMAC (production-MVP
// §12.1 stores the IPC client secret under DPAPI; the decision secret
// has the same lifetime class — installation-local, never persisted in
// the journal).
using SecretKey = std::array<std::byte, 32>;

// Cryptographically secure random bytes (see the platform note above).
void csrandom_fill(std::span<std::byte> out);

[[nodiscard]] SecretKey csrandom_secret();

// HMAC-SHA256 (FIPS 198-1) over the message spans under one key.
[[nodiscard]] SHA256Digest hmac_sha256(const SecretKey& key, std::span<const std::byte> message);

// Constant-time equality: no early exit on the first differing byte.
[[nodiscard]] bool constant_time_equal(std::span<const std::byte> a, std::span<const std::byte> b) noexcept;
} // namespace qiven::runtime::auth
