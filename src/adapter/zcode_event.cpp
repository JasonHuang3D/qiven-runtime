#include <qiven/runtime/adapter/zcode_event.hpp>

#include <qiven/runtime/cognition/bundle.hpp>

namespace qiven::runtime::adapter
{
namespace
{
using qiven::runtime::cognition::digest_of;

constexpr u32 root_depth = 1;

// Scans one JSON string (opening quote at text[pos]); returns the decoded
// UTF-8 content and leaves pos past the closing quote. Escape-aware for
// both skipping and extraction; \uXXXX decodes the BMP (a lone surrogate
// is unreadable at this grade — bounded first-party law, not a parser).
[[nodiscard]] bool scan_string(std::string_view text, usize& pos, std::string* out)
{
    if (pos >= text.size() || text[pos] != '"')
    {
        return false;
    }
    pos += 1;
    if (out != nullptr)
    {
        out->clear();
    }
    while (pos < text.size())
    {
        const char c = text[pos];
        if (c == '"')
        {
            pos += 1;
            return true;
        }
        if (static_cast<unsigned char>(c) < 0x20)
        {
            return false; // raw control byte — unreadable
        }
        if (c == '\\')
        {
            if (pos + 1 >= text.size())
            {
                return false;
            }
            const char esc = text[pos + 1];
            pos += 2;
            switch (esc)
            {
            case '"':
                if (out)
                    out->push_back('"');
                break;
            case '\\':
                if (out)
                    out->push_back('\\');
                break;
            case '/':
                if (out)
                    out->push_back('/');
                break;
            case 'b':
                if (out)
                    out->push_back('\b');
                break;
            case 'f':
                if (out)
                    out->push_back('\f');
                break;
            case 'n':
                if (out)
                    out->push_back('\n');
                break;
            case 'r':
                if (out)
                    out->push_back('\r');
                break;
            case 't':
                if (out)
                    out->push_back('\t');
                break;
            case 'u':
            {
                if (pos + 4 > text.size())
                {
                    return false;
                }
                u32 code = 0;
                for (int digit = 0; digit < 4; digit += 1)
                {
                    const char h = text[pos + static_cast<usize>(digit)];
                    code <<= 4;
                    if (h >= '0' && h <= '9')
                        code |= static_cast<u32>(h - '0');
                    else if (h >= 'a' && h <= 'f')
                        code |= static_cast<u32>(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F')
                        code |= static_cast<u32>(h - 'A' + 10);
                    else
                        return false;
                }
                pos += 4;
                if (code >= 0xD800 && code <= 0xDFFF)
                {
                    return false; // surrogate half — unreadable at this grade
                }
                if (out != nullptr)
                {
                    if (code < 0x80)
                    {
                        out->push_back(static_cast<char>(code));
                    }
                    else if (code < 0x800)
                    {
                        out->push_back(static_cast<char>(0xC0 | (code >> 6)));
                        out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
                    }
                    else
                    {
                        out->push_back(static_cast<char>(0xE0 | (code >> 12)));
                        out->push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                        out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
                    }
                }
                break;
            }
            default: return false; // invalid escape — unreadable
            }
            continue;
        }
        if (out != nullptr)
        {
            out->push_back(c);
        }
        pos += 1;
    }
    return false; // unterminated
}

void skip_ws(std::string_view text, usize& pos)
{
    while (pos < text.size())
    {
        const char c = text[pos];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
        {
            pos += 1;
            continue;
        }
        break;
    }
}

// Walks one complete JSON value starting at pos (object, array, string,
// number, or literal), leaving pos past it. Depth-aware; validates only
// the structure needed to SKIP honestly (brackets/strings balanced).
[[nodiscard]] bool skip_value(std::string_view text, usize& pos, u32 depth)
{
    if (pos >= text.size() || depth > max_hook_payload_depth)
    {
        return false;
    }
    const char c = text[pos];
    if (c == '"')
    {
        return scan_string(text, pos, nullptr);
    }
    if (c == '{' || c == '[')
    {
        const char open  = c;
        const char close = (open == '{') ? '}' : ']';
        pos += 1;
        u32 nested = depth + 1;
        while (true)
        {
            skip_ws(text, pos);
            if (pos >= text.size())
            {
                return false;
            }
            if (text[pos] == close)
            {
                pos += 1;
                return true;
            }
            if (open == '{')
            {
                std::string key;
                if (!scan_string(text, pos, &key))
                {
                    return false;
                }
                skip_ws(text, pos);
                if (pos >= text.size() || text[pos] != ':')
                {
                    return false;
                }
                pos += 1;
                skip_ws(text, pos);
            }
            if (!skip_value(text, pos, nested))
            {
                return false;
            }
            skip_ws(text, pos);
            if (pos < text.size() && text[pos] == ',')
            {
                pos += 1;
                continue;
            }
        }
    }
    if (c == 't')
    {
        if (text.compare(pos, 4, "true") == 0)
        {
            pos += 4;
            return true;
        }
        return false;
    }
    if (c == 'f')
    {
        if (text.compare(pos, 5, "false") == 0)
        {
            pos += 5;
            return true;
        }
        return false;
    }
    if (c == 'n')
    {
        if (text.compare(pos, 4, "null") == 0)
        {
            pos += 4;
            return true;
        }
        return false;
    }
    // number (signed/float tolerated — located, never interpreted)
    if (c == '-' || c == '+' || (c >= '0' && c <= '9'))
    {
        while (pos < text.size())
        {
            const char d = text[pos];
            if ((d >= '0' && d <= '9') || d == '-' || d == '+' || d == '.' ||
                d == 'e' || d == 'E')
            {
                pos += 1;
                continue;
            }
            break;
        }
        return true;
    }
    return false;
}

struct Extraction
{
    ZcodeEventFields fields;
    bool have_session = false;
    bool have_event   = false;
    bool have_tool    = false;
};

// Parses the members of ONE object whose entries start at pos. Captures
// wanted keys at the given logical level; recurses through skip_value
// for everything else.
[[nodiscard]] bool parse_members(std::string_view text, usize& pos, u32 depth,
                                 Extraction& out, bool tool_input_level)
{
    while (true)
    {
        skip_ws(text, pos);
        if (pos >= text.size())
        {
            return false;
        }
        if (text[pos] == '}')
        {
            pos += 1;
            return true;
        }
        std::string key;
        if (!scan_string(text, pos, &key))
        {
            return false;
        }
        skip_ws(text, pos);
        if (pos >= text.size() || text[pos] != ':')
        {
            return false;
        }
        pos += 1;
        skip_ws(text, pos);

        const bool wanted_string = !tool_input_level &&
                                   (key == "session_id" || key == "sessionId" || key == "hook_event_name" ||
                                    key == "event_name" || key == "tool_name" || key == "toolName");
        const bool wanted_inner = tool_input_level &&
                                  (key == "command" || key == "file_path" || key == "filePath");
        if (wanted_string || wanted_inner)
        {
            std::string value;
            if (pos < text.size() && text[pos] == '"' && scan_string(text, pos, &value))
            {
                if (key == "session_id" || key == "sessionId")
                {
                    out.fields.session_handle = std::move(value);
                    out.have_session          = true;
                }
                else if (key == "hook_event_name" || key == "event_name")
                {
                    out.fields.event_name = std::move(value);
                    out.have_event        = true;
                }
                else if (key == "tool_name" || key == "toolName")
                {
                    out.fields.tool_name = std::move(value);
                    out.have_tool        = true;
                }
                else if (key == "command")
                {
                    out.fields.command = std::move(value);
                }
                else
                {
                    out.fields.file_path = std::move(value);
                }
            }
            else if (wanted_string)
            {
                // A required identity field that is not a readable string
                // cannot back a request — fail closed.
                return false;
            }
            else if (pos < text.size())
            {
                if (!skip_value(text, pos, depth + 1))
                {
                    return false;
                }
            }
            else
            {
                return false;
            }
        }
        else if (!tool_input_level &&
                 (key == "tool_input" || key == "toolInput") && pos < text.size() &&
                 text[pos] == '{')
        {
            pos += 1;
            if (!parse_members(text, pos, depth + 1, out, true))
            {
                return false;
            }
        }
        else
        {
            if (!skip_value(text, pos, depth + 1))
            {
                return false;
            }
        }
        skip_ws(text, pos);
        if (pos < text.size() && text[pos] == ',')
        {
            pos += 1;
            continue;
        }
        if (pos < text.size() && text[pos] == '}')
        {
            pos += 1;
            return true;
        }
        return false;
    }
}
} // namespace

qiven::Result<ZcodeEventFields, i32> extract_zcode_event(std::span<const std::byte> raw)
{
    using Result = qiven::Result<ZcodeEventFields, i32>;
    if (raw.size() > max_hook_payload_bytes)
    {
        return Result::fail(hook_err_payload_oversize);
    }
    const std::string_view text(reinterpret_cast<const char*>(raw.data()), raw.size());

    Extraction out;
    out.fields.payload_bytes = raw.size();
    out.fields.payload_digest =
        digest_of(std::string_view(reinterpret_cast<const char*>(raw.data()), raw.size()));

    usize pos = 0;
    skip_ws(text, pos);
    if (pos >= text.size() || text[pos] != '{')
    {
        return Result::fail(hook_err_payload_unreadable);
    }
    pos += 1;
    if (!parse_members(text, pos, root_depth, out, false))
    {
        return Result::fail(hook_err_payload_unreadable);
    }
    skip_ws(text, pos);
    if (pos != text.size())
    {
        return Result::fail(hook_err_payload_unreadable); // trailing garbage
    }
    // 2026-09-23 deny-118 incident correction: NO field is required. The
    // real payload carries tool_input.command (router-verified live) but
    // no stable session/event identity (RCA-14 H1 evidence); identity
    // comes from the trusted registration template. A digestable,
    // bounded payload always submits.
    return Result(std::move(out.fields));
}
} // namespace qiven::runtime::adapter
