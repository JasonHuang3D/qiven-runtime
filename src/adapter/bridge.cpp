#include <qiven/runtime/adapter/bridge.hpp>

#include <qiven/hashing.hpp>
#include <qiven/hashing_sha256.hpp>

#include <algorithm>
#include <fstream>

namespace qiven::runtime::adapter
{
namespace
{
std::string hex_digest(const std::vector<std::byte>& payload)
{
    const ContentDigest digest { sha256(payload.data(), payload.size()) };
    return to_hex(digest);
}

constexpr const char* activation_kind_name(port::ActivationKind kind)
{
    switch (kind)
    {
    case port::ActivationKind::SessionStart: return "session-start";
    case port::ActivationKind::PromptSubmission: return "prompt-submission";
    case port::ActivationKind::TaskTransition: return "task-transition";
    case port::ActivationKind::WorkflowTransition: return "workflow-transition";
    case port::ActivationKind::DeclaredIntent: return "declared-intent";
    }
    return "unknown";
}
} // namespace

AdapterBridge::AdapterBridge(std::filesystem::path state_file, std::string adapter_name, u64 credential_token) :
m_state_file(std::move(state_file))
{
    m_manifest.adapter_name     = std::move(adapter_name);
    m_manifest.manifest_version = 1;
    m_manifest.credential_token = credential_token;

    CapabilityDescriptor tool_actions {};
    tool_actions.id              = CapabilityId { 1 };
    tool_actions.adapter         = adapter_instance_id(m_manifest);
    tool_actions.operation_class = OperationClass::Observe;
    m_manifest.capabilities.push_back(tool_actions);

    m_window = "hook process boundary: the residual check-to-execution window "
               "spans bridge-exit to tool dispatch and is harness-owned; this "
               "adapter cannot prove it, only declare it";
}

bool AdapterBridge::load()
{
    if (m_loaded)
    {
        return true;
    }
    m_loaded = true;
    std::error_code ec;
    if (!std::filesystem::exists(m_state_file, ec) || ec)
    {
        return true; // fresh state
    }
    std::ifstream input(m_state_file);
    if (!input)
    {
        return false;
    }
    std::string line;
    while (std::getline(input, line))
    {
        m_event_log.push_back(line);
        if (line.rfind("proposal ", 0) == 0)
        {
            ++m_proposals;
        }
        else if (line.rfind("activation ", 0) == 0)
        {
            ++m_activations;
            const u64 id = std::strtoull(line.c_str() + 10, nullptr, 10);
            if (id >= m_next_injection)
            {
                m_next_injection = id + 1;
            }
        }
        else if (line.rfind("observation ", 0) == 0)
        {
            const u64 action = std::strtoull(line.c_str() + 12, nullptr, 10);
            m_observed_actions.push_back(action);
        }
    }
    return true;
}

bool AdapterBridge::save() const
{
    std::error_code ec;
    std::filesystem::create_directories(m_state_file.parent_path(), ec);
    const std::filesystem::path temp = m_state_file;
    const std::string temp_name      = temp.string() + ".tmp";
    {
        std::ofstream out(temp_name, std::ios::trunc);
        if (!out)
        {
            return false;
        }
        for (const std::string& line : m_event_log)
        {
            out << line << '\n';
        }
    }
    std::filesystem::rename(temp_name, m_state_file, ec); // atomic replace
    return !ec;
}

BridgeReport AdapterBridge::activate(port::ActivationKind kind)
{
    if (!load() || !save())
    {
        return { false, "state file unavailable" };
    }
    // §13: activation is preparation only — the event identity is what a
    // §66 receipt would cite; correctness never depends on this
    const u64 injection_event = m_next_injection++;
    ++m_activations;
    m_event_log.push_back("activation " + std::to_string(injection_event) + " " + activation_kind_name(kind));
    if (!save())
    {
        return { false, "state write failed" };
    }
    return { true, "injection-event " + std::to_string(injection_event) };
}

BridgeReport AdapterBridge::intercept(const InterceptCommand& command)
{
    if (!load() || !save())
    {
        return { false, "state file unavailable" };
    }

    // §14 completeness law: template-supplied fields must be complete —
    // the bridge never guesses a missing identity
    if (command.tool_name.empty() || command.session_token == 0 || command.harness_action == 0)
    {
        m_event_log.push_back("rejected-proposal incomplete-surface");
        save();
        return { false, "incomplete interception surface (ADL 14)" };
    }

    port::InterceptedProposal proposal;
    proposal.adapter_name     = m_manifest.adapter_name;
    proposal.session_token    = command.session_token;
    proposal.actor_token      = 1; // the accepted binding of this adapter
    proposal.credential_token = m_manifest.credential_token;
    proposal.capability_id    = 1;
    proposal.operation        = command.tool_name;
    proposal.target           = "harness:" + command.tool_name;
    proposal.arguments        = command.raw_payload; // VERBATIM hook bytes
    proposal.harness_action   = command.harness_action;

    auto action = materialize_proposal(proposal);
    if (!action.is_ok())
    {
        m_event_log.push_back("rejected-proposal materialization-failed");
        save();
        return BridgeReport { false, action.reason().message };
    }

    ++m_proposals;
    m_event_log.push_back("proposal " + std::to_string(command.harness_action) + " " + command.tool_name + " session " +
                          std::to_string(command.session_token) + " sha256 " + hex_digest(command.raw_payload));
    if (!save())
    {
        return { false, "state write failed" };
    }
    return { true, "accepted; argument digest sha256 " + hex_digest(command.raw_payload) };
}

BridgeReport AdapterBridge::observe(const ObservationCommand& command)
{
    if (!load() || !save())
    {
        return { false, "state file unavailable" };
    }

    // §46: one correlated observation per exact execution
    if (std::find(m_observed_actions.begin(), m_observed_actions.end(), command.harness_action) != m_observed_actions.end())
    {
        m_event_log.push_back("rejected-observation duplicate " + std::to_string(command.harness_action));
        save();
        return { false, "duplicate observation (ADL 46)" };
    }

    // §15 tri-state from harness facts: an absent exit code is
    // Indeterminate — never Failed, never a retry permission
    const char* outcome = "indeterminate";
    std::string evidence;
    if (command.has_exit_code)
    {
        if (command.exit_code == 0)
        {
            outcome  = "succeeded";
            evidence = "exit 0";
        }
        else
        {
            outcome  = "failed";
            evidence = "exit " + std::to_string(command.exit_code);
        }
    }

    m_observed_actions.push_back(command.harness_action);
    m_event_log.push_back("observation " + std::to_string(command.harness_action) + " " + outcome +
                          (evidence.empty() ? "" : " " + evidence));
    if (!save())
    {
        return { false, "state write failed" };
    }
    return { true, std::string("outcome ") + outcome };
}

port::ClaimChannelResult AdapterBridge::route_claim(const port::OutboundClaim& claim) const
{
    if (claim.channel == port::ClaimChannel::ToolMediated)
    {
        return port::ClaimChannelResult::Governed;
    }
    return port::ClaimChannelResult::NotGoverned; // §67: never silent Allow
}

std::string AdapterBridge::evidence_dump()
{
    if (!load())
    {
        return "state file unavailable";
    }
    std::string out;
    for (const std::string& line : m_event_log)
    {
        out += line;
        out += '\n';
    }
    return out;
}

u64 AdapterBridge::proposals()
{
    load();
    return m_proposals;
}

u64 AdapterBridge::activations()
{
    load();
    return m_activations;
}

bool AdapterBridge::observation_recorded(u64 harness_action)
{
    return load() && std::find(m_observed_actions.begin(), m_observed_actions.end(), harness_action) !=
                         m_observed_actions.end();
}

} // namespace qiven::runtime::adapter
