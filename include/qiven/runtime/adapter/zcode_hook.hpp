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

#include <qiven/error.hpp>
#include <qiven/types.hpp>

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

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
// Transport diagnosability split (2026-09-24 corrective lane; the deny-116
// incident: every transport cause collapsed into one undifferentiated
// code). The classes are DISJOINT; 116 remains only for genuinely unknown
// host-unavailability shapes (post-connect silent death, unexpected reply
// shapes) and never silently absorbs a classable cause.
inline constexpr i32 hook_reason_no_listener   = 120; // pipe connect: no host serving
inline constexpr i32 hook_reason_admission     = 121; // host rejected the client image
inline constexpr i32 hook_reason_version_skew  = 122; // protocol version mismatch
inline constexpr i32 hook_reason_secret_skew   = 123; // HMAC fails (stale/different root)
inline constexpr i32 hook_reason_timeout       = 124; // no reply within the deadline

struct HookRun
{
    std::filesystem::path runtime_root; // <governed checkout>/.qiven/runtime
    std::string event;                  // session_start | pre_tool | post_tool
    // OWNED stdin bytes, verbatim (2026-09-23 trial-2 incident: a span
    // member over a caller's temporary vector dangled and fed freed-heap
    // pointers to the extractor — the deny-118 recurrence; captured by
    // the kit's --dump-stdin probe). The run OWNS the payload: this bug
    // class is now unconstructible.
    std::vector<std::byte> payload;
    u64 deadline_ms = 4500; // ZCode budget minus the 250 ms margin
    u64 now_ms      = 0;
    std::string mediated_tools; // session_start manifest note
    // Trusted registration template (2026-09-23 deny-118 correction): the
    // identity fields come from the hook COMMAND LINE the workspace config
    // pins per matcher entry — never from payload fields, which are
    // corroborating evidence only (the real payload carries no stable
    // session/event identity; RCA-14 H1 evidence).
    std::string session_handle; // --session-handle (evidence-grade token)
    std::string tool;           // --tool (the matcher's tool name)
};

struct HookOutcome
{
    int exit_code = 0;       // 0 allow/not_governed/degraded; 2 deny
    std::string stderr_text; // deny reason / honesty notes ("" = silent)
};

// The whole hook lifecycle as one call: main() reads stdin, resolves the
// root, calls this, writes stderr_text, exits with exit_code.
[[nodiscard]] HookOutcome run_zcode_hook(const HookRun& run);

// Classify an IPC transport/handshake failure into the diagnosable deny
// code (120-124; 116 for genuinely unknown shapes). Exposed for the
// conformance regression table. The mapping is honest string evidence:
// the messages matched are produced by this repository's ipc layer.
[[nodiscard]] i32 classify_transport_failure(const qiven::Error& error,
                                             bool read_deadline_expired = false);
} // namespace qiven::runtime::adapter
