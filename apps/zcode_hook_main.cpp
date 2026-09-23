// ============================================================================
// apps/zcode_hook_main.cpp — qiven-zcode-hook, the production thin ZCode
// hook client (MVP-4 batch design section 3.3; ARCH section 6.2)
//
//   qiven-zcode-hook --event <session_start|pre_tool|post_tool>
//                    --root <governed qiven-context checkout>
//
// Reads the harness event from stdin (VERBATIM: the payload digest binds
// the exact bytes), transacts one authenticated round-trip with the
// RuntimeHost, maps the verdict to the ZCode exit contract, and exits:
//   exit 0 — allow / not_governed / degraded-advisory (stderr note only)
//   exit 2 — deny (stderr: "[qiven] deny <code>: <detail>")
//
// The hook NEVER classifies, counts, decides, or converts a failure into
// an allow. --root may come from QIVEN_CONTEXT_ROOT when the flag is
// absent. Exit codes: 0 pass, 2 deny/usage-failure.
// ============================================================================

#include <qiven/runtime/adapter/zcode_hook.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace
{
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
} // namespace

int main(int argc, char** argv)
{
    std::string event;
    std::filesystem::path root;
    for (int i = 1; i + 1 < argc; i += 2)
    {
        const std::string flag = argv[i];
        if (flag == "--event")
        {
            event = argv[i + 1];
        }
        else if (flag == "--root")
        {
            root = argv[i + 1];
        }
    }
    if (root.empty())
    {
        if (const char* env = std::getenv("QIVEN_CONTEXT_ROOT"); env != nullptr)
        {
            root = env;
        }
    }
    if (event.empty() ||
        (event != "session_start" && event != "pre_tool" && event != "post_tool") ||
        root.empty())
    {
        std::fprintf(stderr,
                     "usage: qiven-zcode-hook --event <session_start|pre_tool|post_tool> "
                     "--root <governed checkout>\n");
        return 2;
    }

    qiven::runtime::adapter::HookRun run;
    run.runtime_root   = root / ".qiven" / "runtime";
    run.event          = event;
    run.payload        = read_stdin_verbatim();
    run.deadline_ms    = event == "session_start" ? 9750 : 4750; // minus the 250 ms margin
    run.mediated_tools = "Bash,Write,Edit";                      // the declared manifest surface (profile-checked)
    run.now_ms         = static_cast<qiven::u64>(0);             // transport stamps time; host uses its clock

    const auto outcome = qiven::runtime::adapter::run_zcode_hook(run);
    if (!outcome.stderr_text.empty())
    {
        std::fputs(outcome.stderr_text.c_str(), stderr);
        std::fputc('\n', stderr);
    }
    return outcome.exit_code;
}
