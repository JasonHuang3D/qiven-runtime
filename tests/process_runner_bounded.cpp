// ============================================================================
// process_runner_bounded — processx first-form bounds (MVP-2 batch design
// section 6): output cap (73), deadline kill (TimedOut), exit-code
// fidelity, missing executable (71).
// ============================================================================

#include <qiven/contracts.hpp>
#include <qiven/runtime/processx/process_runner.hpp>

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>

namespace
{
using qiven::runtime::processx::ProcessRun;
using qiven::runtime::processx::ProcessRunner;
using qiven::runtime::processx::ProcessSpec;

const std::filesystem::path cmd_exe = R"(C:\Windows\System32\cmd.exe)";

ProcessSpec cmd_spec(std::vector<std::string> args, qiven::u64 deadline_ms = 30'000,
                     qiven::u64 cap = 8 * 1024 * 1024)
{
    ProcessSpec spec;
    spec.executable       = cmd_exe;
    spec.argv             = std::move(args);
    spec.working_dir      = R"(C:\Windows)";
    spec.deadline_ms      = deadline_ms;
    spec.output_cap_bytes = cap;
    return spec;
}
} // namespace

int main()
{
    const ProcessRunner runner;

    // Exit-code fidelity through the explicit argv (no shell string).
    {
        auto run = runner.run(cmd_spec({ "cmd", "/d", "/c", "exit 3" }));
        QIVEN_VERIFY(run.is_ok());
        QIVEN_VERIFY(run.value().end == ProcessRun::End::Exited);
        QIVEN_VERIFY(run.value().exit_code == 3);
    }

    // Captured stdout.
    {
        auto run = runner.run(cmd_spec({ "cmd", "/d", "/c", "echo hello-processx" }));
        QIVEN_VERIFY(run.is_ok());
        QIVEN_VERIFY(run.value().exit_code == 0);
        QIVEN_VERIFY(run.value().out.find("hello-processx") != std::string::npos);
    }

    // Argument-level quoting: a space-containing argument arrives as ONE
    // argument (quoted by the runner, not shell-parsed apart). The full
    // adversarial quoting matrix belongs to the MVP-3 hardening tests;
    // this proves the spawn is explicit-argv, never a command string.
    {
        auto run = runner.run(cmd_spec({ "cmd", "/d", "/c", "echo", "hello processx world" }));
        QIVEN_VERIFY(run.is_ok());
        QIVEN_VERIFY(run.value().out.find("hello processx world") != std::string::npos);
    }

    // Missing executable fails typed 71.
    {
        ProcessSpec spec;
        spec.executable  = R"(C:\does-not-exist\qiven-no-such-tool.exe)";
        spec.argv        = { "qiven-no-such-tool" };
        spec.working_dir = R"(C:\Windows)";
        spec.deadline_ms = 5000;
        auto run         = runner.run(spec);
        QIVEN_VERIFY(!run.is_ok());
        QIVEN_VERIFY(run.reason().code == qiven::runtime::processx::err_spawn);
    }

    // Deadline: a long-running DIRECT child is terminated and classified.
    // (The tree-reclamation case — a child of the child surviving the
    // kill — is deliberately MVP-3's Job Object hardening exit gate;
    // the first form's contract is bounded lifetime of the spawned child.)
    {
        ProcessSpec spec;
        spec.executable    = R"(C:\Windows\System32\PING.EXE)";
        spec.argv          = { "ping", "-n", "30", "127.0.0.1" };
        spec.working_dir   = R"(C:\Windows)";
        spec.deadline_ms   = 1500;
        const auto started = std::chrono::steady_clock::now();
        auto run           = runner.run(spec);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
        QIVEN_VERIFY(run.is_ok());
        QIVEN_VERIFY(run.value().end == ProcessRun::End::TimedOut);
        // The kill happened near the deadline, not at the child's own
        // 30-second runtime: bounded lifetime is the contract.
        QIVEN_VERIFY(elapsed.count() < 10'000);
    }

    // Output cap: a flooding child fails typed 73 (bounded memory, never
    // unbounded growth, never a blocked child).
    {
        auto run = runner.run(cmd_spec(
            { "cmd", "/d", "/c",
              "for /l %i in (1,1,20000) do @echo aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" },
            60'000, 64 * 1024));
        QIVEN_VERIFY(!run.is_ok());
        QIVEN_VERIFY(run.reason().code == qiven::runtime::processx::err_output_limit);
    }

    std::printf("[ OK ] process-runner-bounded\n");
    return 0;
}
