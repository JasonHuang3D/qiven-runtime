#pragma once

// ============================================================================
// port/claims.hpp — outbound claim channel (component ADL §16/§67)
//
// Two channels stay DISTINCT:
//   tool-mediated claims (commit message, PR text, review verdict,
//   publication payload, structured tool argument) are governed through
//   ORDINARY ACTION INTERCEPTION — they are actions like any other;
//   free-response claims (arbitrary assistant text) are NOT hard-governed
//   in the first profile: the port reports NotGoverned for them, and the
//   DeploymentProfile states this explicitly (C-23 honesty).
// ============================================================================

#include <qiven/runtime/identity.hpp>

#include <string>

namespace qiven::runtime::port
{
enum class ClaimChannel : u8
{
    ToolMediated, // flows through interception as an action
    FreeResponse, // arbitrary assistant text — not hard-governed
};

enum class ClaimChannelResult : u8
{
    Governed,    // routed through the interception boundary
    NotGoverned, // free-response: no hard-enforcement claim (§67)
};

struct OutboundClaim
{
    ClaimChannel channel = ClaimChannel::ToolMediated;
    ContentDigest content_digest {}; // SHA-256 over the claim payload
    std::string channel_identity;    // e.g. "commit-message", "pr-body"
};

class IOutboundClaimPort
{
public:
    virtual ~IOutboundClaimPort() = default;

    // Route one claim. Tool-mediated claims are GOVERNED (through
    // interception — the port does not invent a second governing path);
    // free-response claims return NotGoverned, never a silent Allow.
    [[nodiscard]] virtual ClaimChannelResult route_claim(const OutboundClaim& claim) = 0;
};
} // namespace qiven::runtime::port
