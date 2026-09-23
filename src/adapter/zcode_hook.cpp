#include <qiven/runtime/adapter/zcode_hook.hpp>

#include <qiven/runtime/adapter/zcode_event.hpp>
#include <qiven/runtime/cognition/bundle.hpp>
#include <qiven/runtime/ipc/framing.hpp>
#include <qiven/runtime/ipc/named_pipe_server.hpp>
#include <qiven/runtime/ipc/protocol.hpp>

#include <fstream>
#include <iterator>
#include <utility>

namespace qiven::runtime::adapter
{
namespace
{
HookOutcome allow_silent()
{
    return HookOutcome { 0, "" };
}

HookOutcome advisory_note(std::string text)
{
    return HookOutcome { 0, "[qiven-hook] " + std::move(text) };
}

// Source-tagged deny (2026-09-23 owner direction after trial 2): the
// owner reading the session must be able to tell WHERE the verdict came
// from -- a hook-client-side failure never contacted the host, so the
// host console rightly shows nothing for it.
HookOutcome deny_client(i32 reason, std::string detail)
{
    return HookOutcome {
        2, "[qiven-hook] deny " + std::to_string(reason) +
               " (hook-client; no host verdict): " + std::move(detail)
    };
}

HookOutcome deny_host(i32 reason, std::string detail)
{
    return HookOutcome {
        2, "[qiven-hook] deny " + std::to_string(reason) +
               " (host verdict): " + std::move(detail)
    };
}

qiven::Result<qiven::runtime::ipc::Reply> transact(const HookRun& run,
                                                   const ZcodeEventFields& fields)
{
    using ReplyResult = qiven::Result<qiven::runtime::ipc::Reply>;
    namespace ipc     = qiven::runtime::ipc;

    auto secret = ipc::InstallationSecret::ensure(run.runtime_root);
    if (!secret.is_ok())
    {
        return ReplyResult::fail(
            qiven::Error::make(qiven::error_category::unavailable,
                               classify_transport_failure(secret.reason()),
                               secret.reason().message));
    }
    const ipc::FrameCodec codec(secret.value());

    std::string install_id;
    {
        std::ifstream in(run.runtime_root / "install.id");
        if (!in)
        {
            return ReplyResult::fail(qiven::Error::make(qiven::error_category::unavailable,
                                                        hook_reason_no_listener,
                                                        "no installed RuntimeHost"));
        }
        install_id.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char> {});
        while (!install_id.empty() && (install_id.back() == '\n' || install_id.back() == '\r' ||
                                       install_id.back() == ' '))
        {
            install_id.pop_back();
        }
    }

    auto client = ipc::PipeClient::connect(ipc::pipe_name(install_id));
    if (!client.is_ok())
    {
        return ReplyResult::fail(qiven::Error::make(
            qiven::error_category::unavailable, hook_reason_no_listener,
            "no RuntimeHost listener on the installation pipe (" + client.reason().message +
                ")"));
    }

    u64 seq = 1;
    // hello first: the host answers or rejects the version BEFORE the
    // event is processed; a rejected handshake is a deny for pre_tool.
    ipc::Request hello;
    hello.kind         = ipc::Request::Kind::Hello;
    hello.client_kind  = "zcode-hook";
    hello.client_build = 1;
    hello.request_id   = 1;
    hello.deadline_ms  = run.deadline_ms;
    ipc::FrameHeader header;
    header.request_id     = hello.request_id;
    header.connection_seq = seq;
    if (!client.value().write_bytes(codec.encode(header, ipc::encode_request_body(hello))))
    {
        return ReplyResult::fail(qiven::Error::make(qiven::error_category::unavailable,
                                                    hook_reason_host_unavailable,
                                                    "hello write failed"));
    }
    auto hello_frame = client.value().read_frame(run.deadline_ms);
    if (!hello_frame.has_value())
    {
        if (client.value().last_read_timed_out())
        {
            return ReplyResult::fail(
                qiven::Error::make(qiven::error_category::unavailable, hook_reason_timeout,
                                   "no hello reply within " + std::to_string(run.deadline_ms) +
                                       " ms (timeout)"));
        }
        return ReplyResult::fail(qiven::Error::make(qiven::error_category::unavailable,
                                                    hook_reason_host_unavailable,
                                                    "no hello reply from host"));
    }
    auto hello_verified = codec.decode(hello_frame.value());
    if (!hello_verified.is_ok())
    {
        return ReplyResult::fail(
            qiven::Error::make(qiven::error_category::unavailable,
                               classify_transport_failure(hello_verified.reason()),
                               hello_verified.reason().message));
    }
    auto hello_reply = ipc::decode_reply(hello_verified.value().body);
    if (!hello_reply.is_ok())
    {
        return ReplyResult::fail(qiven::Error::make(qiven::error_category::invalid_argument,
                                                    hello_reply.reason().code,
                                                    hello_reply.reason().detail));
    }
    if (hello_reply.value().kind == ipc::Reply::Kind::ErrorView)
    {
        // Typed host rejection at the handshake: the admission surface is a
        // real reply now (pipe_service), so the class is diagnosable.
        const std::string& detail = hello_reply.value().error_detail;
        const i32 code            = detail.rfind("admission rejected", 0) == 0
                                        ? hook_reason_admission
                                        : static_cast<i32>(hello_reply.value().error_code);
        return ReplyResult::fail(
            qiven::Error::make(qiven::error_category::unavailable, code,
                               "handshake rejected by host: " + detail));
    }
    if (hello_reply.value().kind != ipc::Reply::Kind::HelloAck)
    {
        return ReplyResult::fail(qiven::Error::make(qiven::error_category::unavailable,
                                                    hook_reason_host_unavailable,
                                                    "unexpected hello reply shape"));
    }

