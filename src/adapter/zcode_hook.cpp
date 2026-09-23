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
        return ReplyResult::fail(secret.reason());
    }
    const ipc::FrameCodec codec(secret.value());

    std::string install_id;
    {
        std::ifstream in(run.runtime_root / "install.id");
        if (!in)
        {
            return ReplyResult::fail(qiven::Error::make(qiven::error_category::unavailable, 116,
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
        return ReplyResult::fail(client.reason());
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
        return ReplyResult::fail(qiven::Error::make(qiven::error_category::unavailable, 116,
                                                    "hello write failed"));
    }
    auto hello_frame = client.value().read_frame();
    if (!hello_frame.has_value())
    {
        return ReplyResult::fail(qiven::Error::make(qiven::error_category::unavailable, 116,
                                                    "no hello reply from host"));
    }
    auto hello_verified = codec.decode(hello_frame.value());
    if (!hello_verified.is_ok())
    {
        return ReplyResult::fail(hello_verified.reason());
    }
    auto hello_reply = ipc::decode_reply(hello_verified.value().body);
    if (!hello_reply.is_ok() || hello_reply.value().kind != ipc::Reply::Kind::HelloAck)
    {
        return ReplyResult::fail(qiven::Error::make(qiven::error_category::unavailable, 116,
                                                    "handshake rejected by host"));
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
        return ReplyResult::fail(qiven::Error::make(qiven::error_category::unavailable, 116,
                                                    "request write failed"));
    }
    auto frame = client.value().read_frame();
    if (!frame.has_value())
    {
        return ReplyResult::fail(qiven::Error::make(qiven::error_category::unavailable, 116,
                                                    "no reply from host"));
    }
    auto verified = codec.decode(frame.value());
    if (!verified.is_ok())
    {
        return ReplyResult::fail(verified.reason());
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
        if (pre_tool)
        {
            return deny_client(hook_reason_host_unavailable,
                               reply.reason().message +
                                   " -- cannot classify -- fail-closed deny (nothing is governed "
                                   "while the host is unreachable)");
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
