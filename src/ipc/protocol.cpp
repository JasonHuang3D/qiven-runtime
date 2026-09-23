#include <qiven/runtime/ipc/protocol.hpp>

namespace qiven::runtime::ipc
{
namespace
{
ProtocolError bad(std::string detail)
{
    return ProtocolError { err_frame, std::move(detail) };
}
} // namespace

qiven::Result<Request, ProtocolError> decode_request(std::string_view body)
{
    using jsonx::JsonValue;
    auto document = jsonx::parse(body);
    if (!document.is_ok())
    {
        return qiven::Result<Request, ProtocolError>::fail(
            bad("body is not a bounded JSON document: " + document.reason().message));
    }
    const JsonValue& value = document.value();
    if (value.kind != JsonValue::Kind::Object)
    {
        return qiven::Result<Request, ProtocolError>::fail(bad("body must be an object"));
    }
    // Closed vocabulary: every present key must be known.
    for (const auto& member : value.object)
    {
        if (member.first != "kind" && member.first != "request_id" &&
            member.first != "deadline_ms" && member.first != "client_kind" &&
            member.first != "client_build" && member.first != "body" &&
            member.first != "event" && member.first != "session_handle" &&
            member.first != "tool_name" && member.first != "payload_sha256" &&
            member.first != "payload_bytes" && member.first != "command" &&
            member.first != "file_path" && member.first != "mediated_tools" &&
            member.first != "grace_ms")
        {
            return qiven::Result<Request, ProtocolError>::fail(
                bad("unknown request field '" + member.first + "'"));
        }
    }
    if (!value.is("kind", JsonValue::Kind::String))
    {
        return qiven::Result<Request, ProtocolError>::fail(bad("kind string is required"));
    }

    Request request;
    const std::string kind = value.string_or("kind", "");
    if (kind == "hello")
    {
        request.kind         = Request::Kind::Hello;
        request.client_kind  = value.string_or("client_kind", "");
        request.client_build = value.number_or("client_build", 0);
        if (request.client_kind.empty())
        {
            return qiven::Result<Request, ProtocolError>::fail(
                bad("hello requires client_kind"));
        }
    }
    else if (kind == "status")
    {
        request.kind = Request::Kind::Status;
    }
    else if (kind == "doctor")
    {
        request.kind = Request::Kind::Doctor;
    }
    else if (kind == "mutation")
    {
        request.kind = Request::Kind::Mutation;
        request.body = value.string_or("body", "");
    }
    else if (kind == "hook_event")
    {
        request.kind           = Request::Kind::HookEvent;
        request.event          = value.string_or("event", "");
        request.session_handle = value.string_or("session_handle", "");
        request.tool_name      = value.string_or("tool_name", "");
        request.payload_sha256 = value.string_or("payload_sha256", "");
        request.payload_bytes  = value.number_or("payload_bytes", 0);
        request.command        = value.string_or("command", "");
        request.file_path      = value.string_or("file_path", "");
        request.mediated_tools = value.string_or("mediated_tools", "");
        if (request.event != "session_start" && request.event != "pre_tool" &&
            request.event != "post_tool")
        {
            return qiven::Result<Request, ProtocolError>::fail(
                bad("hook_event requires a known event"));
        }
        if (request.session_handle.empty() || request.payload_sha256.empty())
        {
            return qiven::Result<Request, ProtocolError>::fail(
                bad("hook_event requires session_handle and payload_sha256"));
        }
    }
    else if (kind == "shutdown")
    {
        request.kind     = Request::Kind::Shutdown;
        request.grace_ms = value.number_or("grace_ms", 3000);
        if (request.grace_ms == 0 || request.grace_ms > 30000)
        {
            return qiven::Result<Request, ProtocolError>::fail(
                bad("grace_ms must be in (0, 30000]"));
        }
    }
    else
    {
        return qiven::Result<Request, ProtocolError>::fail(bad("unknown request kind '" + kind + "'"));
    }

    request.request_id  = value.number_or("request_id", 0);
    request.deadline_ms = value.number_or("deadline_ms", 3000);
    // SessionStart may ask for a refresh-grade budget (MVP-4 H-2); every
    // other kind stays inside the 5 s interaction ceiling.
    const u64 ceiling =
        (request.kind == Request::Kind::HookEvent && request.event == "session_start")
            ? 10000
            : 5000;
    if (request.deadline_ms == 0 || request.deadline_ms > ceiling)
    {
        return qiven::Result<Request, ProtocolError>::fail(
            bad("deadline_ms must be in (0, " + std::to_string(ceiling) + "]"));
    }
    return qiven::Result<Request, ProtocolError>(std::move(request));
}

std::string encode_reply(const Reply& reply)
{
    using jsonx::JsonValue;
    jsonx::JsonObject object;
    object.emplace_back("request_id", JsonValue::make_number(reply.request_id));
    switch (reply.kind)
    {
    case Reply::Kind::HelloAck:
        object.emplace_back("kind", JsonValue::make_string("hello_ack"));
        object.emplace_back("host_build", JsonValue::make_string(reply.host_build));
        object.emplace_back("install_id", JsonValue::make_string(reply.install_id));
        object.emplace_back("boot_epoch", JsonValue::make_number(reply.boot_epoch));
        break;
    case Reply::Kind::StatusView:
        object.emplace_back("kind", JsonValue::make_string("status"));
        object.emplace_back("state", JsonValue::make_string(reply.state));
        object.emplace_back("install_id", JsonValue::make_string(reply.install_id));
        object.emplace_back("boot_epoch", JsonValue::make_number(reply.boot_epoch));
        object.emplace_back("generation", JsonValue::make_number(reply.generation));
        object.emplace_back("bundle_revision", JsonValue::make_string(reply.bundle_revision));
        object.emplace_back("journal_events", JsonValue::make_number(reply.journal_events));
        object.emplace_back("quarantined", JsonValue::make_bool(reply.quarantined));
        break;
    case Reply::Kind::DoctorView:
    {
        object.emplace_back("kind", JsonValue::make_string("doctor"));
        object.emplace_back("integrity_ok", JsonValue::make_bool(reply.integrity_ok));
        object.emplace_back("audit_chain_ok", JsonValue::make_bool(reply.audit_chain_ok));
        object.emplace_back("bundle_active_ok", JsonValue::make_bool(reply.bundle_active_ok));
        std::vector<JsonValue> findings;
        for (const auto& finding : reply.findings)
        {
            findings.push_back(JsonValue::make_string(finding));
        }
        object.emplace_back("findings", JsonValue::make_array(std::move(findings)));
        break;
    }
    case Reply::Kind::ErrorView:
    {
        object.emplace_back("kind", JsonValue::make_string("error"));
        jsonx::JsonObject error;
        error.emplace_back("code", JsonValue::make_number(static_cast<u64>(reply.error_code)));
        error.emplace_back("detail", JsonValue::make_string(reply.error_detail));
        object.emplace_back("error", JsonValue::make_object(std::move(error)));
        break;
    }
    case Reply::Kind::HookAck:
    {
        object.emplace_back("kind", JsonValue::make_string("hook_ack"));
        object.emplace_back("verdict", JsonValue::make_string(reply.verdict));
        object.emplace_back("session_id", JsonValue::make_string(reply.session_id));
        object.emplace_back("action_id", JsonValue::make_string(reply.action_id));
        object.emplace_back("reason_code",
                            JsonValue::make_number(static_cast<u64>(reply.reason_code)));
        object.emplace_back("reason_detail", JsonValue::make_string(reply.reason_detail));
        object.emplace_back("refresh", JsonValue::make_string(reply.refresh));
        object.emplace_back("generation", JsonValue::make_number(reply.generation));
        break;
    }
    case Reply::Kind::ShutdownAck:
    {
        object.emplace_back("kind", JsonValue::make_string("shutdown_ack"));
        object.emplace_back("draining", JsonValue::make_bool(reply.draining));
        break;
    }
    }
    return jsonx::write(JsonValue::make_object(std::move(object)));
}

Reply make_error(u64 request_id, i32 code, std::string detail)
{
    Reply reply;
    reply.kind         = Reply::Kind::ErrorView;
    reply.request_id   = request_id;
    reply.error_code   = code;
    reply.error_detail = std::move(detail);
    return reply;
}

std::string encode_request_body(const Request& request)
{
    using jsonx::JsonValue;
    jsonx::JsonObject object;
    switch (request.kind)
    {
    case Request::Kind::Hello:
        object.emplace_back("kind", JsonValue::make_string("hello"));
        object.emplace_back("client_kind", JsonValue::make_string(request.client_kind));
        object.emplace_back("client_build", JsonValue::make_number(request.client_build));
        break;
    case Request::Kind::Status: object.emplace_back("kind", JsonValue::make_string("status")); break;
    case Request::Kind::Doctor: object.emplace_back("kind", JsonValue::make_string("doctor")); break;
    case Request::Kind::Mutation:
        object.emplace_back("kind", JsonValue::make_string("mutation"));
        object.emplace_back("body", JsonValue::make_string(request.body));
        break;
    case Request::Kind::HookEvent:
    {
        object.emplace_back("kind", JsonValue::make_string("hook_event"));
        object.emplace_back("event", JsonValue::make_string(request.event));
        object.emplace_back("session_handle", JsonValue::make_string(request.session_handle));
        object.emplace_back("tool_name", JsonValue::make_string(request.tool_name));
        object.emplace_back("payload_sha256", JsonValue::make_string(request.payload_sha256));
        object.emplace_back("payload_bytes", JsonValue::make_number(request.payload_bytes));
        object.emplace_back("command", JsonValue::make_string(request.command));
        object.emplace_back("file_path", JsonValue::make_string(request.file_path));
        object.emplace_back("mediated_tools", JsonValue::make_string(request.mediated_tools));
        break;
    }
    case Request::Kind::Shutdown:
        object.emplace_back("kind", JsonValue::make_string("shutdown"));
        object.emplace_back("grace_ms", JsonValue::make_number(request.grace_ms));
        break;
    }
    object.emplace_back("request_id", JsonValue::make_number(request.request_id));
    object.emplace_back("deadline_ms", JsonValue::make_number(request.deadline_ms));
    return jsonx::write(JsonValue::make_object(std::move(object)));
}

qiven::Result<Reply, ProtocolError> decode_reply(std::string_view body)
{
    using ReplyResult = qiven::Result<Reply, ProtocolError>;
    using jsonx::JsonValue;
    auto document = jsonx::parse(body);
    if (!document.is_ok())
    {
        return ReplyResult::fail(bad("reply is not a bounded JSON document: " +
                                     document.reason().message));
    }
    const JsonValue& value = document.value();
    if (value.kind != JsonValue::Kind::Object)
    {
        return ReplyResult::fail(bad("reply must be an object"));
    }
    for (const auto& member : value.object)
    {
        if (member.first != "kind" && member.first != "request_id" && member.first != "host_build" &&
            member.first != "install_id" && member.first != "boot_epoch" && member.first != "state" &&
            member.first != "generation" && member.first != "bundle_revision" &&
            member.first != "journal_events" && member.first != "quarantined" &&
            member.first != "integrity_ok" && member.first != "audit_chain_ok" &&
            member.first != "bundle_active_ok" && member.first != "findings" &&
            member.first != "error" && member.first != "verdict" &&
            member.first != "session_id" && member.first != "action_id" &&
            member.first != "reason_code" && member.first != "reason_detail" &&
            member.first != "refresh" && member.first != "draining")
        {
            return ReplyResult::fail(bad("unknown reply field '" + member.first + "'"));
        }
    }
    const std::string kind = value.string_or("kind", "");
    Reply reply;
    reply.request_id = value.number_or("request_id", 0);
    if (kind == "hello_ack")
    {
        reply.kind       = Reply::Kind::HelloAck;
        reply.host_build = value.string_or("host_build", "");
        reply.install_id = value.string_or("install_id", "");
        reply.boot_epoch = value.number_or("boot_epoch", 0);
    }
    else if (kind == "status")
    {
        reply.kind            = Reply::Kind::StatusView;
        reply.state           = value.string_or("state", "");
        reply.install_id      = value.string_or("install_id", "");
        reply.boot_epoch      = value.number_or("boot_epoch", 0);
        reply.generation      = value.number_or("generation", 0);
        reply.bundle_revision = value.string_or("bundle_revision", "");
        reply.journal_events  = value.number_or("journal_events", 0);
        reply.quarantined     = value.is("quarantined", JsonValue::Kind::Bool) &&
                            value.find("quarantined")->bool_value;
    }
    else if (kind == "doctor")
    {
        reply.kind         = Reply::Kind::DoctorView;
        reply.integrity_ok = value.is("integrity_ok", JsonValue::Kind::Bool) &&
                             value.find("integrity_ok")->bool_value;
        reply.audit_chain_ok = value.is("audit_chain_ok", JsonValue::Kind::Bool) &&
                               value.find("audit_chain_ok")->bool_value;
        reply.bundle_active_ok = value.is("bundle_active_ok", JsonValue::Kind::Bool) &&
                                 value.find("bundle_active_ok")->bool_value;
        if (const auto* findings = value.find("findings");
            findings != nullptr && findings->kind == JsonValue::Kind::Array)
        {
            for (const auto& finding : findings->array)
            {
                if (finding.kind == JsonValue::Kind::String)
                {
                    reply.findings.push_back(finding.string_value);
                }
            }
        }
    }
    else if (kind == "error")
    {
        reply.kind = Reply::Kind::ErrorView;
        if (const auto* error = value.find("error");
            error != nullptr && error->kind == JsonValue::Kind::Object)
        {
            reply.error_code   = static_cast<i32>(error->number_or("code", 0));
            reply.error_detail = error->string_or("detail", "");
        }
        else
        {
            return ReplyResult::fail(bad("error reply requires an error object"));
        }
    }
    else if (kind == "hook_ack")
    {
        reply.kind          = Reply::Kind::HookAck;
        reply.verdict       = value.string_or("verdict", "");
        reply.session_id    = value.string_or("session_id", "");
        reply.action_id     = value.string_or("action_id", "");
        reply.reason_code   = static_cast<i64>(value.number_or("reason_code", 0));
        reply.reason_detail = value.string_or("reason_detail", "");
        reply.refresh       = value.string_or("refresh", "");
        reply.generation    = value.number_or("generation", 0);
        if (reply.verdict != "allow" && reply.verdict != "deny" &&
            reply.verdict != "not_governed" && reply.verdict != "degraded")
        {
            return ReplyResult::fail(bad("hook_ack carries an unknown verdict"));
        }
    }
    else if (kind == "shutdown_ack")
    {
        reply.kind     = Reply::Kind::ShutdownAck;
        reply.draining = value.is("draining", JsonValue::Kind::Bool) &&
                         value.find("draining")->bool_value;
    }
    else
    {
        return ReplyResult::fail(bad("unknown reply kind '" + kind + "'"));
    }
    return ReplyResult(std::move(reply));
}
} // namespace qiven::runtime::ipc
