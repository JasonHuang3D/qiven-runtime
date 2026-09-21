#include <qiven/runtime/adapter/loopback.hpp>

#include <qiven/contracts.hpp>
#include <qiven/hashing_sha256.hpp>
#include <qiven/runtime/outcome.hpp>

#include <cstdio>
#include <string>

// ============================================================================
// Adapter conformance suite (RCA-14). The SAME suite runs against the
// LoopbackAdapter (in-process, this file) and — in the H1-designated
// execution — against the REAL harness adapter. A conformance failure is
// a failure of the ADAPTER CONTRACT, wherever it runs.
// ============================================================================

namespace
{
using qiven::u64;
using qiven::runtime::ContentDigest;
using qiven::runtime::ExecutionOutcome;
using qiven::runtime::ObservationStatus;
using qiven::runtime::ObservedAction;
using qiven::runtime::PostActionObservation;
using qiven::runtime::PostActionObserver;
using qiven::runtime::adapter::AdapterManifest;
using qiven::runtime::port::ActivationEvent;
using qiven::runtime::port::ActivationKind;
using qiven::runtime::port::ClaimChannel;
using qiven::runtime::port::ClaimChannelResult;
using qiven::runtime::port::InterceptedProposal;
using qiven::runtime::port::OutboundClaim;

// What any conforming adapter must expose to be driven by this suite.
class AdapterFixture
{
public:
    virtual ~AdapterFixture() = default;

