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
    return client.write_bytes(codec.encode(header, qiven::runtime::ipc::encode_request_body(request)));
}

Request hello_request(qiven::u64 id)
{
    Request hello;
    hello.kind         = Request::Kind::Hello;
    hello.client_kind  = "contract-test";
    hello.client_build = 1;
    hello.request_id   = id;
    hello.deadline_ms  = 2000;
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
    request.deadline_ms    = 2000;
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
                connection.value(), codec, [] { return std::string(""); },
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
                connection.value(), codec, [] { return std::string("unit-test image rejection"); },
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
                connection.value(), codec, [] { return std::string(""); },
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
                connection.value(), codec, [] { return std::string(""); },
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
        // Secret skew (HMAC 62 at decode).
        QIVEN_VERIFY(classify_transport_failure(
                         qiven::Error::make(qiven::error_category::invalid_argument, ipc::err_auth,
                                            "ipc: HMAC verification failed")) ==
                     adapter::hook_reason_secret_skew);
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

    std::printf("[ OK ] ipc_multiframe_contract: all corrective regressions pass\n");
    return 0;
}
