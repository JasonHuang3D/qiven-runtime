#pragma once

// ============================================================================
// processx/process_runner.hpp — bounded child execution (first form)
// (MVP-2 batch design section 3.2; cpp-design section 11; ARCH section
// 6.5 — the minimal qiven-process slice)
//
// Runs ONE allowlisted-style executable with an explicit argv — never a
// shell command string — with bounded lifetime and captured outputs:
//   - CreateProcessW, CREATE_NO_WINDOW, no inherited stdin (closed pipe);
//   - two dedicated reader threads with per-stream caps; cap overflow is
//     a typed output-limit failure (73), never unbounded growth;
//   - a hard deadline; expiry terminates the child and classifies the
//     run TimedOut with whatever was captured (72-class at callers that
//     need a failure rather than a classification).
//
// MVP-2 hardening deferral (batch design C-2): Job Object kill-on-close
// tree reclamation, environment allowlist, and executable allowlist by
// version/digest land with MVP-3; until then this runner is the
// git-plumbing executor, not a general task runner.
//
// Failure channel: qiven::Result with process-runner codes 71-74
// (cpp-design section 5). Not thread-safe as an object; safe to call
// from one thread at a time (const, no shared mutable state).
// ============================================================================

#include <qiven/result.hpp>
#include <qiven/types.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace qiven::runtime::processx
{
inline constexpr i32 err_spawn        = 71;
inline constexpr i32 err_deadline     = 72;
inline constexpr i32 err_output_limit = 73;
inline constexpr i32 err_allowlist    = 74;

inline constexpr u64 default_deadline_ms      = 30'000;
inline constexpr u64 default_output_cap_bytes = 8 * 1024 * 1024;

struct ProcessSpec
{
    std::filesystem::path executable; // absolute; the caller (profile) owns allowlisting
    std::vector<std::string> argv;    // explicit arguments; argv[0] is the program name
    std::filesystem::path working_dir;
    u64 deadline_ms      = default_deadline_ms;
    u64 output_cap_bytes = default_output_cap_bytes;
};

struct ProcessRun
{
    enum class End : u8
    {
        Exited,   // observed exit code is authoritative
        TimedOut, // deadline expired; the child was terminated; partial output retained
        SpawnFailed
    };

    End end       = End::Exited;
    i32 exit_code = 0;
    std::string out;
    std::string err;
};

class ProcessRunner
{
public:
    // Executes the spec. Returns:
    //   - ok(ProcessRun{Exited}) with the child's exit code;
    //   - ok(ProcessRun{TimedOut}) when the deadline expired (the child
    //     was terminated; callers decide whether that is a failure —
    //     git plumbing maps it to a source-unverifiable denial);
    //   - fail(71) when the process cannot be spawned (missing image,
    //     bad working dir);
    //   - fail(73) when a stream exceeded its cap (the child is
    //     terminated; output is genuinely anomalous, not truncated-ok).
    [[nodiscard]] qiven::Result<ProcessRun> run(const ProcessSpec& spec) const;
};
} // namespace qiven::runtime::processx
