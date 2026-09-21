#pragma once

// ============================================================================
// adapter/bridge.hpp — the REAL harness adapter bridge (RCA-14 H1
// prerequisite; component ADL §12 mapping layer)
//
// Maps harness-native events (PreToolUse / PostToolUse / SessionStart —
// adapter MAPPINGS per §12, never architecture-level names) onto the
// four channel contracts:
//
//   SessionStart   -> notify_activation   (§13: preparation only)
//   PreToolUse     -> submit_proposal     (§14: completeness law)
//   PostToolUse    -> report_observation  (§15/§46: one per execution)
//
// The RAW hook payload bytes are the argument blob verbatim: the
// argument digest binds the exact bytes the harness handed over, with
// no parsing risk in between. Parsing is limited to what the contract
// needs from explicit fields the hook command template supplies.
//
// Persistence: the bridge process is one-shot per hook invocation, so
// all adapter state (proposal ledger, single-observation accounting,
// activation counter, evidence) lives in ONE canonical text state file,
// written atomically (temp + rename). The file is the conformance
// evidence artifact — human-inspectable by design.
//
// Conformance mode: verdicts are advisory (exit 0 with a reason note);
// the H1 execution records real traffic; hard-governed deny is a later
// profile decision, not this bridge's to make unilaterally.
// ============================================================================

#include <qiven/result.hpp>
#include <qiven/runtime/adapter/manifest.hpp>
#include <qiven/runtime/observed_action.hpp>
#include <qiven/runtime/port/activation.hpp>
#include <qiven/runtime/port/claims.hpp>
#include <qiven/runtime/port/interception.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace qiven::runtime::adapter
{
// Harness-native event kinds this bridge maps (§12 mappings).
enum class HarnessEvent : u8
{
    SessionStart,
    PreToolUse,
    PostToolUse,
};

struct InterceptCommand
{
    std::string tool_name;              // harness tool identity (e.g. "Bash")
    u64 session_token  = 0;             // harness session identity
    u64 harness_action = 0;             // harness correlation identity (hook
                                        // event counter, supplied by the hook
                                        // template or the bridge's ledger)
    std::vector<std::byte> raw_payload; // the hook's stdin bytes VERBATIM
};

struct ObservationCommand
{
    u64 harness_action = 0;
    // §15 tri-state as established by harness facts:
    //   exit code present + zero    -> Succeeded (evidence "exit 0")
    //   exit code present + nonzero -> Failed (evidence "exit N")
    //   no exit code / timeout /    -> Indeterminate (no evidence)
    //   lost response
    bool has_exit_code = false;
    int exit_code      = 0;
};

struct BridgeReport
{
    bool accepted = false;
    std::string detail; // human-readable, evidence-grade
};

class AdapterBridge final
{
public:
    AdapterBridge(std::filesystem::path state_file, std::string adapter_name, u64 credential_token);

    // --- the four channels -----------------------------------------------

    // SessionStart mapping. Returns the injection-event identity (§66).
    [[nodiscard]] BridgeReport activate(port::ActivationKind kind);

    // PreToolUse mapping: §14 surface from the template-supplied fields +
    // the verbatim payload; records the proposal in the ledger.
    [[nodiscard]] BridgeReport intercept(const InterceptCommand& command);

    // PostToolUse mapping: §15 tri-state from exit-code facts; §46
    // single-observation accounting (a second observation for the same
    // harness action is an integrity conflict).
    [[nodiscard]] BridgeReport observe(const ObservationCommand& command);

    // Claims channel (§16/§67).
    [[nodiscard]] port::ClaimChannelResult route_claim(const port::OutboundClaim& claim) const;

    // --- introspection -----------------------------------------------------

    [[nodiscard]] const AdapterManifest& manifest() const noexcept
    {
        return m_manifest;
    }

    [[nodiscard]] const std::string& declared_window() const noexcept
    {
        return m_window;
    }

    // Full evidence dump (the state file content; lazy-loads).
    [[nodiscard]] std::string evidence_dump();

    [[nodiscard]] bool observation_recorded(u64 harness_action);

    // Lazy-loading accessors: a fresh bridge instance reflects the
    // persisted ledger without an explicit poke.
    [[nodiscard]] u64 proposals();
    [[nodiscard]] u64 activations();

private:
    [[nodiscard]] bool load();
    [[nodiscard]] bool save() const;

    std::filesystem::path m_state_file;
    AdapterManifest m_manifest;
    std::string m_window;

    // ledger state (persisted)
    u64 m_proposals      = 0;
    u64 m_activations    = 0;
    u64 m_next_injection = 1;
    std::vector<u64> m_observed_actions;
    std::vector<std::string> m_event_log; // append-only evidence lines
    bool m_loaded = false;
};
} // namespace qiven::runtime::adapter
