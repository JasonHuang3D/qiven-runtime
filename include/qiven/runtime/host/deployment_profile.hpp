#pragma once

// ============================================================================
// host/deployment_profile.hpp — file-backed accepted-profile source
// (MVP-2 batch design section 3.5; ARCH section 3; cpp-design section 9)
//
// A DeploymentProfile is ACCEPTED configuration, never synthesized. The
// accepted instance is a repository file (schema
// qiven-deployment-profile-v1, block-style bounded parser — cpp-design
// D-4: the runtime never gains a general YAML parser). Loading proves:
// non-empty explicit governed scope (glob-free, ARCH section 3.2),
// present conformance evidence (ADL section 7), revision match when
// expected, and structural consistency (mediation references declared
// capabilities). The profile value is then built through ProfileBuilder
// — the in-code builder remains the only construction path.
//
// The file also carries the cognition-delivery facts the publisher
// consumes (repository, authorized ref, policy path + hard-gate digest,
// source paths, git executable) — exposed via load(); the port's
// accept() cross-checks the caller's acceptance assertions against the
// file before a profile exists.
// ============================================================================

#include <qiven/result.hpp>
#include <qiven/runtime/port/deployment_profile.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace qiven::runtime::host
{
inline constexpr usize max_profile_bytes = 128 * 1024;

struct ProfileCognition
{
    std::string repository;
    std::string authorized_ref;
    std::string policy_path;
    std::string policy_sha256; // hard-gate digest (ARCH section 7.3)
    std::vector<std::string> source_paths;
};

// MVP-4 (batch design section 3.5): the write-capable tool entry points
// the hook mediates, declared as profile DATA — the complete-mediation
// claim is checked against this enumeration, never against code.
struct ToolInventoryEntry
{
    std::string tool;       // harness tool name, e.g. "Bash"
    u64 capability = 0;     // the declared capability id it maps to
    std::string extraction; // command | file_path
    std::string detector;   // exact_path | conservative_text_reference
};

struct ProfileFile
{
    std::string profile_id;
    u64 revision                     = 0;
    u64 control_version              = 0;
    u64 resolver_registry_revision   = 0;
    u64 classifier_contract_revision = 0;
    std::string conformance_evidence;
    u64 freshness_window_ms = 0;
    std::vector<std::string> governed_paths;
    ProfileCognition cognition;
    std::filesystem::path git_executable;
    u64 git_min_version_major_minor = 0;            // (major << 16) | minor, 0 = unrecorded
    std::vector<ToolInventoryEntry> tool_inventory; // MVP-4
    std::string record_launcher;                    // accepted qiven-record grammar pointer (MVP-4)
    // The built profile value (valid only when load()/accept() succeeded).
    profile::DeploymentProfile built;
};

using ProfileFileResult = qiven::Result<ProfileFile, port::ProfileAcceptError>;

// Parses and validates the accepted instance WITHOUT the caller-assertion
// cross-checks (accept() adds those). The returned struct carries the
// cognition-delivery facts the publisher consumes.
[[nodiscard]] ProfileFileResult load_profile_file(const std::filesystem::path& file);

class FileProfileSource final : public port::IProfilePort
{
public:
    // accept(): the request's assertions must match the loaded instance —
    // identity, revision (when expected_revision != 0), the governed
    // scope enumeration (field-for-field), and present conformance
    // evidence. Any disagreement denies (ScopeAmbiguous / EvidenceMissing
    // / VersionMismatch); an unacceptable profile never reaches a
    // generation.
    [[nodiscard]] port::ProfileAcceptResult accept(
        const port::ProfileAcceptRequest& request) const override;
};
} // namespace qiven::runtime::host
