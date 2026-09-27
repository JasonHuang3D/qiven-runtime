#include <qiven/runtime/cognition/task_descriptor.hpp>

#include <qiven/hashing_sha256.hpp>

#include <algorithm>
#include <cctype>

namespace qiven::runtime::cognition
{
namespace
{
void json_escape(std::string& out, std::string_view text)
{
    out.push_back('"');
    for (const char ch : text)
    {
        if (ch == '"' || ch == '\\')
        {
            out.push_back('\\');
        }
        if (static_cast<unsigned char>(ch) >= 0x20)
        {
            out.push_back(ch);
        }
        else
        {
            switch (ch)
            {
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
            {
                char buffer[8];
                std::snprintf(buffer, sizeof(buffer), "\\u%04x",
                              static_cast<unsigned>(static_cast<unsigned char>(ch)));
                out += buffer;
                break;
            }
            }
        }
    }
    out.push_back('"');
}

void append_string_list(std::string& out, const char* key,
                        const std::vector<std::string>& values)
{
    out += ",\"";
    out += key;
    out += "\":[";
    bool first = true;
    for (const std::string& value : values)
    {
        if (!first)
        {
            out.push_back(',');
        }
        first = false;
        json_escape(out, value);
    }
    out.push_back(']');
}

std::string lower_alpha_run(std::string_view text, usize& position, bool allow_dash = true)
{
    std::string out;
    while (position < text.size() &&
           ((text[position] >= 'a' && text[position] <= 'z') ||
            (text[position] >= 'A' && text[position] <= 'Z') ||
            (text[position] >= '0' && text[position] <= '9') ||
            (allow_dash && text[position] == '-') || text[position] == '_'))
    {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(text[position]))));
        ++position;
    }
    return out;
}
} // namespace

