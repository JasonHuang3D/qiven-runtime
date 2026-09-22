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
            member.first != "client_build" && member.first != "body")
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
    else
    {
        return qiven::Result<Request, ProtocolError>::fail(bad("unknown request kind '" + kind + "'"));
    }

    request.request_id  = value.number_or("request_id", 0);
    request.deadline_ms = value.number_or("deadline_ms", 3000);
    if (request.deadline_ms == 0 || request.deadline_ms > 5000)
    {
        return qiven::Result<Request, ProtocolError>::fail(
            bad("deadline_ms must be in (0, 5000]"));
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
            member.first != "bundle_active_ok" && member.first != "findings" && member.first != "error")
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
    else
    {
        return ReplyResult::fail(bad("unknown reply kind '" + kind + "'"));
    }
    return ReplyResult(std::move(reply));
}
} // namespace qiven::runtime::ipc
