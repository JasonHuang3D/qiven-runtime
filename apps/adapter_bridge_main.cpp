// ============================================================================
// apps/adapter_bridge_main.cpp — qiven-adapter-bridge: the one-shot CLI the
// ZCode hooks invoke (RCA-14 real adapter bridge).
//
// Subcommands (hook mapping per ADL §12):
//   activate --kind <session-start|prompt-submission|task-transition|
//                    workflow-transition|declared-intent>
//   intercept --tool <name> --session <token> --action <id>   [stdin: raw
//              hook payload — digested VERBATIM as the argument blob]
//   observe --action <id> [--exit <code>]    (no --exit = indeterminate)
//   manifest | window | evidence
//
// State: --state <file> (default ./.generated-temp/adapter-bridge/state.log,
// atomic temp+rename writes). Exit 0 on accepted/advisory outcomes, 2 on
// bridge-side rejections (incomplete surface, duplicate observation) with
// the reason on stderr.
// ============================================================================

#include <qiven/runtime/adapter/bridge.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace
{
using qiven::u64;
using qiven::runtime::adapter::AdapterBridge;
using qiven::runtime::adapter::BridgeReport;
using qiven::runtime::port::ActivationKind;

std::vector<std::byte> read_stdin_verbatim()
{
    std::vector<std::byte> bytes;
    char buffer[4096];
    while (std::cin.read(buffer, sizeof buffer) || std::cin.gcount() > 0)
    {
        const auto count = static_cast<std::size_t>(std::cin.gcount());
        bytes.insert(bytes.end(), reinterpret_cast<const std::byte*>(buffer),
                     reinterpret_cast<const std::byte*>(buffer) + count);
        if (!std::cin)
        {
            break;
        }
    }
    return bytes;
}

u64 parse_u64(const char* text)
{
    return std::strtoull(text, nullptr, 10);
}

ActivationKind parse_kind(const std::string& text)
{
    if (text == "prompt-submission")
        return ActivationKind::PromptSubmission;
    if (text == "task-transition")
        return ActivationKind::TaskTransition;
    if (text == "workflow-transition")
        return ActivationKind::WorkflowTransition;
    if (text == "declared-intent")
        return ActivationKind::DeclaredIntent;
    return ActivationKind::SessionStart;
}

int finish(const BridgeReport& report)
{
    if (report.accepted)
    {
        std::printf("%s\n", report.detail.c_str());
        return 0;
    }
    std::fprintf(stderr, "%s\n", report.detail.c_str());
    return 2;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: qiven-adapter-bridge <command> [options]\n");
        return 2;
    }
    const std::string command = argv[1];

    std::filesystem::path state_file = std::filesystem::current_path() / ".generated-temp" / "adapter-bridge" / "state.log";
    std::string tool;
    std::string kind = "session-start";
    u64 session      = 0;
    u64 action       = 0;
    bool has_exit    = false;
    int exit_code    = 0;

    for (int i = 2; i + 1 < argc; i += 2)
    {
        const std::string flag = argv[i];
        const char* value      = argv[i + 1];
        if (flag == "--state")
            state_file = value;
        else if (flag == "--tool")
            tool = value;
        else if (flag == "--kind")
            kind = value;
        else if (flag == "--session")
            session = parse_u64(value);
        else if (flag == "--action")
            action = parse_u64(value);
        else if (flag == "--exit")
        {
            has_exit  = true;
            exit_code = std::atoi(value);
        }
    }

    AdapterBridge bridge(state_file, "zcode-real-adapter", 0xB1216E);

    if (command == "activate")
    {
        return finish(bridge.activate(parse_kind(kind)));
    }
    if (command == "intercept")
    {
        qiven::runtime::adapter::InterceptCommand intercept;
        intercept.tool_name      = tool;
        intercept.session_token  = session;
        intercept.harness_action = action;
        intercept.raw_payload    = read_stdin_verbatim();
        return finish(bridge.intercept(intercept));
    }
    if (command == "observe")
    {
        qiven::runtime::adapter::ObservationCommand observation;
        observation.harness_action = action;
        observation.has_exit_code  = has_exit;
        observation.exit_code      = exit_code;
        return finish(bridge.observe(observation));
    }
    if (command == "manifest")
    {
        std::printf("adapter %s version %u capabilities %zu\n", bridge.manifest().adapter_name.c_str(),
                    bridge.manifest().manifest_version, bridge.manifest().capabilities.size());
        return 0;
    }
    if (command == "window")
    {
        std::printf("%s\n", bridge.declared_window().c_str());
        return 0;
    }
    if (command == "evidence")
    {
        // SILENCE IS A DEFECT (owner-caught): an evidence query must
        // always speak - a missing or empty state is a diagnosable
        // condition, never a blank exit
        std::error_code ec;
        if (!std::filesystem::exists(state_file, ec) || ec)
        {
            std::fprintf(stderr, "bridge state not found: %s\n", state_file.string().c_str());
            std::printf("no hook events recorded yet. Hooks fire only in sessions STARTED AFTER\n"
                        "the config change (workspace hooks do not hot-reload). Verify the hook\n"
                        "commands point at this --state path, then start a new session and make a\n"
                        "few tool calls.\n");
            return 1;
        }
        const std::string dump = bridge.evidence_dump();
        std::printf("bridge-state %s\n", state_file.string().c_str());
        if (dump.empty())
        {
            std::printf("state file exists but recorded no events.\n");
            return 1;
        }
        std::fputs(dump.c_str(), stdout);
        return 0;
    }

    std::fprintf(stderr, "unknown command: %s\n", command.c_str());
    return 2;
}