std::optional<std::string> language_of_extension(std::string_view path) noexcept
{
    auto ends_with = [&](std::string_view suffix) {
        return path.size() > suffix.size() &&
               path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    if (ends_with(".cpp") || ends_with(".hpp") || ends_with(".h") || ends_with(".cxx"))
    {
        return std::string("cpp");
    }
    if (ends_with(".py"))
    {
        return std::string("python");
    }
    if (ends_with(".cmd") || ends_with(".bat"))
    {
        return std::string("batch");
    }
    if (ends_with(".cmake") || path.find("CMakeLists.txt") != std::string_view::npos)
    {
        return std::string("cmake");
    }
    if (ends_with(".md"))
    {
        return std::string("markdown");
    }
    if (ends_with(".yaml") || ends_with(".yml"))
    {
        return std::string("yaml");
    }
    return std::nullopt;
}

std::vector<std::string> extract_explicit_ids(std::string_view text)
{
    std::vector<std::string> ids;
    usize position = 0;
    while (position < text.size())
    {
        const usize run_begin = position;
        // the leading word must NOT consume '-': "ADR-0024" splits so the
        // id prefix is recognized before its tail
        const std::string token = lower_alpha_run(text, position, /*allow_dash=*/false);
        const bool token_like =
            token == "adr" || token == "obl" || token == "mem" || token == "tca";
        if (token_like && position < text.size() && text[position] == '-')
        {
            usize after_dash = position + 1;
            // consume the id tail: digits/hex/uppercase runs joined by '-'
            while (after_dash < text.size() &&
                   ((text[after_dash] >= '0' && text[after_dash] <= '9') ||
                    (text[after_dash] >= 'a' && text[after_dash] <= 'z') ||
                    (text[after_dash] >= 'A' && text[after_dash] <= 'Z') ||
                    text[after_dash] == '-'))
            {
                ++after_dash;
            }
            std::string candidate;
            for (const char ch : token)
            {
                candidate.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
            }
            candidate.push_back('-');
            for (usize i = position + 1; i < after_dash; ++i)
            {
                candidate.push_back(
                    static_cast<char>(std::toupper(static_cast<unsigned char>(text[i]))));
            }
            // normalize: ADR-00NN (4 digits), OBL-/MEM-/TCA- keep their shape
            ids.push_back(std::move(candidate));
            position = after_dash;
            continue;
        }
        if (position == run_begin)
        {
            ++position; // skip separators
        }
    }
    // dedupe, keep first-seen order (deterministic)
    std::vector<std::string> unique;
    for (std::string& id : ids)
    {
        if (std::find(unique.begin(), unique.end(), id) == unique.end())
        {
            unique.push_back(std::move(id));
        }
    }
    return unique;
}

std::string_view phase_text(TaskPhase phase) noexcept
{
    switch (phase)
    {
    case TaskPhase::Specify:
        return "specify";
    case TaskPhase::Design:
        return "design";
    case TaskPhase::Implementation:
        return "implementation";
    case TaskPhase::Review:
        return "review";
    case TaskPhase::Acceptance:
        return "acceptance";
    }
    return "design";
}

std::string_view risk_text(TaskRisk risk) noexcept
{
    switch (risk)
    {
    case TaskRisk::R0:
        return "R0";
    case TaskRisk::R1:
        return "R1";
    case TaskRisk::R2:
        return "R2";
    case TaskRisk::R3:
        return "R3";
    }
    return "R2";
}

TaskDescriptor normalize_task(const TaskEnvelope& envelope)
{
    TaskDescriptor descriptor;
    descriptor.objective      = envelope.objective;
    descriptor.repository     = envelope.repository;
    descriptor.revision       = envelope.revision;
    descriptor.phase          = envelope.phase;
    descriptor.risk           = envelope.risk;
    descriptor.phase_observed = envelope.phase_observed;
    descriptor.risk_observed  = envelope.risk_observed;
    descriptor.changed_paths  = envelope.changed_paths;

    for (const std::string& path : envelope.changed_paths)
    {
        // path prefix: leading components up to the file (a/b/c.md → a/, a/b/)
        const usize last_slash = path.find_last_of('/');
        if (last_slash != std::string::npos && last_slash > 0)
        {
            std::string prefix;
            usize slash = path.find('/');
            while (slash != std::string::npos && slash < last_slash)
            {
                prefix = path.substr(0, slash + 1);
                if (std::find(descriptor.path_prefixes.begin(),
                              descriptor.path_prefixes.end(),
                              prefix) == descriptor.path_prefixes.end())
                {
                    descriptor.path_prefixes.push_back(prefix);
                }
                slash = path.find('/', slash + 1);
            }
        }
        if (const auto language = language_of_extension(path))
        {
            if (std::find(descriptor.languages.begin(), descriptor.languages.end(), *language) ==
                descriptor.languages.end())
            {
                descriptor.languages.push_back(*language);
            }
        }
    }

    descriptor.explicit_ids = extract_explicit_ids(envelope.objective);
    // boundary_kinds stay EMPTY: no fixed mechanical derivation exists in
    // v1 (judgment-bearing field law; curator input has no code path).
    return descriptor;
}

std::string TaskDescriptor::canonical_json() const
{
    std::string out = "{";
    json_escape(out, "objective");
    out.push_back(':');
    json_escape(out, objective);
    out.push_back(',');
    json_escape(out, "repository");
    out.push_back(':');
    json_escape(out, repository);
    out.push_back(',');
    json_escape(out, "revision");
    out.push_back(':');
    json_escape(out, revision);
    out.push_back(',');
    json_escape(out, "phase");
    out.push_back(':');
    json_escape(out, std::string(phase_text(phase)));
    out.push_back(',');
    json_escape(out, "risk");
    out.push_back(':');
    json_escape(out, std::string(risk_text(risk)));
    append_string_list(out, "changed_paths", changed_paths);
    append_string_list(out, "path_prefixes", path_prefixes);
    append_string_list(out, "languages", languages);
    append_string_list(out, "explicit_ids", explicit_ids);
    append_string_list(out, "boundary_kinds", boundary_kinds);
    out.push_back('}');
    return out;
}

std::string TaskDescriptor::digest_hex() const
{
    const std::string json = canonical_json();
    qiven::SHA256Hasher hasher;
    hasher.update(reinterpret_cast<const std::byte*>(json.data()), json.size());
    const qiven::SHA256Digest digest = hasher.finish();
    static constexpr char hex[]      = "0123456789abcdef";
    std::string out;
    out.resize(digest.size() * 2);
    for (usize i = 0; i < digest.size(); ++i)
    {
        const auto b   = static_cast<unsigned char>(digest[i]);
        out[2 * i]     = hex[b >> 4];
        out[2 * i + 1] = hex[b & 0x0Fu];
    }
    return out;
}
} // namespace qiven::runtime::cognition
