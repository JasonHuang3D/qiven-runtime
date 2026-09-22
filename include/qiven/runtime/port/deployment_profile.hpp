#pragma once

// ============================================================================
// port/deployment_profile.hpp — accepted-profile source port
// (production-MVP architecture §3, MVP-0 definition; the file-backed
// loader lands with MVP-2)
//
// A DeploymentProfile is ACCEPTED configuration, never synthesized from
// prompts (ADL §7). This port defines where accepted profiles come from
// and what acceptance proves: an explicit profile identity, a governed
// ResourceScope enumeration (§3.2), and the conformance-evidence
// reference the acceptance ran against. The profile.hpp value types and
// ProfileBuilder stay the in-code construction path; this port governs
// the production load/accept boundary.
// ============================================================================

#include <qiven/result.hpp>
#include <qiven/runtime/profile.hpp>
#include <qiven/runtime/scope.hpp>

#include <string>

namespace qiven::runtime::port
{
enum class ProfileAcceptErrorKind : u8
{
    Unavailable,
    Malformed,
    EvidenceMissing, // acceptance requires conformance evidence (ADL §7)
    ScopeAmbiguous,  // governed scope must be explicit (§3.2)
    VersionMismatch,
};

struct ProfileAcceptError
{
    ProfileAcceptErrorKind kind = ProfileAcceptErrorKind::Unavailable;
    std::string detail;
};

struct ProfileAcceptRequest
{
    std::string profile_id;           // e.g. "zcode-jason-context-record-mvp"
    u64 expected_revision = 0;        // 0 = accept the current revision
    std::string source;               // accepted source location (file, bundle path)
    ResourceScope governed_scope;     // the explicit enumeration this profile claims
    std::string conformance_evidence; // the acceptance run's evidence reference
};

using ProfileAcceptResult = qiven::Result<profile::DeploymentProfile, ProfileAcceptError>;

class IProfilePort
{
public:
    virtual ~IProfilePort() = default;

    // Load and ACCEPT a profile: the governed scope must be a non-empty
    // explicit enumeration, conformance evidence must be present, and the
    // loaded revision must match the expected one when given. Fails
    // closed — an unacceptable profile never reaches a generation.
    [[nodiscard]] virtual ProfileAcceptResult accept(const ProfileAcceptRequest& request) const = 0;
};
} // namespace qiven::runtime::port
