#pragma once

// ============================================================================
// port/interception.hpp — the primary pre-execution boundary (component
// ADL §12/§14)
//
// The harness-specific adapter submits an immutable observation of the
// exact proposed operation here. The adapter must surface ENOUGH DATA to
// construct a complete ObservedAction (actor/session/adapter/capability
// identity, operation, target, structured arguments, harness correlation)
// — a proposal missing any of these is rejected as incomplete, never
// patched up by guessing.
//
// Architecture-level contracts never use harness-native event names
// (PreToolUse/PostToolUse/Stop/PermissionRequest are adapter MAPPINGS,
// §12).
// ============================================================================

#include <qiven/result.hpp>
#include <qiven/runtime/observed_action.hpp>

#include <string>

namespace qiven::runtime::port
{
// The adapter-supplied proposal surface (§14). Everything the control
// plane needs to construct an ObservedAction — supplied by the ADAPTER
// from harness facts, never reconstructed from prose.
struct InterceptedProposal
{
    std::string adapter_name; // adapter identity
    u64 session_token    = 0; // harness session identity
    u64 actor_token      = 0; // from the accepted binding (§8)
    u64 credential_token = 0;
    u64 capability_id    = 0;
    std::string operation;            // operation identity
    std::string target;               // target/resource identity
    std::vector<std::byte> arguments; // structured arguments
    u64 harness_action = 0;           // harness correlation identity
};

enum class ProposalRejection : u8
{
    IncompleteSurface, // a §14 required field is empty/zero — never guessed
};

// Validates that the proposal surface is complete (§14) and constructs
// the immutable ObservedAction for the control attempt. Pure.
[[nodiscard]] qiven::Result<ObservedAction> materialize_proposal(const InterceptedProposal& proposal);

// The interception channel. Day-one shape: submit returns the materialized
// observation to the caller (the ControlCore pipeline takes it from
// there); a real transport delivers dispositions back asynchronously.
class IActionInterceptionPort
{
public:
    virtual ~IActionInterceptionPort() = default;

    [[nodiscard]] virtual qiven::Result<ObservedAction> submit_proposal(const InterceptedProposal& proposal) = 0;
};
} // namespace qiven::runtime::port
