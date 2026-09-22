// ============================================================================
// process_runner_bounds — MVP-3 exit gates 4/5 (ARCH §15) plus the §16.4
// rows the hardened runner owns: tree reclamation, output flooding, the
// executable allowlist, and the environment allowlist.
// ============================================================================

#include <qiven/contracts.hpp>
#include <qiven/runtime/processx/process_runner.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace
{
using qiven::runtime::processx::AllowlistedExecutable;
using qiven::runtime::processx::ProcessRun;
using qiven::runtime::processx::ProcessRunner;
using qiven::runtime::processx::ProcessSpec;

const std::filesystem::path cmd_exe = R"(C:\Windows\System32\cmd.exe)";

ProcessSpec make_spec(std::vector<std::string> args, qiven::u64 deadline_ms = 60'000)
{
    ProcessSpec spec;
    spec.executable  = cmd_exe;
    spec.argv        = std::move(args);
    spec.working_dir = R"(C:\Windows)";
    spec.deadline_ms = deadline_ms;
    return spec;
}
} // namespace

int main()
{
    const ProcessRunner runner;

    // Exit gate 4: a timed-out child AND ITS TREE are reclaimed. cmd wraps
    // ping (a grandchild); the MVP-2 first form let the grandchild survive
    // the direct kill — the Job Object must reclaim the whole tree now.
    {
        const auto started = std::chrono::steady_clock::now();
        auto run           = runner.run(
            make_spec({ "cmd", "/d", "/c", "ping -n 30 127.0.0.1 > nul" }, 2000));
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
        QIVEN_VERIFY(run.is_ok());
        QIVEN_VERIFY(run.value().end == ProcessRun::End::TimedOut);
        QIVEN_VERIFY(elapsed.count() < 12'000); // near the deadline, not ping's 30 s
    }

    // Exit gate 5: large stdout cannot deadlock or grow unbounded memory —
    // the cap is a typed failure (73) with the readers drained.
    {
        ProcessSpec spec = make_spec(
            { "cmd", "/d", "/c",
              "for /l %i in (1,1,40000) do @echo aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" });
        // ~1.7 MB of output against a 1 MiB cap: bounded memory, typed 73.
        spec.output_cap_bytes = 1024 * 1024;
        auto run              = runner.run(spec);
        QIVEN_VERIFY(!run.is_ok());
        QIVEN_VERIFY(run.reason().code == qiven::runtime::processx::err_output_limit);
    }

    // Executable allowlist: unlisted binary → 74; wrong digest → 74.
    {
        ProcessSpec spec = make_spec({ "cmd", "/d", "/c", "echo admitted" });
        spec.allowed_executables.push_back({ R"(C:\Windows\System32\PING.EXE)", "" });
        auto denied = runner.run(spec);
        QIVEN_VERIFY(!denied.is_ok());
        QIVEN_VERIFY(denied.reason().code == qiven::runtime::processx::err_allowlist);

        ProcessSpec wrong_digest = make_spec({ "cmd", "/d", "/c", "echo admitted" });
        wrong_digest.allowed_executables.push_back(
            { R"(C:\Windows\System32\cmd.exe)", std::string(64, '0') });
        auto mismatched = runner.run(wrong_digest);
        QIVEN_VERIFY(!mismatched.is_ok());
        QIVEN_VERIFY(mismatched.reason().code == qiven::runtime::processx::err_allowlist);

        ProcessSpec admitted = make_spec({ "cmd", "/d", "/c", "echo admitted" });
        admitted.allowed_executables.push_back({ R"(C:\Windows\System32\cmd.exe)", "" });
        auto ok = runner.run(admitted);
        QIVEN_VERIFY(ok.is_ok());
        QIVEN_VERIFY(ok.value().exit_code == 0);
    }

    // Environment allowlist: the child environment is EXACTLY the listed
    // entries plus the OS floor — nothing inherited.
    {
        ProcessSpec spec   = make_spec({ "cmd", "/d", "/c", "set" });
        spec.env_allowlist = { { "QIVEN_TEST_MARKER", "42" } };
        auto run           = runner.run(spec);
        QIVEN_VERIFY(run.is_ok());
        QIVEN_VERIFY(run.value().out.find("QIVEN_TEST_MARKER=42") != std::string::npos);
        QIVEN_VERIFY(run.value().out.find("QIVEN_RUNTIME_TEST_WORKROOT") == std::string::npos);
        QIVEN_VERIFY(run.value().out.find("PATH=") == std::string::npos);
        QIVEN_VERIFY(run.value().out.find("SystemRoot=") != std::string::npos); // OS floor
    }

    std::printf("[ OK ] process-runner-bounds\n");
    return 0;
}
