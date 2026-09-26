// ============================================================================
// ipc_multiframe_contract — MVP-4 corrective-lane regressions (2026-09-24)
// for the deny-116 incident (qiven-context evidence/audits/
// 2026-09-24-mvp4-h1-deny116-incident.md).
//
// Every test here FAILS under the actual prior implementations:
//   - the exe serve loop served ONE frame per connection while the hook
//     client sends Hello + HookEvent on one connection (test 1);
//   - admission rejection was a SILENT drop (test 2);
//   - connection_seq was never checked (test 3);
//   - a silent client wedged the serve loop forever (test 4);
//   - every transport cause collapsed into deny 116 (test 5).
// The production loop these tests exercise is ipc/pipe_service.cpp — the
// EXTRACTED unit the exe now calls (not a test-local reimplementation).
// ============================================================================

#include <qiven/contracts.hpp>
#include <qiven/runtime/adapter/zcode_hook.hpp>
#include <qiven/runtime/auth.hpp>
#include <qiven/runtime/ipc/framing.hpp>
#include <qiven/runtime/ipc/named_pipe_server.hpp>
#include <qiven/runtime/ipc/pipe_service.hpp>
#include <qiven/runtime/ipc/protocol.hpp>
#include <qiven/runtime/ipc/serve_loop.hpp>
#include <qiven/types.hpp>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <windows.h>

namespace
{
using qiven::runtime::auth::SecretKey;
using qiven::runtime::ipc::FrameCodec;
using qiven::runtime::ipc::FrameHeader;
using qiven::runtime::ipc::PipeClient;
using qiven::runtime::ipc::Reply;
using qiven::runtime::ipc::Request;

SecretKey test_key()
{
    SecretKey key {};
    for (std::size_t i = 0; i < key.size(); ++i)
    {
        key[i] = static_cast<std::byte>(i * 7 + 1);
    }
    return key;
}

std::string unique_install_id(const char* tag)
{
    return std::string(tag) + "-" + std::to_string(GetCurrentProcessId());
}

bool write_request(PipeClient& client, const FrameCodec& codec, const Request& request,
                   qiven::u64 connection_seq)
{
    FrameHeader header;
    header.request_id     = request.request_id;
    header.connection_seq = connection_seq;
    return client.write_bytes(codec.encode(header, qiven::runtime::ipc::encode_request_body(request)),
                              5000);
}

Request hello_request(qiven::u64 id)
{
    Request hello;
    hello.kind         = Request::Kind::Hello;
    hello.client_kind  = "contract-test";
    hello.client_build = 2;
    hello.request_id   = id;
    return hello;
}

Request hook_request(qiven::u64 id)
{
    Request request;
    request.kind           = Request::Kind::HookEvent;
    request.event          = "pre_tool";
    request.tool_name      = "Bash";
    request.session_handle = "contract-test-session";
    request.request_id     = id;
    request.payload_sha256 = std::string(64, 'a');
    request.payload_bytes  = 8;
    return request;
}

Reply canned_reply(const Request& request)
{
    if (request.kind == Request::Kind::Hello)
    {
        Reply ack;
        ack.kind       = Reply::Kind::HelloAck;
        ack.request_id = request.request_id;
        ack.host_build = "contract-test";
        return ack;
    }
    Reply ack;
    ack.kind       = Reply::Kind::HookAck;
    ack.request_id = request.request_id;
    ack.verdict    = "allow";
    return ack;
}

qiven::runtime::ipc::Reply decode_client_reply(const FrameCodec& codec, const std::string& frame)
{
    auto verified = codec.decode(frame);
    QIVEN_VERIFY(verified.is_ok());
    auto reply = qiven::runtime::ipc::decode_reply(verified.value().body);
    QIVEN_VERIFY(reply.is_ok());
    return reply.value();
}
} // namespace

