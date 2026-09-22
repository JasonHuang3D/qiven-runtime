#include <qiven/runtime/jsonx/json_codec.hpp>

#include <qiven/error.hpp>

#include <cctype>
#include <charconv>
#include <cstdio>

namespace qiven::runtime::jsonx
{
namespace
{
qiven::Error malformed(usize offset, std::string_view what)
{
    return qiven::Error::make(qiven::error_category::invalid_argument, err_malformed,
                              "json: " + std::string(what) + " at offset " + std::to_string(offset));
}

qiven::Error bounds_error(std::string_view what)
{
    return qiven::Error::make(qiven::error_category::resource_exhausted, err_bounds,
                              "json: " + std::string(what));
}

bool is_control_byte(unsigned char c) noexcept
{
    return c < 0x20 || c == 0x7F;
}

// Strict UTF-8 sequence validation (overlong/surrogate/out-range reject).
// Returns the sequence length, or 0 when invalid at this byte.
usize utf8_sequence_length_at(std::string_view bytes, usize offset) noexcept
{
    const auto lead = static_cast<unsigned char>(bytes[offset]);
    if (lead < 0x80)
    {
        return 1;
    }
    usize length = 0;
    u32 minimum  = 0;
    if ((lead & 0xE0) == 0xC0)
    {
        length  = 2;
        minimum = 0x80;
    }
    else if ((lead & 0xF0) == 0xE0)
    {
        length  = 3;
        minimum = 0x800;
    }
    else if ((lead & 0xF8) == 0xF0)
    {
        length  = 4;
        minimum = 0x10000;
    }
    else
    {
        return 0;
    }
    if (offset + length > bytes.size())
    {
        return 0;
    }
    u32 code = lead & (0x7F >> length);
    for (usize i = 1; i < length; ++i)
    {
        const auto cont = static_cast<unsigned char>(bytes[offset + i]);
        if ((cont & 0xC0) != 0x80)
        {
            return 0;
        }
        code = (code << 6) | (cont & 0x3F);
    }
    if (code < minimum || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF))
    {
        return 0;
    }
    return length;
}

void append_utf8(std::string& out, u32 code)
{
    if (code < 0x80)
    {
        out.push_back(static_cast<char>(code));
    }
    else if (code < 0x800)
    {
        out.push_back(static_cast<char>(0xC0 | (code >> 6)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
    else if (code < 0x10000)
    {
        out.push_back(static_cast<char>(0xE0 | (code >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
    else
    {
        out.push_back(static_cast<char>(0xF0 | (code >> 18)));
        out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
}

using StringResult = qiven::Result<std::string>;
using ValueResult  = qiven::Result<JsonValue>;

class Parser
{
public:
    explicit Parser(std::string_view bytes) :
    m_bytes(bytes)
    {
    }

    ValueResult document()
    {
        skip_ws();
        auto root = parse_value(1);
        if (!root.is_ok())
        {
            return root;
        }
        skip_ws();
        if (m_pos != m_bytes.size())
        {
            return ValueResult::fail(malformed(m_pos, "trailing garbage"));
        }
        return root;
    }

private:
    [[nodiscard]] bool at_end() const noexcept
    {
        return m_pos >= m_bytes.size();
    }
    [[nodiscard]] char peek() const noexcept
    {
        return m_bytes[m_pos];
    }

    void skip_ws() noexcept
    {
        while (!at_end() && (peek() == ' ' || peek() == '\t' || peek() == '\r' || peek() == '\n'))
        {
            ++m_pos;
        }
    }

    [[nodiscard]] bool consume(char expected) noexcept
    {
        if (!at_end() && peek() == expected)
        {
            ++m_pos;
            return true;
        }
        return false;
    }

    ValueResult parse_value(u32 depth)
    {
        if (depth > max_depth)
        {
            return ValueResult::fail(bounds_error("nesting depth exceeds limit"));
        }
        if (at_end())
        {
            return ValueResult::fail(malformed(m_pos, "unexpected end of input"));
        }
        switch (peek())
        {
        case '{': return parse_object(depth);
        case '[': return parse_array(depth);
        case '"':
        {
            auto text = parse_string();
            if (!text.is_ok())
            {
                return ValueResult::fail(text.reason());
            }
            return ValueResult(JsonValue::make_string(std::move(text.value())));
        }
        case 't':
            if (m_bytes.compare(m_pos, 4, "true") == 0)
            {
                m_pos += 4;
                return ValueResult(JsonValue::make_bool(true));
            }
            return ValueResult::fail(malformed(m_pos, "unknown literal"));
        case 'f':
            if (m_bytes.compare(m_pos, 5, "false") == 0)
            {
                m_pos += 5;
                return ValueResult(JsonValue::make_bool(false));
            }
            return ValueResult::fail(malformed(m_pos, "unknown literal"));
        case 'n':
            if (m_bytes.compare(m_pos, 4, "null") == 0)
            {
                m_pos += 4;
                return ValueResult(JsonValue::make_null());
            }
            return ValueResult::fail(malformed(m_pos, "unknown literal"));
        default: return parse_number();
        }
    }

    ValueResult parse_number()
    {
        const usize start = m_pos;
        if (!at_end() && (peek() == '-' || peek() == '+'))
        {
            return ValueResult::fail(
                malformed(start, "signed numbers are outside the bounded model"));
        }
        while (!at_end() && std::isdigit(static_cast<unsigned char>(peek())))
        {
            ++m_pos;
        }
        if (!at_end() && (peek() == '.' || peek() == 'e' || peek() == 'E'))
        {
            return ValueResult::fail(
                malformed(m_pos, "floating-point numbers are outside the bounded model"));
        }
        if (m_pos == start)
        {
            return ValueResult::fail(malformed(start, "expected a value"));
        }
        u64 value            = 0;
        const auto converted = std::from_chars(m_bytes.data() + start, m_bytes.data() + m_pos, value);
        if (converted.ec != std::errc {})
        {
            return ValueResult::fail(malformed(start, "number out of range"));
        }
        return ValueResult(JsonValue::make_number(value));
    }

    StringResult parse_string()
    {
        if (!consume('"'))
        {
            return StringResult::fail(malformed(m_pos, "expected a string"));
        }
        std::string out;
        while (true)
        {
            if (at_end())
            {
                return StringResult::fail(malformed(m_pos, "unterminated string"));
            }
            const unsigned char c = static_cast<unsigned char>(peek());
            if (c == '"')
            {
                ++m_pos;
                return StringResult(std::move(out));
            }
            if (c == '\\')
            {
                ++m_pos;
                if (at_end())
                {
                    return StringResult::fail(malformed(m_pos, "dangling escape"));
                }
                switch (peek())
                {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u':
                {
                    auto decoded = parse_unicode_escape();
                    if (!decoded.is_ok())
                    {
                        return StringResult::fail(decoded.reason());
                    }
                    out.append(decoded.value());
                    break;
                }
                default:
                    return StringResult::fail(malformed(m_pos, "unknown escape sequence"));
                }
                ++m_pos;
                continue;
            }
            if (is_control_byte(c))
            {
                return StringResult::fail(malformed(m_pos, "raw control byte in string"));
            }
            const usize length = utf8_sequence_length_at(m_bytes, m_pos);
            if (length == 0)
            {
                return StringResult::fail(malformed(m_pos, "invalid UTF-8"));
            }
            out.append(m_bytes.substr(m_pos, length));
            m_pos += length;
        }
    }

    // \uXXXX only. Surrogate halves are rejected outright: the bounded
    // model has no paired-escape machinery, and guessing a replacement is
    // exactly the silent reinterpretation a machine-file codec must not do.
    StringResult parse_unicode_escape()
    {
        const usize digits_start = m_pos + 1;
        if (digits_start + 4 > m_bytes.size())
        {
            return StringResult::fail(malformed(m_pos, "truncated \\u escape"));
        }
        u32 code = 0;
        for (usize i = 0; i < 4; ++i)
        {
            const char digit = m_bytes[digits_start + i];
            code <<= 4;
            if (digit >= '0' && digit <= '9')
            {
                code |= static_cast<u32>(digit - '0');
            }
            else if (digit >= 'a' && digit <= 'f')
            {
                code |= static_cast<u32>(digit - 'a' + 10);
            }
            else if (digit >= 'A' && digit <= 'F')
            {
                code |= static_cast<u32>(digit - 'A' + 10);
            }
            else
            {
                return StringResult::fail(malformed(digits_start + i, "bad hex digit"));
            }
        }
        m_pos += 4;
        if (code >= 0xD800 && code <= 0xDFFF)
        {
            return StringResult::fail(malformed(digits_start, "surrogate half rejected"));
        }
        std::string encoded;
        append_utf8(encoded, code);
        return StringResult(std::move(encoded));
    }

    ValueResult parse_object(u32 depth)
    {
        ++m_pos; // '{'
        JsonObject members;
        skip_ws();
        if (consume('}'))
        {
            return ValueResult(JsonValue::make_object(std::move(members)));
        }
        while (true)
        {
            skip_ws();
            auto key = parse_string();
            if (!key.is_ok())
            {
                return ValueResult::fail(key.reason());
            }
            skip_ws();
            if (!consume(':'))
            {
                return ValueResult::fail(malformed(m_pos, "expected ':'"));
            }
            skip_ws();
            auto value = parse_value(depth + 1);
            if (!value.is_ok())
            {
                return value;
            }
            for (const auto& member : members)
            {
                if (member.first == key.value())
                {
                    return ValueResult::fail(
                        malformed(m_pos, "duplicate object key '" + key.value() + "'"));
                }
            }
            members.emplace_back(std::move(key.value()), std::move(value.value()));
            skip_ws();
            if (consume(','))
            {
                continue;
            }
            if (consume('}'))
            {
                return ValueResult(JsonValue::make_object(std::move(members)));
            }
            return ValueResult::fail(malformed(m_pos, "expected ',' or '}'"));
        }
    }

    ValueResult parse_array(u32 depth)
    {
        ++m_pos; // '['
        std::vector<JsonValue> items;
        skip_ws();
        if (consume(']'))
        {
            return ValueResult(JsonValue::make_array(std::move(items)));
        }
        while (true)
        {
            skip_ws();
            auto item = parse_value(depth + 1);
            if (!item.is_ok())
            {
                return item;
            }
            items.push_back(std::move(item.value()));
            skip_ws();
            if (consume(','))
            {
                continue;
            }
            if (consume(']'))
            {
                return ValueResult(JsonValue::make_array(std::move(items)));
            }
            return ValueResult::fail(malformed(m_pos, "expected ',' or ']'"));
        }
    }

    std::string_view m_bytes;
    usize m_pos = 0;
};

void write_string_escaped(std::string& out, std::string_view text)
{
    out.push_back('"');
    for (usize i = 0; i < text.size();)
    {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        switch (c)
        {
        case '"':
            out += "\\\"";
            ++i;
            continue;
        case '\\':
            out += "\\\\";
            ++i;
            continue;
        case '\b':
            out += "\\b";
            ++i;
            continue;
        case '\f':
            out += "\\f";
            ++i;
            continue;
        case '\n':
            out += "\\n";
            ++i;
            continue;
        case '\r':
            out += "\\r";
            ++i;
            continue;
        case '\t':
            out += "\\t";
            ++i;
            continue;
        default: break;
        }
        if (is_control_byte(c))
        {
            char buffer[8];
            std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
            out += buffer;
            ++i;
            continue;
        }
        const usize length = utf8_sequence_length_at(text, i);
        if (length == 0)
        {
            // Parsed values are valid UTF-8 by construction; program-built
            // values use literals. A broken byte here is a caller defect —
            // closing the string keeps the document structurally valid
            // instead of emitting a corrupt sequence.
            out.push_back('"');
            return;
        }
        out.append(text.substr(i, length));
        i += length;
    }
    out.push_back('"');
}

void write_value(std::string& out, const JsonValue& value)
{
    switch (value.kind)
    {
    case JsonValue::Kind::Null: out += "null"; break;
    case JsonValue::Kind::Bool: out += value.bool_value ? "true" : "false"; break;
    case JsonValue::Kind::Number: out += std::to_string(value.number_value); break;
    case JsonValue::Kind::String: write_string_escaped(out, value.string_value); break;
    case JsonValue::Kind::Array:
    {
        out.push_back('[');
        for (usize i = 0; i < value.array.size(); ++i)
        {
            if (i != 0)
            {
                out.push_back(',');
            }
            write_value(out, value.array[i]);
        }
        out.push_back(']');
        break;
    }
    case JsonValue::Kind::Object:
    {
        out.push_back('{');
        for (usize i = 0; i < value.object.size(); ++i)
        {
            if (i != 0)
            {
                out.push_back(',');
            }
            write_string_escaped(out, value.object[i].first);
            out.push_back(':');
            write_value(out, value.object[i].second);
        }
        out.push_back('}');
        break;
    }
    }
}
} // namespace

qiven::Result<JsonValue> parse(std::string_view bytes)
{
    if (bytes.size() > max_input_bytes)
    {
        return ValueResult::fail(bounds_error("input exceeds 1 MiB"));
    }
    Parser parser(bytes);
    return parser.document();
}

std::string write(const JsonValue& value)
{
    std::string out;
    out.reserve(256);
    write_value(out, value);
    return out;
}
} // namespace qiven::runtime::jsonx
