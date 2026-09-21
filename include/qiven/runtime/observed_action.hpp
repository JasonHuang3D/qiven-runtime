#pragma once

// ============================================================================
// observed_action.hpp — what the boundary SAW, not what anyone CLAIMS
// (component ADL §12-§14, design §4; obligations C-01/C-02)
//
// The authority asymmetry is structural (ADR-0038): an ObservedAction is
// adapter-reported mechanism fact — identity fields originate from the
// accepted adapter/session binding, the argument digest binds the exact
// bytes — while ActionIntent (intent.hpp) is a runtime projection the
// control plane derives. The observed action is never edited by
// classification; it is immutable for the whole control attempt.
//
// Day-one the argument blob is ordinary heap storage (std::vector); the
// design's allocator-protocol OwnedVector is a hot-path integration
// deferred until the control-plane cost profile justifies it — recorded as
// a deliberate deviation, not a silent one.
// ============================================================================

#include <qiven/runtime/adapter/manifest.hpp>
#include <qiven/runtime/identity.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace qiven::runtime
{
struct ObservedAction
{
    AdapterInstanceId adapter {};
    HarnessSessionId session {};
    ActorInstanceId actor {}; // from the accepted binding, never NL claims
    CapabilityId capability {};
    std::string operation;
    std::string target;                   // resource identity
    ContentDigest argument_digest {};     // sha256 over the exact argument bytes
    std::vector<std::byte> argument_blob; // retained so decisions bind content
};

// The single construction path: digest and blob are derived together in one
// place, so a decision can never bind a digest whose content the runtime
// does not hold.
[[nodiscard]] ObservedAction observe_action(AdapterInstanceId adapter,
                                            HarnessSessionId session,
                                            ActorInstanceId actor,
                                            CapabilityId capability,
                                            std::string operation,
                                            std::string target,
                                            std::span<const std::byte> arguments);

// The boundary correlation key for one exact observed action (ADL §17).
[[nodiscard]] CorrelationKey correlation_key(const RuntimeGenerationId& generation, const ObservedAction& action,
                                             const HarnessActionId& harness_action) noexcept;
} // namespace qiven::runtime
