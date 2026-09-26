#include <qiven/runtime/ipc/pipe_service.hpp>

#include <chrono>
#include <windows.h>

namespace qiven::runtime::ipc
{
namespace
{
void write_error_frame(PipeConnection& connection, const FrameCodec& codec, u64 request_id,
                       i32 code, const std::string& detail, u64 write_deadline_ms)
{
    const Reply error      = make_error(request_id, code, detail);
    const std::string body = encode_reply(error);
    FrameHeader header;
    header.request_id = request_id;
    (void)connection.write_bytes(codec.encode(header, body), write_deadline_ms);
}
} // namespace

// Bounded linger after a terminal error frame (2024-09-24, found by the
// contract test): DisconnectNamedPipe in the connection destructor can
// DISCARD bytes the client has not read yet — writing an error frame and
// closing in the same breath loses the reply to a race. The linger must
// wait WITHOUT consuming (an earlier read_frame-based linger ate the
// client's still-queued request and returned instantly, defeating itself):
// poll the connection (peek-only) until the client closes or 500 ms pass.
// A well-behaved client reads within milliseconds and disconnects; a
// client that outruns the bound loses the error frame (documented).
void linger_for_client_read(PipeConnection& connection)
{
    using namespace std::chrono;
    const auto deadline = steady_clock::now() + milliseconds(500);
    while (steady_clock::now() < deadline)
    {
        if (!connection.peer_connected())
        {
            return;
        }
        Sleep(5);
    }
}

ServeStats serve_connection(PipeConnection& connection, const FrameCodec& codec, const AdmitFn& admit,
                            const HandleFn& handle, const ServeOptions& options)
{
    ServeStats stats;

    const std::string rejection = admit ? admit(connection) : std::string();
    if (!rejection.empty())
    {
        // Typed reply surface (corrective-lane decision D2): an admission
        // rejection is SILENT no longer — the client can tell admission
        // apart from a dead host (the 2024-09-23 silent-drop class).
        write_error_frame(connection, codec, 0, err_auth, "admission rejected: " + rejection,
                          options.write_deadline_ms);
        linger_for_client_read(connection);
        stats.admission_rejected = true;
        return stats;
    }
    stats.admitted = true;

    u64 last_seq = 0;
    while (true)
    {
        // The stop flag is observed BETWEEN requests only (host-server
        // redesign §5 phase 2): once set, no NEW frame is read; a request
        // already in flight completes before its thread returns.
        if (options.stop != nullptr && options.stop->load(std::memory_order_acquire))
        {
            stats.stop_closed = true;
            return stats;
        }

        auto frame = connection.read_frame(options.idle_timeout_ms, options.stop);
        if (!frame.has_value())
        {
            if (connection.last_read_aborted())
            {
                stats.stop_closed = true;
            }
            else if (connection.last_read_timed_out())
            {
                stats.idle_closed = true;
            }
            else
            {
                stats.client_closed = true;
            }
            return stats;
        }

        auto verified = codec.decode(frame.value());
        if (!verified.is_ok())
        {
            write_error_frame(connection, codec, 0, verified.reason().code,
                              verified.reason().message, options.write_deadline_ms);
            linger_for_client_read(connection);
            stats.frame_error = true;
            return stats;
        }

        // Per-connection sequence law (framing.hpp): strictly increasing.
        // A regression or reuse is a replay-class protocol violation: typed
        // 63 reply, then the connection ends (state is suspect).
        if (verified.value().header.connection_seq <= last_seq)
        {
            write_error_frame(connection, codec, verified.value().header.request_id, err_replay,
                              "connection sequence regression (expected > " +
                                  std::to_string(last_seq) + ", got " +
                                  std::to_string(verified.value().header.connection_seq) + ")",
                              options.write_deadline_ms);
            linger_for_client_read(connection);
            stats.seq_violation = true;
            return stats;
        }
        last_seq = verified.value().header.connection_seq;

        auto request = decode_request(verified.value().body);
        if (!request.is_ok())
        {
            write_error_frame(connection, codec, verified.value().header.request_id,
                              request.reason().code, request.reason().detail,
                              options.write_deadline_ms);
            linger_for_client_read(connection);
            stats.frame_error = true;
            return stats;
        }

        if (stats.frames_served + 1 > options.max_frames)
        {
            write_error_frame(connection, codec, verified.value().header.request_id, err_frame,
                              "connection frame budget exceeded", options.write_deadline_ms);
            linger_for_client_read(connection);
            stats.budget_closed = true;
            return stats;
        }

        Reply reply = handle(request.value());
        FrameHeader reply_header;
        reply_header.request_id     = verified.value().header.request_id;
        reply_header.connection_seq = verified.value().header.connection_seq;
        if (!connection.write_bytes(codec.encode(reply_header, encode_reply(reply)),
                                    options.write_deadline_ms))
        {
            stats.client_closed = true;
            return stats;
        }
        stats.frames_served += 1;
    }
}
} // namespace qiven::runtime::ipc
