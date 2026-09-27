#pragma once

// ============================================================================
// ipc/protocol.hpp — request/response envelope kinds over the frames
// (MVP-3 batch design section 3.2; ARCH section 12.1; mvp4-host-server LL-3)
//
// MVP-3 surface: hello / status / doctor / mutation (the mutation kind is
// a typed HostRecovering placeholder until the governed pipeline batches
// land — batch design section 1.1). MVP-4 adds hook_event and shutdown;
// the host-server redesign adds refresh (operator trigger). Bodies are
// bounded jsonx documents (u64/string/bool/array-of-string); unknown
// kinds, unknown fields, wrong types, and bad UTF-8 fail closed as typed
// frame errors (64). THE WIRE CARRIES NO DEADLINE (host-server redesign
// LL-3): budgets are caller-side read bounds, never protocol state — the
// trial-4 class (a hello deadline validated against a ceiling) is dead
// by construction.
// ============================================================================

#include <qiven/result.hpp>
#include <qiven/runtime/ipc/framing.hpp>
#include <qiven/runtime/jsonx/json_codec.hpp>

#include <optional>
#include <string>
#include <vector>

namespace qiven::runtime::ipc
{
struct ProtocolError
{
    i32 code = err_frame;
    std::string detail;
};

struct Request
{
    enum class Kind : u8
    {
        Hello,
        Status,
        Doctor,
        Mutation,  // MVP-3 placeholder: always HostRecovering
        HookEvent, // MVP-4: the ZCode hook adapter events
        Shutdown,  // MVP-4 H-4: operator-initiated drain + exit
        Refresh,   // host-server redesign: operator refresh trigger
    };

    Kind kind      = Kind::Status;
    u64 request_id = 0;
    std::string client_kind; // hello
    u64 client_build = 0;    // hello
    std::string body;        // mutation payload (unused in MVP-3)
    // hook_event fields (MVP-4 design section 3.2)
    std::string event;          // session_start | pre_tool | post_tool
    std::string session_handle; // harness session id — evidence only
    std::string tool_name;
    std::string payload_sha256; // hex digest of the verbatim stdin bytes
    u64 payload_bytes = 0;
    std::string command;        // Bash extraction (when present)
    std::string file_path;      // Write/Edit extraction (when present)
    std::string mediated_tools; // session_start: "Bash,Write,Edit"
    u64 grace_ms = 0;           // shutdown: drain bound
};

struct Reply
{
    enum class Kind : u8
    {
        HelloAck,
        StatusView,
        DoctorView,
        ErrorView,
        HookAck,
        ShutdownAck,
        RefreshAck,
    };

    Kind kind      = Kind::ErrorView;
    u64 request_id = 0;
    std::string host_build;
    std::string install_id;
    u64 boot_epoch = 0;
    // status view
    std::string state;
    u64 generation = 0;
    std::string bundle_revision;
    u64 journal_events = 0;
    bool quarantined   = false;
    std::string refresh_state; // current | local_fallback | expired | degraded
    u64 last_refresh_ok_ms  = 0;
    u64 next_refresh_due_ms = 0;
    // doctor view
    bool integrity_ok     = false;
    bool audit_chain_ok   = false;
    bool bundle_active_ok = false;
    std::vector<std::string> findings;
    // error view
    i32 error_code = 0;
    std::string error_detail;
    // hook_ack view (MVP-4)
    std::string verdict;    // allow | deny | not_governed | degraded
    std::string session_id; // host-assigned runtime session id (hex)
    std::string action_id;  // host-assigned action id (hex)
    i64 reason_code = 0;
    std::string reason_detail;
    std::string refresh; // informational on EVERY hook event (host state)
    // shutdown_ack view
    bool draining = false;
    // refresh_ack view
    std::string refresh_result; // triggered | coalesced_pending
};

// Body layout (all kinds): {"kind": "<name>", "request_id": N, ...fields}.
// Decode rejects unknown keys (the retired deadline_ms among them — an old
// client fails closed typed, mapped to the honest 116-class deny).
[[nodiscard]] qiven::Result<Request, ProtocolError> decode_request(std::string_view body);
[[nodiscard]] std::string encode_reply(const Reply& reply);

// Client side: build a request body; parse a reply body (unknown reply
// fields and wrong types fail closed, symmetric with decode_request).
[[nodiscard]] std::string encode_request_body(const Request& request);
[[nodiscard]] qiven::Result<Reply, ProtocolError> decode_reply(std::string_view body);

[[nodiscard]] Reply make_error(u64 request_id, i32 code, std::string detail);
} // namespace qiven::runtime::ipc
