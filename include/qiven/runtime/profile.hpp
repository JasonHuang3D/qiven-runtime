#pragma once

// ============================================================================
// profile.hpp — DeploymentProfile: immutable, versioned runtime
// configuration (component ADL §7-§10)
//
// A profile is ACCEPTED configuration, never synthesized from prompts
// (ADL §7). It defines the closed universe against which complete mediation
// is claimed: who is governed (GovernedActorSet, from accepted
// adapter/session bindings — never natural-language identity claims), what
// they can reach (CapabilityUniverse), and how each capability is
// intercepted (MediationInventory).
//
// The first hard-governed landing consumes only an accepted,
// conformance-proven profile. ProfileBuilder is C++ code.
// ============================================================================

#include <qiven/result.hpp>
#include <qiven/runtime/adapter/manifest.hpp>
#include <qiven/runtime/identity.hpp>

#include <string>
#include <vector>

namespace qiven::runtime::profile
{
// An accepted adapter/session binding. Actor identity originates ONLY here
// (ADL §8); statements like "I am the worker" are not runtime
// authentication.
struct ActorBinding
{
    AdapterInstanceId adapter {};
    u64 session_token    = 0; // accepted harness-session principal
    u64 credential_token = 0; // day-one fixed loopback credential
};

struct GovernedActorSet
{
    u64 revision = 0;
    std::vector<ActorBinding> actors;
};

struct CapabilityUniverse
{
    u64 revision = 0;
    std::vector<adapter::CapabilityDescriptor> capabilities; // everything exposed to the actors
};

// How a capability is intercepted (ADL §10). A governed class is fully
// mediated only when every producing capability carries a real interception
// path; Unmediated entries exist so gaps are explicit, not silent.
enum class MediationKind : u8
{
    ActionInterception, // observable pre-action boundary with deny semantics
    ClaimChannelOnly,   // tool-mediated claim channel, no action surface
    Unmediated,         // declared in the universe but NOT claimed mediated
};

struct MediationEntry
{
    CapabilityId capability {};
    MediationKind kind = MediationKind::Unmediated;
};

struct MediationInventory
{
    u64 revision = 0;
    std::vector<MediationEntry> entries;
};

// Claim-channel coverage of the profile (C-23 claim honesty): the first
// landing claims tool-mediated channels only; free-text claims are
// explicitly NotGoverned.
struct ClaimChannelCoverage
{
    bool tool_mediated = false;
    bool free_text     = false;
};

struct DeploymentProfile
{
    std::string name;
    ProfileRevision revision {};
    GovernedActorSet actors;
    CapabilityUniverse capabilities;
    MediationInventory mediation;
    ClaimChannelCoverage claims;

    // Accepted binding revisions recorded by the profile (ADL §7). The
    // resolver registry and classifier contracts land with RCA-2..RCA-4;
    // their accepted revisions bind here from day one so a generation
    // change is always explicit (ADL §6).
    u64 resolver_registry_revision   = 0;
    u64 classifier_contract_revision = 0;
    u64 control_version              = 0; // control implementation/version identity

    // Conformance evidence reference for the accepted profile (ADL §7).
    std::string conformance_evidence;
};

// C++ profile construction (ADL §7: no prompt synthesis). build() validates:
// unique capability ids, unique actor bindings, mediation entries
// referencing declared capabilities, and a non-empty name/revision.
class ProfileBuilder
{
public:
    ProfileBuilder& set_name(std::string name);
    ProfileBuilder& set_revision(ProfileRevision revision);
    ProfileBuilder& set_actor_set(GovernedActorSet actors);
    ProfileBuilder& set_capability_universe(CapabilityUniverse capabilities);
    ProfileBuilder& set_mediation_inventory(MediationInventory mediation);
    ProfileBuilder& set_claims(ClaimChannelCoverage claims);
    ProfileBuilder& set_control_version(u64 version);
    ProfileBuilder& set_conformance_evidence(std::string reference);

    [[nodiscard]] qiven::Result<DeploymentProfile> build();

private:
    DeploymentProfile m_partial;
};
} // namespace qiven::runtime::profile
