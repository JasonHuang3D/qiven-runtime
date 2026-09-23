#pragma once

// ============================================================================
// adapter/zcode_hook.hpp — the production thin ZCode hook client library
// (MVP-4 batch design section 3.3; ARCH section 6.2)
//
// One-shot per hook invocation: read the event bytes, extract the request
// fields, transact ONE authenticated round-trip (hello + hook_event) over
// the installation pipe, map the verdict to the ZCode exit contract.
//
// Thin-client law (ARCH section 6.2 MUST NOTs): no classification, no
// authoritative session/action counters, no decision issuance, no
// mutations, no failure-to-allow. The ONLY judgment here is the
// mechanical verdict-to-exit mapping; every fail-closed path DENIES for
// pre_tool and exits honestly (never claiming governed status) for the
// advisory events.
// ============================================================================

#include <qiven/types.hpp>

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>

namespace qiven::runtime::adapter
{
// Verdict reason codes (MVP-4 design section 3.2; 110-119 range).
inline constexpr i32 hook_reason_governed_write    = 110;
inline constexpr i32 hook_reason_bash_reference    = 111;
inline constexpr i32 hook_reason_unknown_tool      = 112;
inline constexpr i32 hook_reason_scope_mismatch    = 113;
inline constexpr i32 hook_reason_unknown_session   = 114;
inline constexpr i32 hook_reason_correlation       = 115;
inline constexpr i32 hook_reason_host_unavailable  = 116;
inline constexpr i32 hook_reason_cognition_expired = 117;
inline constexpr i32 hook_reason_payload           = 118;
inline constexpr i32 hook_reason_shutting_down     = 119;

struct HookRun
{
    std::filesystem::path runtime_root; // <governed checkout>/.qiven/runtime
    std::string event;                  // session_start | pre_tool | post_tool
    std::span<const std::byte> payload; // stdin bytes, verbatim
    u64 deadline_ms = 4500;             // ZCode budget minus the 250 ms margin
    u64 now_ms      = 0;
    std::string mediated_tools; // session_start manifest note
};

struct HookOutcome
{
    int exit_code = 0;       // 0 allow/not_governed/degraded; 2 deny
    std::string stderr_text; // deny reason / honesty notes ("" = silent)
};

// The whole hook lifecycle as one call: main() reads stdin, resolves the
// root, calls this, writes stderr_text, exits with exit_code.
[[nodiscard]] HookOutcome run_zcode_hook(const HookRun& run);
} // namespace qiven::runtime::adapter