int main()
{
    const SecretKey key = test_key();
    const FrameCodec codec(key);

    // Pure encode/decode round trip of both request shapes (no pipes).
    {
        const Request hello     = hello_request(1);
        const std::string wire1 = codec.encode(FrameHeader {},
                                               qiven::runtime::ipc::encode_request_body(hello));
        auto v1                 = codec.decode(wire1);
        QIVEN_VERIFY(v1.is_ok());
        auto r1 = qiven::runtime::ipc::decode_request(v1.value().body);
        QIVEN_VERIFY(r1.is_ok());
        const Request hook      = hook_request(2);
        const std::string wire2 = codec.encode(FrameHeader {},
                                               qiven::runtime::ipc::encode_request_body(hook));
        auto v2                 = codec.decode(wire2);
        if (!v2.is_ok())
        {
            std::printf("[diag] frame-2 codec.decode failed: code=%d msg=%s\n",
                        v2.reason().code, v2.reason().message.c_str());
        }
        QIVEN_VERIFY(v2.is_ok());
        auto r2 = qiven::runtime::ipc::decode_request(v2.value().body);
        if (!r2.is_ok())
        {
            std::printf("[diag] frame-2 decode_request failed: code=%d detail=%s\n",
                        r2.reason().code, r2.reason().detail.c_str());
        }
        QIVEN_VERIFY(r2.is_ok());
        std::printf("[ OK ] pure round trip of both request shapes\n");
    }

    // --- Test 1: TWO frames on ONE connection (the hook client's shape) ---
    {
        auto server = qiven::runtime::ipc::NamedPipeServer::create(
            unique_install_id("mfc1"));
        QIVEN_VERIFY(server.is_ok());

        // Deterministic connection order (host_lifecycle precedent): the
        // client connects to the FIRST instance BEFORE accept() stands up
        // the next one — otherwise CreateFile may land on the next instance
        // that nobody serves (an instance-selection race, not mediation).
        auto client = PipeClient::connect(
            qiven::runtime::ipc::pipe_name(unique_install_id("mfc1")));
        QIVEN_VERIFY(client.is_ok());

        qiven::runtime::ipc::ServeStats stats;
        std::thread server_thread([&]() {
            auto connection = server.value().accept();
            QIVEN_VERIFY(connection.is_ok());
            stats = qiven::runtime::ipc::serve_connection(
                connection.value(), codec, [](qiven::runtime::ipc::PipeConnection&) { return std::string(""); },
                [](const Request& request) { return canned_reply(request); });
        });

        QIVEN_VERIFY(write_request(client.value(), codec, hello_request(1), 1));
        {
            auto frame = client.value().read_frame(2000);
            QIVEN_VERIFY(frame.has_value());
            const Reply reply = decode_client_reply(codec, frame.value());
            QIVEN_VERIFY(reply.kind == Reply::Kind::HelloAck);
        }
        QIVEN_VERIFY(write_request(client.value(), codec, hook_request(2), 2));
        {
            auto frame = client.value().read_frame(2000);
            QIVEN_VERIFY(frame.has_value());
            const Reply reply = decode_client_reply(codec, frame.value());
            QIVEN_VERIFY(reply.kind == Reply::Kind::HookAck);
            QIVEN_VERIFY(reply.verdict == "allow");
        }
        client = qiven::Result<PipeClient>::fail(qiven::Error {}); // close: EOF, not idle
        server_thread.join();
        QIVEN_VERIFY(stats.admitted);
        QIVEN_VERIFY(stats.frames_served == 2);
        QIVEN_VERIFY(stats.client_closed);
        QIVEN_VERIFY(!stats.seq_violation && !stats.frame_error && !stats.idle_closed);
        std::printf("[ OK ] multiframe: hello + event on one connection\n");
    }

    // --- Test 2: admission rejection has a typed reply surface ---
    {
        auto server = qiven::runtime::ipc::NamedPipeServer::create(
            unique_install_id("mfc2"));
        QIVEN_VERIFY(server.is_ok());

        auto client = PipeClient::connect(
            qiven::runtime::ipc::pipe_name(unique_install_id("mfc2")));
        QIVEN_VERIFY(client.is_ok());

        std::thread server_thread([&]() {
            auto connection = server.value().accept();
            QIVEN_VERIFY(connection.is_ok());
            const auto stats = qiven::runtime::ipc::serve_connection(
                connection.value(), codec, [](qiven::runtime::ipc::PipeConnection&) { return std::string("unit-test image rejection"); },
                [](const Request& request) { return canned_reply(request); });
            QIVEN_VERIFY(stats.admission_rejected);
            QIVEN_VERIFY(stats.frames_served == 0);
        });

        QIVEN_VERIFY(write_request(client.value(), codec, hello_request(1), 1));
        {
            // Prior implementation: SILENT drop — this read timed out/EOF'd
            // and the cause was indistinguishable from a dead host.
            auto frame = client.value().read_frame(2000);
            QIVEN_VERIFY(frame.has_value());
            const Reply reply = decode_client_reply(codec, frame.value());
            QIVEN_VERIFY(reply.kind == Reply::Kind::ErrorView);
            QIVEN_VERIFY(reply.error_code == qiven::runtime::ipc::err_auth);
            QIVEN_VERIFY(reply.error_detail.rfind("admission rejected", 0) == 0);
        }
        server_thread.join();
        std::printf("[ OK ] admission: typed 62 reply (not silence)\n");
    }

    // --- Test 3: connection_seq regression is a typed 63 violation ---
    {
        auto server = qiven::runtime::ipc::NamedPipeServer::create(
            unique_install_id("mfc3"));
        QIVEN_VERIFY(server.is_ok());

        auto client = PipeClient::connect(
            qiven::runtime::ipc::pipe_name(unique_install_id("mfc3")));
        QIVEN_VERIFY(client.is_ok());

        std::thread server_thread([&]() {
            auto connection = server.value().accept();
            QIVEN_VERIFY(connection.is_ok());
            const auto stats = qiven::runtime::ipc::serve_connection(
                connection.value(), codec, [](qiven::runtime::ipc::PipeConnection&) { return std::string(""); },
                [](const Request& request) { return canned_reply(request); });
            QIVEN_VERIFY(stats.seq_violation);
        });

        QIVEN_VERIFY(write_request(client.value(), codec, hello_request(1), 5));
        {
            auto frame = client.value().read_frame(2000);
            QIVEN_VERIFY(frame.has_value());
            QIVEN_VERIFY(decode_client_reply(codec, frame.value()).kind == Reply::Kind::HelloAck);
        }
        // Sequence REUSE (5 again): replay-class protocol violation.
        QIVEN_VERIFY(write_request(client.value(), codec, hook_request(2), 5));
        {
            auto frame = client.value().read_frame(2000);
            QIVEN_VERIFY(frame.has_value());
            const Reply reply = decode_client_reply(codec, frame.value());
            QIVEN_VERIFY(reply.kind == Reply::Kind::ErrorView);
            QIVEN_VERIFY(reply.error_code == qiven::runtime::ipc::err_replay);
        }
        server_thread.join();
        std::printf("[ OK ] replay: seq regression denied typed 63\n");
    }

    // --- Test 4: an idle client cannot wedge the serve loop ---
    {
        auto server = qiven::runtime::ipc::NamedPipeServer::create(
            unique_install_id("mfc4"));
        QIVEN_VERIFY(server.is_ok());

        auto client = PipeClient::connect(
            qiven::runtime::ipc::pipe_name(unique_install_id("mfc4")));
        QIVEN_VERIFY(client.is_ok());

        qiven::runtime::ipc::ServeStats stats;
        std::thread server_thread([&]() {
            auto connection = server.value().accept();
            QIVEN_VERIFY(connection.is_ok());
            qiven::runtime::ipc::ServeOptions options;
            options.idle_timeout_ms = 200;
            stats                   = qiven::runtime::ipc::serve_connection(
                connection.value(), codec, [](qiven::runtime::ipc::PipeConnection&) { return std::string(""); },
                [](const Request& request) { return canned_reply(request); }, options);
        });

        // Say nothing; the server must close within a bounded idle window.
        server_thread.join(); // prior implementation: this join never returned
        QIVEN_VERIFY(stats.idle_closed);
        QIVEN_VERIFY(stats.frames_served == 0);
        std::printf("[ OK ] idle: silent client closed after the bound\n");
    }

    // --- Test 5: the transport denial taxonomy is disjoint and classable ---
    {
        using qiven::runtime::adapter::classify_transport_failure;
        namespace adapter = qiven::runtime::adapter;
        namespace ipc     = qiven::runtime::ipc;

        // No listener (connect refused / no install).
        QIVEN_VERIFY(classify_transport_failure(
                         qiven::Error::make(qiven::error_category::unavailable, ipc::err_frame,
                                            "pipe: pipe connect failed (os error 2)")) ==
                     adapter::hook_reason_no_listener);
        // Version skew (typed 64 at decode).
        QIVEN_VERIFY(classify_transport_failure(
                         qiven::Error::make(qiven::error_category::invalid_argument, ipc::err_frame,
                                            "ipc: unsupported protocol version")) ==
                     adapter::hook_reason_version_skew);
        // Secret skew: derived from a REAL wrong-key decode failure (not a
        // synthetic string — adversarial review m1: the classifier must not
        // be pinned only to hand-copied producer text).
        {
            SecretKey other_key {};
            for (std::size_t i = 0; i < other_key.size(); ++i)
            {
                other_key[i] = static_cast<std::byte>(i * 13 + 5);
            }
            const ipc::FrameCodec wrong_codec(other_key);
            const std::string wire = codec.encode(FrameHeader {},
                                                  qiven::runtime::ipc::encode_request_body(
                                                      hello_request(9)));
            auto broken            = wrong_codec.decode(wire);
            QIVEN_VERIFY(!broken.is_ok());
            QIVEN_VERIFY(classify_transport_failure(broken.reason()) ==
                         adapter::hook_reason_secret_skew);
        }
        // Deadline expiry.
        QIVEN_VERIFY(classify_transport_failure(
                         qiven::Error::make(qiven::error_category::unavailable, 0, "unused"),
                         true) == adapter::hook_reason_timeout);
        // Genuinely unknown shape stays 116.
        QIVEN_VERIFY(classify_transport_failure(
                         qiven::Error::make(qiven::error_category::unavailable, 0,
                                            "no reply from host (connection ended)")) ==
                     adapter::hook_reason_host_unavailable);
        // Disjointness of the emitted vocabulary itself.
        QIVEN_VERIFY(adapter::hook_reason_no_listener != adapter::hook_reason_host_unavailable);
        QIVEN_VERIFY(adapter::hook_reason_admission != adapter::hook_reason_secret_skew);
        QIVEN_VERIFY(adapter::hook_reason_timeout != adapter::hook_reason_version_skew);
        std::printf("[ OK ] taxonomy: 120/121/122/123/124/116 disjoint and classable\n");
    }

    // --- Test 6: the per-connection frame budget is enforced typed -------
    {
        auto server = qiven::runtime::ipc::NamedPipeServer::create(
            unique_install_id("mfc6"));
        QIVEN_VERIFY(server.is_ok());

        auto client = PipeClient::connect(
            qiven::runtime::ipc::pipe_name(unique_install_id("mfc6")));
        QIVEN_VERIFY(client.is_ok());

        qiven::runtime::ipc::ServeStats stats;
        std::thread server_thread([&]() {
            auto connection = server.value().accept();
            QIVEN_VERIFY(connection.is_ok());
            qiven::runtime::ipc::ServeOptions options;
            options.max_frames = 2;
            stats              = qiven::runtime::ipc::serve_connection(
                connection.value(), codec, [](qiven::runtime::ipc::PipeConnection&) { return std::string(""); },
                [](const Request& request) { return canned_reply(request); }, options);
        });

        // Frames 1 and 2 serve normally; frame 3 exceeds the budget.
        for (int i = 1; i <= 2; ++i)
        {
            QIVEN_VERIFY(write_request(client.value(), codec, hello_request(i), i));
            auto frame = client.value().read_frame(2000);
            QIVEN_VERIFY(frame.has_value());
            QIVEN_VERIFY(decode_client_reply(codec, frame.value()).kind == Reply::Kind::HelloAck);
        }
        QIVEN_VERIFY(write_request(client.value(), codec, hello_request(3), 3));
        {
            auto frame = client.value().read_frame(2000);
            QIVEN_VERIFY(frame.has_value());
            const Reply reply = decode_client_reply(codec, frame.value());
            QIVEN_VERIFY(reply.kind == Reply::Kind::ErrorView);
            QIVEN_VERIFY(reply.error_detail.find("frame budget") != std::string::npos);
        }
        server_thread.join();
        QIVEN_VERIFY(stats.frames_served == 2);
        QIVEN_VERIFY(stats.budget_closed);
        std::printf("[ OK ] budget: third frame denied typed after max_frames\n");
    }

    // --- Test 7: a DRIPPING client cannot wedge the serve loop (M1) ------
    {
        auto server = qiven::runtime::ipc::NamedPipeServer::create(
            unique_install_id("mfc7"));
        QIVEN_VERIFY(server.is_ok());

        auto client = PipeClient::connect(
            qiven::runtime::ipc::pipe_name(unique_install_id("mfc7")));
        QIVEN_VERIFY(client.is_ok());

        qiven::runtime::ipc::ServeStats stats;
        std::thread server_thread([&]() {
            auto connection = server.value().accept();
            QIVEN_VERIFY(connection.is_ok());
            qiven::runtime::ipc::ServeOptions options;
            options.idle_timeout_ms = 300;
            stats                   = qiven::runtime::ipc::serve_connection(
                connection.value(), codec, [](qiven::runtime::ipc::PipeConnection&) { return std::string(""); },
                [](const Request& request) { return canned_reply(request); }, options);
        });

        // One byte of a header, then silence: the prior blocking read held
        // the serve thread forever (only first-byte arrival was bounded).
        QIVEN_VERIFY(client.value().write_bytes("Q", 2000));
        server_thread.join(); // must return within the bound, not hang
        QIVEN_VERIFY(stats.idle_closed);
        QIVEN_VERIFY(stats.frames_served == 0);
        std::printf("[ OK ] drip: partial-frame stall closed within the bound\n");
    }

    // --- ServeLoop (host-server redesign): the production listen pool ------
    {
        using qiven::runtime::ipc::ServeLoop;
        ServeLoop::Options options;
        options.listen_arms     = 2;
        options.max_connections = 1; // tiny cap so the busy path is reachable
        options.idle_timeout_ms = 1000;
        ServeLoop::Hooks hooks;
        hooks.handle = [](const Request& request) {
            Reply reply;
            reply.kind       = Reply::Kind::HelloAck;
            reply.request_id = request.request_id;
            reply.host_build = "serve-loop-test";
            return reply;
        };
        auto loop = ServeLoop::create(unique_install_id("serveloop"), codec, options,
                                      std::move(hooks));
        QIVEN_VERIFY(loop.is_ok());
        std::thread runner([&] { loop.value()->run(); });

        // One client is served; the over-cap client receives the typed 125
        // busy frame (never an ERROR_PIPE_BUSY fake-120 against a healthy
        // pool).
        auto served = PipeClient::connect(
            qiven::runtime::ipc::pipe_name(unique_install_id("serveloop")));
        QIVEN_VERIFY(served.is_ok());
        QIVEN_VERIFY(write_request(served.value(), codec, hello_request(1), 1));
        {
            auto frame = served.value().read_frame(3000);
            QIVEN_VERIFY(frame.has_value());
            const Reply reply = decode_client_reply(codec, frame.value());
            QIVEN_VERIFY(reply.kind == Reply::Kind::HelloAck);
        }
        auto busy = PipeClient::connect(
            qiven::runtime::ipc::pipe_name(unique_install_id("serveloop")));
        QIVEN_VERIFY(busy.is_ok());
        QIVEN_VERIFY(write_request(busy.value(), codec, hello_request(2), 1));
        {
            auto frame = busy.value().read_frame(3000);
            QIVEN_VERIFY(frame.has_value());
            const Reply reply = decode_client_reply(codec, frame.value());
            QIVEN_VERIFY(reply.kind == Reply::Kind::ErrorView);
            QIVEN_VERIFY(reply.error_code == 125); // typed server-busy
        }
        QIVEN_VERIFY(loop.value()->stats().busy_rejected.load() >= 1);
        QIVEN_VERIFY(loop.value()->stats().busy_occupancy_last.load() >= 1);

        // Release the connections; after the idle close a fresh client is
        // served promptly (the cap frees with the connection).
        busy   = qiven::Result<PipeClient>::fail(qiven::Error {});
        served = qiven::Result<PipeClient>::fail(qiven::Error {});
        std::this_thread::sleep_for(std::chrono::milliseconds(1200)); // idle close (1 s)
        {
            auto fresh = PipeClient::connect(
                qiven::runtime::ipc::pipe_name(unique_install_id("serveloop")));
            QIVEN_VERIFY(fresh.is_ok());
            const auto begin = std::chrono::steady_clock::now();
            QIVEN_VERIFY(write_request(fresh.value(), codec, hello_request(3), 1));
            auto frame    = fresh.value().read_frame(3000);
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - begin)
                                .count();
            QIVEN_VERIFY(frame.has_value());
            QIVEN_VERIFY(ms < 1000);
            const Reply reply = decode_client_reply(codec, frame.value());
            QIVEN_VERIFY(reply.kind == Reply::Kind::HelloAck);
        }

        // Client-independence of survival (LL-1/LL-3): a storm of abrupt
        // garbage connections (connect, garbage byte, vanish) never ends
        // the loop, and a well-formed client is still served afterwards.
        for (int storm = 0; storm < 8; ++storm)
        {
            auto junk = PipeClient::connect(
                qiven::runtime::ipc::pipe_name(unique_install_id("serveloop")));
            if (junk.is_ok())
            {
                (void)junk.value().write_bytes("X", 500);                // garbage byte, no frame
                junk = qiven::Result<PipeClient>::fail(qiven::Error {}); // abrupt close
            }
        }
        // Let the storm's serve threads hit their idle bound and release
        // the cap (cap 1 in this fixture) before the well-formed client.
        std::this_thread::sleep_for(std::chrono::milliseconds(1300));
        {
            auto after = PipeClient::connect(
                qiven::runtime::ipc::pipe_name(unique_install_id("serveloop")));
            QIVEN_VERIFY(after.is_ok());
            QIVEN_VERIFY(write_request(after.value(), codec, hello_request(4), 1));
            auto frame = after.value().read_frame(3000);
            QIVEN_VERIFY(frame.has_value());
            const Reply reply = decode_client_reply(codec, frame.value());
            QIVEN_VERIFY(reply.kind == Reply::Kind::HelloAck);
        }
        QIVEN_VERIFY(!loop.value()->stop_requested()); // the loop SURVIVED

        // Stalled-peer isolation (serial-loop old-fail): a connection that
        // writes a PARTIAL frame and stalls must not delay another
        // client's verdict — the pool serves it on another thread. Cap is
        // 1 in this fixture, so raise it first (recreate is not possible;
        // instead this leg runs against a SECOND loop instance).
        {
            using qiven::runtime::ipc::ServeLoop;
            ServeLoop::Options iso;
            iso.listen_arms     = 2;
            iso.max_connections = 2;
            iso.idle_timeout_ms = 3000;
            ServeLoop::Hooks iso_hooks;
            iso_hooks.handle = [](const Request& request) {
                Reply reply;
                reply.kind       = Reply::Kind::HelloAck;
                reply.request_id = request.request_id;
                return reply;
            };
            auto iso_loop = ServeLoop::create(unique_install_id("serveloop-iso"), codec,
                                              iso, std::move(iso_hooks));
            QIVEN_VERIFY(iso_loop.is_ok());
            std::thread iso_runner([&] { iso_loop.value()->run(); });
            auto stalled = PipeClient::connect(
                qiven::runtime::ipc::pipe_name(unique_install_id("serveloop-iso")));
            QIVEN_VERIFY(stalled.is_ok());
            QIVEN_VERIFY(stalled.value().write_bytes("H", 2000)); // partial frame + stall
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            {
                auto healthy = PipeClient::connect(
                    qiven::runtime::ipc::pipe_name(unique_install_id("serveloop-iso")));
                QIVEN_VERIFY(healthy.is_ok());
                const auto begin = std::chrono::steady_clock::now();
                QIVEN_VERIFY(write_request(healthy.value(), codec, hello_request(9), 1));
                auto frame    = healthy.value().read_frame(3000);
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - begin)
                                    .count();
                QIVEN_VERIFY(frame.has_value());
                QIVEN_VERIFY(ms < 1000); // NOT delayed by the stalled peer
                const Reply reply = decode_client_reply(codec, frame.value());
                QIVEN_VERIFY(reply.kind == Reply::Kind::HelloAck);
            }
            iso_loop.value()->request_stop();
            iso_runner.join();
        }

        // Client busy etiquette (§5 accept topology): with every armed
        // instance held by stalled connections, concurrent clients still
        // connect within their bounded busy budget — the WaitNamedPipe
        // etiquette recovers through arm churn; a healthy pool NEVER
        // surfaces ERROR_PIPE_BUSY as a 120-class denial to a bounded
        // client (the design row this carrier owes).
        {
            using qiven::runtime::ipc::ServeLoop;
            ServeLoop::Options etq;
            etq.listen_arms     = 2;
            etq.max_connections = 10; // cap above the pressure set: busy is
                                      // NOT the subject here; slot pressure is
            etq.idle_timeout_ms = 4000;
            ServeLoop::Hooks etq_hooks;
            etq_hooks.handle = [](const Request& request) {
                Reply reply;
                reply.kind       = Reply::Kind::HelloAck;
                reply.request_id = request.request_id;
                return reply;
            };
            auto etq_loop = ServeLoop::create(unique_install_id("serveloop-etiquette"), codec,
                                              etq, std::move(etq_hooks));
            QIVEN_VERIFY(etq_loop.is_ok());
            std::thread etq_runner([&] { etq_loop.value()->run(); });

            // Two stalled connections hold both armed instances (partial
            // frame + stall keeps each serve thread busy on its instance).
            std::vector<PipeClient> stalled;
            for (int i = 0; i < 2; ++i)
            {
                auto held = PipeClient::connect(
                    qiven::runtime::ipc::pipe_name(unique_install_id("serveloop-etiquette")));
                QIVEN_VERIFY(held.is_ok());
                QIVEN_VERIFY(held.value().write_bytes("H", 2000)); // partial frame + stall
                stalled.push_back(std::move(held.value()));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));

            // Pressure set: six concurrent bounded clients under slot
            // pressure — every one connects within its budget and completes
            // a hello round trip (etiquette, never a false no-listener).
            constexpr int pressure_clients = 6;
            std::vector<int> results(pressure_clients, -3);
            std::vector<std::thread> pressers;
            for (int i = 0; i < pressure_clients; ++i)
            {
                pressers.emplace_back([&, i] {
                    auto client = PipeClient::connect(
                        qiven::runtime::ipc::pipe_name(
                            unique_install_id("serveloop-etiquette")),
                        3000);
                    if (!client.is_ok())
                    {
                        results[static_cast<std::size_t>(i)] = -1;
                        return;
                    }
                    if (!write_request(client.value(), codec, hello_request(20 + i), 1))
                    {
                        results[static_cast<std::size_t>(i)] = -2;
                        return;
                    }
                    auto frame = client.value().read_frame(3000);
                    if (!frame.has_value())
                    {
                        results[static_cast<std::size_t>(i)] = -4;
                        return;
                    }
                    const Reply reply = decode_client_reply(codec, frame.value());
                    results[static_cast<std::size_t>(i)] =
                        reply.kind == Reply::Kind::HelloAck ? 0 : -5;
                });
            }
            for (auto& presser : pressers)
            {
                presser.join();
            }
            for (const int result : results)
            {
                QIVEN_VERIFY(result == 0);
            }
            QIVEN_VERIFY(etq_loop.value()->stats().connections_served.load() >=
                         pressure_clients + 2);

            etq_loop.value()->request_stop();
            etq_runner.join();
            std::printf("[ OK ] client etiquette: %d concurrent bounded clients under slot "
                        "pressure all served\n",
                        pressure_clients);
        }

        // Stop lost-wakeup window (recheck law): a stop issued while arms
        // are between instance creation and ConnectNamedPipe still exits
        // run() within the grace — exercised by stopping a FRESH loop
        // immediately after run() starts (arms are in their first cycle).
        {
            using qiven::runtime::ipc::ServeLoop;
            ServeLoop::Hooks wake_hooks;
            wake_hooks.handle = [](const Request&) { return Reply {}; };
            auto wake_loop    = ServeLoop::create(unique_install_id("serveloop-wake"), codec,
                                                  ServeLoop::Options {}, std::move(wake_hooks));
            QIVEN_VERIFY(wake_loop.is_ok());
            std::thread wake_runner([&] { wake_loop.value()->run(); });
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            const auto begin = std::chrono::steady_clock::now();
            wake_loop.value()->request_stop();
            wake_runner.join();
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - begin)
                                .count();
            QIVEN_VERIFY(ms < 6000);
        }

        // Phased stop: request_stop wakes arms and serve threads inside
        // their slices; run() returns within the grace.
        const auto begin = std::chrono::steady_clock::now();
        loop.value()->request_stop();
        runner.join();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - begin)
                            .count();
        QIVEN_VERIFY(ms < 6000); // grace (5 s) + margin; far faster in practice
        std::printf("[ OK ] serve loop: pool admits, 125 busy typed, stop drains in %lld ms\n",
                    static_cast<long long>(ms));
    }

    std::printf("[ OK ] ipc_multiframe_contract: all corrective regressions pass\n");
    return 0;
}
