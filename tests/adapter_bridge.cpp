// ============================================================================
// adapter_bridge.cpp — REAL adapter bridge tests (RCA-14 H1 prerequisite):
// §14 completeness, verbatim-payload digest binding, §46 single-observation
// across bridge instances, §15 tri-state mapping from exit-code facts,
// state persistence (fresh object, same file), claims routing, activation,
// declared window, and the evidence dump.
// ============================================================================

#include <qiven/runtime/adapter/bridge.hpp>

#include <qiven/contracts.hpp>
#include <qiven/hashing_sha256.hpp>

#include <cstdio>
#include <filesystem>

namespace
{
using qiven::u64;
using qiven::runtime::adapter::AdapterBridge;
using qiven::runtime::adapter::InterceptCommand;
using qiven::runtime::adapter::ObservationCommand;
using qiven::runtime::port::ClaimChannel;
using qiven::runtime::port::OutboundClaim;

class TempState
{
public:
    explicit TempState(std::string name) :
    m_path(std::filesystem::temp_directory_path() / name)
    {
        std::error_code ignored;
        std::filesystem::remove(m_path, ignored);
    }
    ~TempState()
    {
        std::error_code ignored;
        std::filesystem::remove(m_path, ignored);
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return m_path;
    }

private:
    std::filesystem::path m_path;
};

InterceptCommand proposal_of(u64 action, char payload)
{
    InterceptCommand command;
    command.tool_name      = "Bash";
    command.session_token  = 42;
    command.harness_action = action;
    const std::byte bytes[] { static_cast<std::byte>(payload) };
    command.raw_payload.assign(bytes, bytes + sizeof bytes);
    return command;
}

std::string digest_of(char payload)
{
    const std::byte bytes[] { static_cast<std::byte>(payload) };
    return qiven::runtime::to_hex(qiven::runtime::ContentDigest { qiven::sha256(bytes, sizeof bytes) });
}
} // namespace

int main()
{
    TempState state("qiven-rca14-bridge-state.log");

    // §14 completeness law: each missing identity field rejects
    {
        AdapterBridge bridge(state.path(), "zcode-real-adapter", 0xB1216E);

        InterceptCommand no_tool = proposal_of(1, 'x');
        no_tool.tool_name.clear();
        QIVEN_VERIFY(!bridge.intercept(no_tool).accepted);

        InterceptCommand no_session = proposal_of(1, 'x');
        no_session.session_token    = 0;
        QIVEN_VERIFY(!bridge.intercept(no_session).accepted);

        InterceptCommand no_action = proposal_of(0, 'x');
        QIVEN_VERIFY(!bridge.intercept(no_action).accepted);

        QIVEN_VERIFY(bridge.proposals() == 0); // nothing recorded on rejects
    }

    // accepted proposal binds the VERBATIM payload digest
    {
        AdapterBridge bridge(state.path(), "zcode-real-adapter", 0xB1216E);
        const auto report = bridge.intercept(proposal_of(10, 'q'));
        QIVEN_VERIFY(report.accepted);
        QIVEN_VERIFY(report.detail.find(digest_of('q')) != std::string::npos);
        QIVEN_VERIFY(bridge.proposals() == 1);

        // same payload -> same digest; different -> different
        const auto same = bridge.intercept(proposal_of(11, 'q'));
        QIVEN_VERIFY(same.detail.find(digest_of('q')) != std::string::npos);
        const auto other = bridge.intercept(proposal_of(12, 'z'));
        QIVEN_VERIFY(other.detail.find(digest_of('z')) != std::string::npos);
    }

    // §46 single-observation ACROSS INSTANCES (fresh object, same file):
    // the second bridge sees the first's ledger
    {
        {
            AdapterBridge first(state.path(), "zcode-real-adapter", 0xB1216E);
            ObservationCommand observation;
            observation.harness_action = 10;
            observation.has_exit_code  = true;
            observation.exit_code      = 0;
            QIVEN_VERIFY(first.observe(observation).accepted);
        }
        AdapterBridge second(state.path(), "zcode-real-adapter", 0xB1216E);
        QIVEN_VERIFY(second.observation_recorded(10));
        QIVEN_VERIFY(!second.observe({ 10, true, 0 }).accepted); // duplicate
    }

    // §15 tri-state from exit-code facts
    {
        AdapterBridge bridge(state.path(), "zcode-real-adapter", 0xB1216E);

        ObservationCommand indeterminate;
        indeterminate.harness_action = 20; // no exit code
        QIVEN_VERIFY(bridge.observe(indeterminate).accepted);

        ObservationCommand succeeded;
        succeeded.harness_action = 21;
        succeeded.has_exit_code  = true;
        QIVEN_VERIFY(bridge.observe(succeeded).accepted);

        ObservationCommand failed;
        failed.harness_action = 22;
        failed.has_exit_code  = true;
        failed.exit_code      = 3;
        QIVEN_VERIFY(bridge.observe(failed).accepted);

        const std::string dump = bridge.evidence_dump();
        QIVEN_VERIFY(dump.find("observation 20 indeterminate") != std::string::npos);
        QIVEN_VERIFY(dump.find("observation 21 succeeded exit 0") != std::string::npos);
        QIVEN_VERIFY(dump.find("observation 22 failed exit 3") != std::string::npos);
    }

    // persistence: proposals, activations and the injection counter survive
    // a fresh instance (one-shot process semantics)
    {
        u64 proposals_before   = 0;
        u64 activations_before = 0;
        {
            AdapterBridge bridge(state.path(), "zcode-real-adapter", 0xB1216E);
            QIVEN_VERIFY(bridge.activate(qiven::runtime::port::ActivationKind::SessionStart).accepted);
            QIVEN_VERIFY(bridge.intercept(proposal_of(30, 'p')).accepted);
            proposals_before   = bridge.proposals();
            activations_before = bridge.activations();
            QIVEN_VERIFY(proposals_before > 0);
            QIVEN_VERIFY(activations_before > 0);
        }
        AdapterBridge reloaded(state.path(), "zcode-real-adapter", 0xB1216E);
        QIVEN_VERIFY(reloaded.proposals() == proposals_before);
        QIVEN_VERIFY(reloaded.activations() == activations_before);
        QIVEN_VERIFY(reloaded.activate(qiven::runtime::port::ActivationKind::PromptSubmission).accepted);
        QIVEN_VERIFY(reloaded.activations() == activations_before + 1);
    }

    // claims routing (§16/§67) and declared window (§42)
    {
        AdapterBridge bridge(state.path(), "zcode-real-adapter", 0xB1216E);

        OutboundClaim tool_mediated;
        tool_mediated.channel = ClaimChannel::ToolMediated;
        QIVEN_VERIFY(bridge.route_claim(tool_mediated) == qiven::runtime::port::ClaimChannelResult::Governed);

        OutboundClaim free_text;
        free_text.channel = ClaimChannel::FreeResponse;
        QIVEN_VERIFY(bridge.route_claim(free_text) == qiven::runtime::port::ClaimChannelResult::NotGoverned);

        QIVEN_VERIFY(!bridge.declared_window().empty());
        QIVEN_VERIFY(!bridge.manifest().capabilities.empty());
        QIVEN_VERIFY(bridge.manifest().adapter_name == "zcode-real-adapter");
    }

    std::printf("[ OK ] adapter-bridge\n");
    return 0;
}
