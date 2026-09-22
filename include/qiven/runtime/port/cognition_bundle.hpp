#pragma once

// ============================================================================
// port/cognition_bundle.hpp — immutable cognition-bundle port
// (production-MVP architecture §7, MVP-0 definition; the native publisher
// lands with MVP-2)
//
// A production Judgment never reads a dirty checkout: every active
// cognition view is constructed from an EXACT Git commit (§7.1). The
// bundle is the immutable, reproducible derivative of that commit:
// manifest + snapshot + policy + the exact source files resolvers need.
// Revision identity and content identity are DISTINCT (§4 invariant 3):
// source_revision is the Git commit/tree identity; snapshot_sha256 is
// content. One never substitutes for the other.
//
// MVP-0 defines the manifest value and the verification contract; the
// publisher (read/validate an exact commit, emit the bundle, atomic
// active-pointer rename) lands with MVP-2.
// ============================================================================

#include <qiven/result.hpp>
#include <qiven/runtime/identity.hpp>
#include <qiven/runtime/port/cognition_port.hpp>

#include <string>
#include <vector>

namespace qiven::runtime::port
{
enum class BundleErrorKind : u8
{
    Unavailable,
    MalformedManifest,
    DigestMismatch, // any manifest-named file failed revalidation
    VersionUnsupported,
    SourceUnverifiable,
};

struct BundleError
{
    BundleErrorKind kind = BundleErrorKind::Unavailable;
    std::string detail;
};

// qiven-cognition-bundle-v1 manifest (§7.2). source_revision/source_tree
// are REVISION identity; the sha256 fields are CONTENT identity.
struct CognitionBundleManifest
{
    std::string schema = "qiven-cognition-bundle-v1";
    std::string repository;      // canonical clone URL
    std::string source_revision; // git commit oid — revision identity
    std::string source_tree;     // git tree oid
    std::string view;            // e.g. "production"
    u32 snapshot_format = 8;
    ContentDigest snapshot_digest {}; // content identity of snapshot.qvs
    ContentDigest policy_digest {};   // content identity of policy.yaml
    std::string publisher_build;
    std::string published_at; // RFC 3339

    struct SourceFile
    {
        std::string path;
        ContentDigest digest {};
    };
    std::vector<SourceFile> source_files;
};

struct BundlePinResult
{
    PinnedCognition pinned;
    CognitionBundleManifest manifest;
};

using BundlePin = qiven::Result<BundlePinResult, BundleError>;

class ICognitionBundlePort
{
public:
    virtual ~ICognitionBundlePort() = default;

    // Pin cognition from a VERIFIED bundle: every manifest-named file is
    // individually re-hash-checked before the pin is issued (§7.2), the
    // snapshot digest must match the manifest, and the pinned cognition
    // records the bundle's source_revision DISTINCTLY from its
    // content-addressed revision. A dirty checkout can never affect the
    // result; a modified bundle file fails closed.
    [[nodiscard]] virtual BundlePin pin_from_bundle(const CognitionBundleManifest& manifest) const = 0;
};
} // namespace qiven::runtime::port
