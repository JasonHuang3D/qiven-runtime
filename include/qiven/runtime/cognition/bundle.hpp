#pragma once

// ============================================================================
// cognition/bundle.hpp — on-disk cognition bundles: layout, verification,
// ACTIVE pointer (MVP-2 batch design section 3.4; ARCH section 7.2)
//
// A bundle is an immutable, regenerable derivative of one exact Git
// commit, stored under <repo>/.qiven/runtime/bundles/<digest-hex>/:
//
//   manifest.json   qiven-cognition-bundle-v1 (the writer's field order)
//   snapshot.qvs    draft-canonical v8 Snapshot (policy-only floor)
//   policy.yaml     the invocation-policy bytes VERBATIM from the commit
//   source/<path>   every declared source file, byte-exact from the tree
//
// Verification is load-time and total: EVERY manifest-named file is
// re-hashed before the bundle may be used (ARCH exit gate: modifying any
// bundle file makes loading fail). The ACTIVE pointer is a single-line
// digest file swapped by atomic rename; a missing or dangling pointer is
// Unavailable — the store NEVER falls back to a working tree (ARCH
// section 7.4). This class also implements ICognitionBundlePort by
// pinning the ACTIVE bundle after proving it matches the caller's
// manifest field-for-field.
// ============================================================================

#include <qiven/result.hpp>
#include <qiven/runtime/identity.hpp>
#include <qiven/runtime/port/cognition_bundle.hpp>
#include <qiven/runtime/port/cognition_port.hpp>

#include <filesystem>
#include <span>
#include <string>

namespace qiven::runtime::cognition
{
struct VerifiedBundle
{
    port::CognitionBundleManifest manifest;
    std::filesystem::path dir;
    std::string snapshot_bytes; // v8 serialization (already digest-verified)
    std::string policy_bytes;   // invocation-policy bytes (already digest-verified)
};

class BundleStore final : public port::ICognitionBundlePort
{
public:
    // runtime_root = <repo>/.qiven/runtime (GR-5); bundles live under it.
    explicit BundleStore(std::filesystem::path runtime_root);

    // Load the ACTIVE bundle and reverify every file (exit gate 2).
    [[nodiscard]] qiven::Result<VerifiedBundle, port::BundleError> load_active() const;

    // Load one bundle by its manifest-digest directory name.
    [[nodiscard]] qiven::Result<VerifiedBundle, port::BundleError> load(
        const ContentDigest& bundle_digest) const;

    // ICognitionBundlePort: verifies the ACTIVE bundle equals the passed
    // manifest field-for-field, then pins its snapshot through the frozen
    // draft reader with the manifest digest as precondition. The returned
    // PinnedCognition carries source_revision (the Git commit oid) DISTINCT
    // from its content-addressed revision (exit gate 3's separation).
    [[nodiscard]] port::BundlePin pin_from_bundle(
        const port::CognitionBundleManifest& manifest) const override;

    // The ACTIVE pointer value (bundle digest), or an error when absent.
    [[nodiscard]] qiven::Result<ContentDigest, port::BundleError> active_digest() const;

    [[nodiscard]] const std::filesystem::path& bundles_root() const noexcept
    {
        return m_bundles_root;
    }

private:
    std::filesystem::path m_bundles_root;
};

// SHA-256 over given bytes as a ContentDigest (shared with the publisher).
[[nodiscard]] ContentDigest digest_of(std::string_view bytes);
[[nodiscard]] ContentDigest digest_of_file(const std::filesystem::path& file);

// Hex helpers shared by publisher/store (bundle directory naming).
[[nodiscard]] std::string hex_lower(std::span<const std::byte> bytes);
[[nodiscard]] qiven::Result<ContentDigest, port::BundleError> digest_from_hex(
    std::string_view hex);
} // namespace qiven::runtime::cognition
