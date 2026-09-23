#pragma once

// ============================================================================
// adapter/zcode_event.hpp — locate-grade extraction of the fields the
// production ZCode hook needs from a harness event payload
// (MVP-4 batch design section 3.1, recorded delta 1.1)
//
// The RAW hook payload bytes are digested VERBATIM (the bridge
// precedent: the argument digest binds exactly what the harness handed
// over). Extraction is a bounded state-machine SCAN, not a validating
// parse: the real ZCode payload may contain signed or floating values
// the jsonx machine-file law rejects, and full-document validation of
// harness input is explicitly not a claim of this batch. The scan is
// string/escape/depth-aware so it can SKIP structure it does not need;
// the governed surface on the wire is the digest plus the extracted
// fields, which the host decodes strictly.
// ============================================================================

#include <qiven/result.hpp>
#include <qiven/runtime/identity.hpp>

#include <cstddef>
#include <span>
#include <string>

namespace qiven::runtime::adapter
{
inline constexpr usize max_hook_payload_bytes = 1024 * 1024;
inline constexpr u32 max_hook_payload_depth   = 16;

// Extraction failure codes (hook-domain, MVP-4 design section 3.2).
inline constexpr i32 hook_err_payload_oversize   = 118;
inline constexpr i32 hook_err_payload_unreadable = 118;

struct ZcodeEventFields
{
    std::string session_handle;      // harness session id — EVIDENCE ONLY
    std::string event_name;          // cross-checks the --event flag
    std::string tool_name;           // "Bash" | "Write" | "Edit" | other
    std::string command;             // Bash: tool_input.command (when present)
    std::string file_path;           // Write/Edit: tool_input.file_path
    ContentDigest payload_digest {}; // sha256 over the raw bytes
    usize payload_bytes = 0;
};

// Locate-grade scan. NO payload field is required (2026-09-23 deny-118
// incident correction): the real ZCode payload carries tool_input.command
// (router-verified live) but no stable session/event identity field
// (RCA-14 H1 evidence). The event kind, tool name, and session handle
// come from the TRUSTED REGISTRATION TEMPLATE (--event/--tool/
// --session-handle on the hook command line); payload fields are
// corroborating evidence only. The digest always binds the verbatim
// bytes, so an unreadable-but-bounded payload still submits.
[[nodiscard]] qiven::Result<ZcodeEventFields, i32> extract_zcode_event(
    std::span<const std::byte> raw);
} // namespace qiven::runtime::adapter
