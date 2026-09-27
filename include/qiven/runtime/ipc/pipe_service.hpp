#pragma once

// ============================================================================
// ipc/pipe_service.hpp — the per-connection frame service loop
// (MVP-4 corrective lane, 2024-09-24, design decision D1; host-server
// redesign LL-3 amendments — docs/design/mvp4-host-server.md §5)
//
// EXTRACTED from apps/runtime_host_main.cpp so the production serve loop is
// a testable library unit, not exe-only code. The 2024-09-23 deny-116
// incident's dominant defect lived exactly here: the exe's loop served ONE
// frame per connection while the hook client sends Hello + HookEvent on
// ONE connection (the wire contract: FrameHeader::connection_seq is
// "strictly increasing per connection") — every real pre_tool denied 116
// with a healthy host, and no test could see it because the loop was not a
// unit (qiven-context evidence/audits/2024-09-24-mvp4-h1-deny116-incident).
//
// Connection model (the decision): ONE connection may carry a bounded
// sequence of frames; the connection ends when the client disconnects, a
// frame does not arrive within the idle timeout, a protocol/security class
// fails (typed error frame, then close), the per-connection frame budget
// is exhausted, or a stop flag is observed BETWEEN requests (the host-
// server redesign: no NEW request is read once the server is stopping; a
// frame already in flight completes). Single-frame clients (runtimectl)
// are unaffected.
//
// Enforced per connection, today, honestly: HMAC (FrameCodec), frame size
// caps, strictly-increasing connection_seq (typed 63 on regression), the
// frame budget, the idle bound, and the write deadline. Every typed error
// close uses the bounded linger (below) so the error frame is not
// discarded by the disconnect that follows it. The ReplayGuard
// nonce/timestamp window is NOT wired into the wire format (request bodies
// carry no nonce/timestamp); see docs/design/mvp4-hook-adapter.md "replay
// honesty note" — constructing a guard without calling it was the removed
// dead claim.
// ============================================================================

#include <qiven/result.hpp>
#include <qiven/runtime/ipc/framing.hpp>
#include <qiven/runtime/ipc/named_pipe_server.hpp>
#include <qiven/runtime/ipc/protocol.hpp>
#include <qiven/types.hpp>

#include <atomic>
#include <functional>
#include <string>

namespace qiven::runtime::ipc
{
struct ServeOptions
{
    u64 idle_timeout_ms   = 30'000; // no frame arrival within this → close
    u64 max_frames        = 64;     // per-connection frame budget (DoS bound)
    u64 write_deadline_ms = 5'000;  // per-reply write bound (hygiene, LL-3)
    // Observed BETWEEN requests (never mid-frame): once set, no NEW frame
    // is read; the loop returns with stop_closed set. Null = serve to the
    // connection's natural end (tests, single-shot drivers).
    const std::atomic<bool>* stop = nullptr;
};

struct ServeStats
{
    u64 frames_served       = 0;
    bool admitted           = false;
    bool admission_rejected = false; // typed 62 error frame written, closed
    bool seq_violation      = false; // typed 63 error frame written, closed
    bool frame_error        = false; // typed codec/protocol error, closed
    bool budget_closed      = false; // typed frame-budget error, closed
    bool idle_closed        = false; // no frame within the idle timeout
    bool client_closed      = false; // clean disconnect / transport end
    bool stop_closed        = false; // stop flag observed between requests
};

// Admission: receives the connection (the hook derives the OS client
// identity from it); return an EMPTY string to admit; any non-empty string
// is the typed rejection detail (a 62 error frame is written so the CLIENT
// can distinguish admission from silence, then the connection ends).
using AdmitFn = std::function<std::string(PipeConnection&)>;

// Request handling: returns the reply for one decoded request.
using HandleFn = std::function<Reply(const Request&)>;

// Bounded linger after a terminal error frame: wait (peek-only, consuming
// nothing) until the peer closes or 500 ms pass, so the reply is not
// discarded by the disconnect. Shared by every typed error close and the
// ServeLoop busy frame (LL-3).
void linger_for_client_read(PipeConnection& connection);

// Serves ONE accepted connection to its end. Never throws; every failure
// path closes the connection after writing a typed error frame where the
// protocol allows one. The caller owns the connection object lifetime.
[[nodiscard]] ServeStats serve_connection(PipeConnection& connection, const FrameCodec& codec,
                                          const AdmitFn& admit, const HandleFn& handle,
                                          const ServeOptions& options = ServeOptions {});
} // namespace qiven::runtime::ipc
