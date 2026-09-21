#pragma once

// ============================================================================
// adapter/handshake.hpp — RuntimeHost admission of an adapter registration
// against the accepted DeploymentProfile (component ADL §10/§11, C-08)
//
// Declared universe + adapter capability manifest + conformance evidence
// must agree (ADL §10). The handshake therefore compares BOTH directions:
//   - a manifest capability not represented (by exact surface) in the
//     profile for this adapter is new surface -> fail closed for its class;
//   - a profile-declared capability for this adapter absent from the
//     manifest is disagreement -> fail closed for its class (the profile no
//     longer describes the actual surface it vouches for).
// The RuntimeHost MUST NOT silently continue under the old profile; the
// result names the affected operation classes explicitly.
// ============================================================================

#include <qiven/runtime/adapter/manifest.hpp>
#include <qiven/runtime/profile.hpp>

#include <vector>

namespace qiven::runtime::adapter
{
enum class HandshakeIssue : u8
{
    UnknownAdapterCredential, // no accepted actor binding carries this adapter+credential
    UnrepresentedInProfile,   // manifest surface the profile does not declare
    AbsentFromManifest,       // profile-declared surface the manifest no longer carries
    SurfaceChanged,           // same capability id, different declared surface
};

struct HandshakeFinding
{
    HandshakeIssue issue = HandshakeIssue::UnrepresentedInProfile;
    CapabilityDescriptor capability {};
};

struct HandshakeResult
{
    bool admitted = false; // true iff no findings at all

    std::vector<HandshakeFinding> findings;

    // Deduplicated operation classes for which the hard-enforcement claim
    // is invalid; the host must run those classes fail-closed.
    std::vector<OperationClass> fail_closed_classes;
};

// Compare one registration against the accepted profile. Pure function; no
// host state is mutated (admission bookkeeping is the host's, not the
// handshake's).
[[nodiscard]] HandshakeResult admit_against_profile(const AdapterManifest& manifest,
                                                    const profile::DeploymentProfile& accepted);
} // namespace qiven::runtime::adapter