    ipc::Request request;
    request.kind  = ipc::Request::Kind::HookEvent;
    request.event = run.event;
    request.session_handle =
        run.session_handle.empty() ? fields.session_handle : run.session_handle;
    request.tool_name      = run.tool.empty() ? fields.tool_name : run.tool;
    request.payload_sha256 = cognition::hex_lower(
        std::span<const std::byte>(fields.payload_digest.sha256.data(),
                                   fields.payload_digest.sha256.size()));
    request.payload_bytes  = fields.payload_bytes;
    request.command        = fields.command;
    request.file_path      = fields.file_path;
    request.mediated_tools = run.mediated_tools;
    request.request_id     = 2;
    request.deadline_ms    = run.deadline_ms;

    seq += 1;
    header.request_id     = request.request_id;
    header.connection_seq = seq;
    if (!client.value().write_bytes(codec.encode(header, ipc::encode_request_body(request))))
    {
        return ReplyResult::fail(qiven::Error::make(qiven::error_category::unavailable,
                                                    hook_reason_host_unavailable,
                                                    "request write failed"));
    }
    auto frame = client.value().read_frame(run.deadline_ms);
    if (!frame.has_value())
    {
        if (client.value().last_read_timed_out())
        {
            return ReplyResult::fail(
                qiven::Error::make(qiven::error_category::unavailable, hook_reason_timeout,
                                   "no reply from host within " + std::to_string(run.deadline_ms) +
                                       " ms (timeout)"));
        }
        return ReplyResult::fail(qiven::Error::make(qiven::error_category::unavailable,
                                                    hook_reason_host_unavailable,
                                                    "no reply from host (connection ended)"));
    }
    auto verified = codec.decode(frame.value());
    if (!verified.is_ok())
    {
        return ReplyResult::fail(qiven::Error::make(
            qiven::error_category::unavailable,
            classify_transport_failure(verified.reason()),
            verified.reason().message));
    }
    auto reply = ipc::decode_reply(verified.value().body);
    if (!reply.is_ok())
    {
        return ReplyResult::fail(qiven::Error::make(qiven::error_category::invalid_argument,
                                                    reply.reason().code, reply.reason().detail));
    }
    return ReplyResult(std::move(reply.value()));
}
} // namespace

i32 classify_transport_failure(const qiven::Error& error, bool read_deadline_expired)
{
    namespace ipc = qiven::runtime::ipc;
    if (read_deadline_expired)
    {
        return hook_reason_timeout;
    }
    const std::string& message = error.message;
    if (message.find("pipe connect failed") != std::string::npos ||
        message.find("no RuntimeHost listener") != std::string::npos ||
        message.find("no installed RuntimeHost") != std::string::npos)
    {
        return hook_reason_no_listener;
    }
    if (error.code == ipc::err_frame && message.find("unsupported protocol version") != std::string::npos)
    {
        return hook_reason_version_skew;
    }
    if (error.code == ipc::err_auth && message.find("HMAC verification failed") != std::string::npos)
    {
        return hook_reason_secret_skew;
    }
    if (message.find("secret") != std::string::npos ||
        message.find("CryptProtectData") != std::string::npos)
    {
        // The local installation secret is unreadable/stale (DPAPI class,
        // incl. the mint-side CryptProtectData failure — adversarial-review
        // m2: that message carries no "secret" token).
        return hook_reason_secret_skew;
    }
    if (error.code == hook_reason_no_listener || error.code == hook_reason_admission ||
        error.code == hook_reason_version_skew || error.code == hook_reason_secret_skew ||
        error.code == hook_reason_timeout)
    {
        return error.code; // already classified by transact
    }
    return hook_reason_host_unavailable;
}

