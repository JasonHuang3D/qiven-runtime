#pragma once

// ============================================================================
// cognition/publisher.hpp — CanonicalCognitionPublisher (MVP-2 batch design
// section 3.4; ARCH section 7; cpp-design section 8)
//
// Reads and validates ONE EXACT Git commit/tree through plumbing
// (rev-parse / ls-tree / cat-file) and produces the immutable bundle
// derivative: manifest + policy-only v8 snapshot + byte-exact source
// files, staged then atomic-renamed into
// <runtime_root>/bundles/<manifest-digest-hex>/, with the ACTIVE pointer
// swapped by rename. The publisher NEVER reads the working tree and
// never mutates the repository checkout (ARCH section 7.1) — a dirty
// checkout cannot affect a published or pinned RuntimeGeneration.
//
// Rich records ride as byte-exact source files; the snapshot carries
// only what frozen v8 represents without loss (the invocation policy).
// Nothing is forced through a lossy conversion (ARCH exit gate 5).
//
// Failure channel: port::BundleError (git plumbing failures are
// SourceUnverifiable; policy bytes that do not parse are
// MalformedManifest; bound violations fail closed).
// ============================================================================

#include <qiven/result.hpp>
#include <qiven/runtime/cognition/policy.hpp>
#include <qiven/runtime/identity.hpp>
#include <qiven/runtime/port/cognition_bundle.hpp>
#include <qiven/runtime/processx/process_runner.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace qiven::runtime::cognition
{
inline constexpr usize max_source_files     = 256;
inline constexpr u64 max_source_file_bytes  = 2 * 1024 * 1024;
inline constexpr u64 max_source_total_bytes = 64 * 1024 * 1024;

struct PublishRequest
{
    std::filesystem::path repo_root;    // the qiven-context checkout (plumbing -C)
    std::string ref;                    // exact commit oid or resolvable refspec
    std::filesystem::path runtime_root; // <repo>/.qiven/runtime (GR-5)
    std::string repository_url;         // stamped verbatim into the manifest
    std::string view = "production";
    std::string policy_path;               // git-relative, e.g. runtime/invocation-policy.yaml
    std::vector<std::string> source_paths; // files and directories riding every bundle
    std::string publisher_build;           // runtime build identity
    u64 now_ms = 0;                        // injected clock (no ambient time)
    std::filesystem::path git_executable;
    const processx::ProcessRunner* runner = nullptr;
};

struct PublishResult
{
    std::filesystem::path bundle_dir;
    ContentDigest bundle_digest; // SHA-256 over the written manifest.json bytes
    port::CognitionBundleManifest manifest;
};

class CanonicalCognitionPublisher
{
public:
    [[nodiscard]] qiven::Result<PublishResult, port::BundleError> publish(
        const PublishRequest& request) const;
};
} // namespace qiven::runtime::cognition