    [[nodiscard]] virtual qiven::runtime::port::IActionInterceptionPort& interception() = 0;
    [[nodiscard]] virtual qiven::runtime::port::IActivationBoundaryPort& activation()   = 0;
    [[nodiscard]] virtual qiven::runtime::port::IOutboundClaimPort& claims()            = 0;
    [[nodiscard]] virtual const AdapterManifest& manifest() const                       = 0;
    [[nodiscard]] virtual const std::string& declared_window() const                    = 0;
    [[nodiscard]] virtual bool observation_recorded(u64 harness_action) const           = 0;
    virtual void record_observation(u64 harness_action)                                 = 0;
};

int g_failures = 0;

void check(bool condition, const char* what)
{
    if (!condition)
    {
        ++g_failures;
        std::printf("[FAIL] conformance: %s\n", what);
    }
}

InterceptedProposal complete_proposal(const std::string& adapter_name)
{
    InterceptedProposal proposal;
    proposal.adapter_name     = adapter_name;
    proposal.session_token    = 7;
    proposal.actor_token      = 11;
    proposal.credential_token = 0xC0DE;
    proposal.capability_id    = 1;
    proposal.operation        = "write_file";
    proposal.target           = "docs/x.md";
    const std::byte args[] { std::byte { 'x' } };
    proposal.arguments.assign(args, args + sizeof args);
    proposal.harness_action = 9001;
    return proposal;
}

int run_adapter_conformance(AdapterFixture& fixture)
{
    // F1: the declared manifest is complete and self-consistent
    {
        const AdapterManifest& manifest = fixture.manifest();
        check(!manifest.adapter_name.empty(), "F1 manifest name");
        check(manifest.manifest_version != 0, "F1 manifest version");
        check(!manifest.capabilities.empty(), "F1 at least one capability");
        check(adapter_instance_id(manifest).fnv != 0, "F1 stable adapter identity");
    }

    // F2 (§14): an incomplete surface is REJECTED, never patched by
    // guessing — each required field individually
    {
        const std::string name = fixture.manifest().adapter_name;

        InterceptedProposal no_operation = complete_proposal(name);
        no_operation.operation.clear();
        check(!fixture.interception().submit_proposal(no_operation).is_ok(), "F2 empty operation rejected");

        InterceptedProposal no_target = complete_proposal(name);
        no_target.target.clear();
        check(!fixture.interception().submit_proposal(no_target).is_ok(), "F2 empty target rejected");

        InterceptedProposal no_session = complete_proposal(name);
        no_session.session_token       = 0;
        check(!fixture.interception().submit_proposal(no_session).is_ok(), "F2 zero session rejected");

        InterceptedProposal no_actor = complete_proposal(name);
        no_actor.actor_token         = 0;
        check(!fixture.interception().submit_proposal(no_actor).is_ok(), "F2 zero actor rejected");

        InterceptedProposal no_capability = complete_proposal(name);
        no_capability.capability_id       = 0;
        check(!fixture.interception().submit_proposal(no_capability).is_ok(), "F2 zero capability rejected");

        InterceptedProposal no_correlation = complete_proposal(name);
        no_correlation.harness_action      = 0;
        check(!fixture.interception().submit_proposal(no_correlation).is_ok(), "F2 zero harness correlation rejected");
    }

    // F3 (§14): a complete surface materializes a full observation with
    // the supplied identities and a digest over the exact arguments
    {
        const std::string name = fixture.manifest().adapter_name;
        auto submitted         = fixture.interception().submit_proposal(complete_proposal(name));
        check(submitted.is_ok(), "F3 complete surface accepted");
        if (submitted.is_ok())
        {
            const ObservedAction& action = submitted.value();
            check(action.session.value == 7, "F3 session identity");
            check(action.actor.value == 11, "F3 actor identity");
            check(action.capability.value == 1, "F3 capability identity");
            check(action.operation == "write_file", "F3 operation");
            check(action.target == "docs/x.md", "F3 target");
            const std::byte args[] { std::byte { 'x' } };
            check(action.argument_digest.sha256 == qiven::sha256(args, sizeof args), "F3 argument digest");
        }
    }

    // F4 (§14): the observed request is IMMUTABLE for the control
    // attempt — the same proposal materializes the identical observation
    {
        const std::string name = fixture.manifest().adapter_name;
        auto first             = fixture.interception().submit_proposal(complete_proposal(name));
        auto second            = fixture.interception().submit_proposal(complete_proposal(name));
        check(first.is_ok() && second.is_ok(), "F4 materialization");
        if (first.is_ok() && second.is_ok())
        {
            check(first.value().argument_digest == second.value().argument_digest, "F4 digest stability");
            check(first.value().operation == second.value().operation, "F4 operation stability");
        }
    }

    // F5 (§46): one correlated observation per exact execution — a second
    // differing observation is an integrity conflict (RCA-9 observer)
    {
        PostActionObserver observer;
        qiven::runtime::ExecutionDecision decision;
        const std::byte payload[] { std::byte { 0x31 } };
        decision.action_digest = ContentDigest { qiven::sha256(payload, sizeof payload) };
        decision.disposition   = qiven::runtime::Disposition::Allow;

        PostActionObservation observation;
        observation.action_digest = decision.action_digest;
        observation.reported      = ExecutionOutcome::Succeeded;
        observation.evidence      = "exit 0";

        const auto accepted = observer.observe(decision, observation, false);
        check(accepted.status == ObservationStatus::Accepted, "F5 first observation accepted");
        const auto conflict = observer.observe(decision, observation, true);
        check(conflict.status == ObservationStatus::RejectedDuplicate, "F5 duplicate rejected (46)");
    }

    // F6 (§15/C-14): "no acknowledgement" is Indeterminate, never Failed
    {
        PostActionObserver observer;
        qiven::runtime::ExecutionDecision decision;
        const std::byte payload[] { std::byte { 0x32 } };
        decision.action_digest = ContentDigest { qiven::sha256(payload, sizeof payload) };
        decision.disposition   = qiven::runtime::Disposition::Allow;

        PostActionObservation observation;
        observation.action_digest = decision.action_digest;
        observation.reported      = ExecutionOutcome::Failed; // even if the
                                                              // adapter SAYS failed
        observation.evidence = "";                            // with no evidence
        const auto result    = observer.observe(decision, observation, false);
        check(result.status == ObservationStatus::Accepted, "F6 accepted");
        check(result.outcome == ExecutionOutcome::Indeterminate, "F6 no-ack is Indeterminate (C-14)");
    }

    // F7 (§16/§67): tool-mediated claims are governed; free-response
    // claims are NotGoverned — never a silent Allow
    {
        OutboundClaim tool_mediated;
        tool_mediated.channel          = ClaimChannel::ToolMediated;
        tool_mediated.channel_identity = "commit-message";
        check(fixture.claims().route_claim(tool_mediated) == ClaimChannelResult::Governed, "F7 tool-mediated governed");

        OutboundClaim free_text;
        free_text.channel = ClaimChannel::FreeResponse;
        check(fixture.claims().route_claim(free_text) == ClaimChannelResult::NotGoverned, "F7 free-response NotGoverned");
    }

    // F8 (§13): activation is tracked and correctness-neutral
    {
        ActivationEvent event;
        event.kind          = ActivationKind::SessionStart;
        const u64 injection = fixture.activation().notify_activation(event);
        check(injection != 0, "F8 activation tracked");
    }

    // F9 (§42 residual): the check-to-execution window is DECLARED — an
    // empty declaration is a conformance defect even when the window is
    // physically zero
    {
        check(!fixture.declared_window().empty(), "F9 residual window declared");
    }

    return g_failures;
}

// --- the loopback fixture (this file's driver) ---
class LoopbackFixture final : public AdapterFixture
{
public:
    LoopbackFixture() :
    m_adapter("loopback", 0xC0DE)
    {
    }

    [[nodiscard]] qiven::runtime::port::IActionInterceptionPort& interception() override
    {
        return m_adapter;
    }
    [[nodiscard]] qiven::runtime::port::IActivationBoundaryPort& activation() override
    {
        return m_adapter;
    }
    [[nodiscard]] qiven::runtime::port::IOutboundClaimPort& claims() override
    {
        return m_adapter;
    }
    [[nodiscard]] const AdapterManifest& manifest() const override
    {
        return m_adapter.manifest();
    }
    [[nodiscard]] const std::string& declared_window() const override
    {
        return m_adapter.check_to_execution_window();
    }
    [[nodiscard]] bool observation_recorded(u64 harness_action) const override
    {
        return m_adapter.observation_recorded(harness_action);
    }
    void record_observation(u64 harness_action) override
    {
        m_adapter.record_observation(harness_action);
    }

private:
    qiven::runtime::adapter::LoopbackAdapter m_adapter;
};
} // namespace

int main()
{
    LoopbackFixture fixture;
    const int failures = run_adapter_conformance(fixture);
    if (failures == 0)
    {
        std::printf("[ OK ] adapter-conformance (loopback)\n");
        return 0;
    }
    std::printf("[FAIL] adapter-conformance (loopback): %d failures\n", failures);
    return 1;
}