HookOutcome run_zcode_hook(const HookRun& run)
{
    const bool pre_tool    = run.event == "pre_tool";
    const bool is_advisory = run.event == "session_start" || run.event == "post_tool";

    auto payload_result = [&]() {
        // Oversize/unreadable payloads deny pre_tool (fail-closed) and only
        // note the advisory events (a completed action cannot be denied).
        if (run.payload.size() > max_hook_payload_bytes)
        {
            return qiven::Result<ZcodeEventFields, i32>::fail(hook_err_payload_oversize);
        }
        return extract_zcode_event(run.payload);
    }();
    if (!payload_result.is_ok())
    {
        if (pre_tool)
        {
            return deny_client(hook_reason_payload,
                               "hook payload oversize or unreadable (digest unavailable)");
        }
        if (is_advisory)
        {
            return advisory_note(run.event + ": payload not readable; event unregistered");
        }
        return deny_client(hook_reason_payload, "unknown event kind");
    }
    const ZcodeEventFields& fields = payload_result.value();

    // Registration-template authority (deny-118 correction): the template
    // supplies the identity; payload fields only corroborate. A payload
    // tool_name that CONTRADICTS the template is a misregistration -- deny.
    if (pre_tool && !run.tool.empty() && !fields.tool_name.empty() &&
        fields.tool_name != run.tool)
    {
        return deny_client(hook_reason_payload,
                           "payload tool '" + fields.tool_name + "' contradicts the registered tool '" +
                               run.tool + "' (misregistration)");
    }

    auto reply = transact(run, fields);
    if (!reply.is_ok())
    {
        const i32 code = classify_transport_failure(reply.reason());
        if (pre_tool)
        {
            // The fail-closed clause is only TRUE for unreachable-host
            // classes; a host that ANSWERED and rejected the handshake
            // must not be described as unreachable (adversarial-review
            // m2: false "cannot be reached" text on host-answered codes).
            const bool host_unreachable_class =
                code == hook_reason_host_unavailable || code == hook_reason_no_listener ||
                code == hook_reason_timeout;
            return deny_client(code,
                               reply.reason().message +
                                   (host_unreachable_class
                                        ? " -- fail-closed deny (nothing is governed while the "
                                          "host cannot be reached)"
                                        : " -- fail-closed deny (no governed status without a "
                                          "completed handshake)"));
        }
        return advisory_note(run.event + ": host unreachable -- " + reply.reason().message +
                             (run.event == "session_start"
                                  ? "; session NOT registered"
                                  : "; outcome unobserved"));
    }
    if (reply.value().kind == qiven::runtime::ipc::Reply::Kind::ErrorView)
    {
        if (pre_tool)
        {
            return deny_host(static_cast<i32>(reply.value().error_code), reply.value().error_detail);
        }
        return advisory_note(run.event + ": host denied -- " +
                             std::to_string(reply.value().error_code) + ": " +
                             reply.value().error_detail);
    }
    if (reply.value().kind != qiven::runtime::ipc::Reply::Kind::HookAck)
    {
        if (pre_tool)
        {
            return deny_client(hook_reason_host_unavailable, "unexpected reply shape");
        }
        return advisory_note(run.event + ": unexpected reply shape");
    }

    const auto& ack = reply.value();
    if (ack.verdict == "deny")
    {
        return deny_host(static_cast<i32>(ack.reason_code), ack.reason_detail);
    }
    if (run.event == "session_start" && !ack.refresh.empty() && ack.refresh != "current")
    {
        return advisory_note("session registered (id " + ack.session_id + "); cognition " +
                             ack.refresh);
    }
    return allow_silent();
}
} // namespace qiven::runtime::adapter
