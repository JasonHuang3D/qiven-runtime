#pragma once

// ============================================================================
// adapter/loopback.hpp — LoopbackAdapter: the scripted in-process harness
// model and the conformance reference (design §8 day-one; RCA-14)
//
// The loopback adapter implements all four channels with honest, scripted
// behavior. It exists so the adapter CONTRACT can be proven in-process:
// the conformance suite (tests/adapter_conformance.cpp) is written
// against the channel contracts and runs against this adapter FIRST; the
// REAL harness adapter must pass the SAME suite — that execution is the
// H1-designated part of RCA-14 (isolation boundary: owner's hands).
// ============================================================================

#include <qiven/runtime/adapter/manifest.hpp>
#include <qiven/runtime/outcome.hpp>
#include <qiven/runtime/port/activation.hpp>
#include <qiven/runtime/port/claims.hpp>
#include <qiven/runtime/port/interception.hpp>

#include <string>
#include <unordered_set>
#include <vector>

namespace qiven::runtime::adapter
{
class LoopbackAdapter final : public port::IActionInterceptionPort,
                              public port::IActivationBoundaryPort,
                              public port::IOutboundClaimPort
{
public:
    LoopbackAdapter(std::string name, u64 credential_token);

    // --- IActionInterceptionPort: validates the §14 surface and
    // materializes the immutable observation; records the harness action
    // for single-observation accounting (§46).
    [[nodiscard]] qiven::Result<ObservedAction> submit_proposal(const port::InterceptedProposal& proposal) override;

    // --- IActivationBoundaryPort: tracks injection events for §66
    // receipts; correctness never depends on this (§13).
    [[nodiscard]] u64 notify_activation(const port::ActivationEvent& event) override;

    // --- IOutboundClaimPort: tool-mediated -> Governed (via
    // interception); free-response -> NotGoverned (§16/§67).
    [[nodiscard]] port::ClaimChannelResult route_claim(const port::OutboundClaim& claim) override;

    // The adapter's declared surface for the RCA-1 handshake.
    [[nodiscard]] const AdapterManifest& manifest() const noexcept
    {
        return m_manifest;
    }

    // §42 residual: the declared check-to-execution window of this
    // adapter's dispatch path (an empty declaration is a conformance
    // defect — the window must be DOCUMENTED, even if "in-process,
    // zero").
    [[nodiscard]] const std::string& check_to_execution_window() const noexcept
    {
        return m_window;
    }

    // Observation accounting for conformance: which harness actions have
    // already been observed (§46 single correlated observation).
    [[nodiscard]] bool observation_recorded(u64 harness_action) const noexcept;
    void record_observation(u64 harness_action);

    [[nodiscard]] u64 activations() const noexcept
    {
        return m_activations;
    }

private:
    AdapterManifest m_manifest;
    std::string m_window;
    std::unordered_set<u64> m_observed;
    u64 m_activations          = 0;
    u64 m_next_injection_event = 1;
};
} // namespace qiven::runtime::adapter
