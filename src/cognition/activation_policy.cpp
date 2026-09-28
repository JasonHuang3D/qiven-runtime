#include <qiven/runtime/cognition/activation_policy.hpp>

#include <charconv>

namespace qiven::runtime::cognition
{
namespace
{
using CoreOutcome   = qiven::Result<CognitionCore, ActivationPolicyParseError>;
using PolicyOutcome = qiven::Result<ActivationPolicy, ActivationPolicyParseError>;

ActivationPolicyParseError fail(ActivationPolicyError kind, usize line, std::string detail)
{
    return ActivationPolicyParseError { kind, line, std::move(detail) };
}

struct Line
{
    usize number = 0;
    usize indent = 0;
    std::string_view text; // trimmed, comments stripped
};

// Split into tracked lines; strip #-comments (outside quotes), blanks.
std::vector<Line> tracked_lines(std::string_view bytes)
{
    std::vector<Line> lines;
    usize number = 0;
    usize start  = 0;
    while (start <= bytes.size())
    {
        const usize newline = bytes.find('\n', start);
        const std::string_view raw =
            bytes.substr(start, newline == std::string_view::npos ? std::string_view::npos
                                                                  : newline - start);
        ++number;
        // comment strip: outside single/double quotes
        bool in_single = false;
        bool in_double = false;
        usize content  = raw.size();
        for (usize i = 0; i < raw.size(); ++i)
        {
            const char ch = raw[i];
            if (ch == '\'' && !in_double)
            {
                in_single = !in_single;
            }
            else if (ch == '"' && !in_single)
            {
                in_double = !in_double;
            }
            else if (ch == '#' && !in_single && !in_double)
            {
                content = i;
                break;
            }
        }
        std::string_view text = raw.substr(0, content);
        usize indent          = 0;
        while (indent < text.size() && (text[indent] == ' ' || text[indent] == '\t'))
        {
            ++indent;
        }
        usize end = text.size();
        while (end > indent && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r'))
        {
            --end;
        }
        text = text.substr(indent, end - indent);
        if (!text.empty())
        {
            lines.push_back(Line { number, indent, text });
        }
        if (newline == std::string_view::npos)
        {
            break;
        }
        start = newline + 1;
    }
    return lines;
}

// "key: value" split at the FIRST colon (value may be empty).
[[nodiscard]] bool split_key(std::string_view line, std::string_view& key,
                             std::string_view& value)
{
    const usize colon = line.find(':');
    if (colon == std::string_view::npos || colon == 0)
    {
        return false;
    }
    key               = line.substr(0, colon);
    usize value_begin = colon + 1;
    while (value_begin < line.size() && line[value_begin] == ' ')
    {
        ++value_begin;
    }
    usize value_end = line.size();
    while (value_end > value_begin && line[value_end - 1] == ' ')
    {
        --value_end;
    }
    value = line.substr(value_begin, value_end - value_begin);
    return true;
}

[[nodiscard]] std::string unquote(std::string_view value)
{
    if (value.size() >= 2 && value.front() == '\'' && value.back() == '\'')
    {
        return std::string(value.substr(1, value.size() - 2));
    }
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
    {
        return std::string(value.substr(1, value.size() - 2));
    }
    return std::string(value);
}

// "- item" list entry at the given indent; text is post-indent.
[[nodiscard]] bool list_item(std::string_view line, std::string_view& item)
{
    if (line.size() < 2 || line[0] != '-' || line[1] != ' ')
    {
        return false;
    }
    usize begin = 2;
    while (begin < line.size() && line[begin] == ' ')
    {
        ++begin;
    }
    item = line.substr(begin);
    return !item.empty();
}

[[nodiscard]] std::optional<u64> parse_u64(std::string_view text) noexcept
{
    u64 value         = 0;
    const auto* first = text.data();
    const auto* last  = text.data() + text.size();
    if (first == last)
    {
        return std::nullopt;
    }
    const auto result = std::from_chars(first, last, value, 10);
    if (result.ec != std::errc {} || result.ptr != last)
    {
        return std::nullopt;
    }
    return value;
}

// schema v1 exact vocabulary -----------------------------------------------
bool in(std::string_view value, std::initializer_list<std::string_view> set) noexcept
{
    for (const std::string_view candidate : set)
    {
        if (value == candidate)
        {
            return true;
        }
    }
    return false;
}
} // namespace

std::optional<PriorityClass> parse_priority_class(std::string_view name) noexcept
{
    if (name == "P0-core")
    {
        return PriorityClass::P0Core;
    }
    if (name == "P1-protected")
    {
        return PriorityClass::P1Protected;
    }
    if (name == "P2-required")
    {
        return PriorityClass::P2Required;
    }
    if (name == "P3-supporting")
    {
        return PriorityClass::P3Supporting;
    }
    if (name == "P4-on-demand")
    {
        return PriorityClass::P4OnDemand;
    }
    return std::nullopt;
}

std::optional<RuleLifecycle> parse_lifecycle(std::string_view name) noexcept
{
    if (name == "active")
    {
        return RuleLifecycle::Active;
    }
    if (name == "superseded")
    {
        return RuleLifecycle::Superseded;
    }
    if (name == "legacy")
    {
        return RuleLifecycle::Legacy;
    }
    if (name == "retired")
    {
        return RuleLifecycle::Retired;
    }
    return std::nullopt;
}

bool is_schema_v1_phase(std::string_view value) noexcept
{
    return in(value, { "specify", "design", "implementation", "review", "acceptance", "all" });
}

bool is_schema_v1_risk(std::string_view value) noexcept
{
    return in(value, { "R0", "R1", "R2", "R3", "all" });
}

bool is_schema_v1_language(std::string_view value) noexcept
{
    return in(value, { "cpp", "python", "batch", "cmake", "markdown", "yaml" });
}

bool is_schema_v1_boundary_kind(std::string_view value) noexcept
{
    return in(value, { "ownership", "lifetime", "representation", "serialization", "ipc",
                       "persistence", "concurrency", "platform", "external-contract", "security",
                       "process-custody", "filesystem", "git", "cognition", "governance",
                       "tooling", "human-interface" });
}

bool is_schema_v1_evidence(std::string_view value) noexcept
{
    return in(value, { "mechanical", "environmental", "fresh-cognitive", "owner-boundary" });
}

// --- cognition-core.yaml ---------------------------------------------------
CoreOutcome parse_cognition_core(std::string_view bytes)
{
    if (bytes.size() > max_activation_policy_bytes)
    {
        return CoreOutcome::fail(fail(ActivationPolicyError::BoundExceeded, 0, "core too large"));
    }
    const std::vector<Line> lines = tracked_lines(bytes);
    CognitionCore core;
    std::string section;
    std::string subsection;
    std::optional<CorpusRepository> corpus_repo;
    std::optional<CapabilitySurface> capability;
    usize filter_indent = 0;
    bool in_filters     = false;

    // Fail-closed law: an incomplete corpus entry is NEVER silently
    // dropped (a narrowed closure would silently change the lock digest).
    std::optional<ActivationPolicyParseError> close_error;
    auto close_repo = [&](usize line) {
        if (corpus_repo)
        {
            if (corpus_repo->repository.empty())
            {
                close_error = fail(ActivationPolicyError::MissingField, line,
                                   "corpus repository entry has no repository name");
            }
            else if (corpus_repo->path_filters.empty())
            {
                close_error = fail(ActivationPolicyError::MissingField, line,
                                   "corpus repository '" + corpus_repo->repository +
                                       "' has no path_filters - a silent closure narrowing is "
                                       "forbidden");
            }
            else
            {
                core.corpus.push_back(std::move(*corpus_repo));
            }
            corpus_repo.reset();
        }
    };
    auto close_capability = [&](usize line) {
        if (capability)
        {
            // Fail-closed law (same class as close_repo): a partially
            // declared capability surface is never silently dropped - a
            // missing surface silently narrows semantic-owner resolution.
            if (capability->repository.empty() || capability->path.empty())
            {
                close_error = fail(ActivationPolicyError::MissingField, line,
                                   "capability surface entry needs both repository and path");
            }
            else
            {
                core.capabilities.push_back(std::move(*capability));
            }
            capability.reset();
        }
    };

    for (const Line& line : lines)
    {
        if (line.indent == 0)
        {
            close_repo(line.number);
            if (close_error)
            {
                return CoreOutcome::fail(*close_error);
            }
            close_capability(line.number);
            subsection.clear();
            std::string_view key;
            std::string_view value;
            if (line.text.front() == '-')
            {
                return CoreOutcome::fail(
                    fail(ActivationPolicyError::Malformed, line.number, "list at top level"));
            }
            if (!split_key(line.text, key, value))
            {
                return CoreOutcome::fail(
                    fail(ActivationPolicyError::Malformed, line.number, "expected key: value"));
            }
            if (key == "schema_version")
            {
                const auto version = parse_u64(value);
                if (!version || *version != 1)
                {
                    return CoreOutcome::fail(fail(ActivationPolicyError::UnknownVocabulary,
                                                  line.number, "schema_version must be 1"));
                }
                core.schema_version = 1;
            }
            else if (value.empty())
            {
                section = std::string(key);
            }
            else
            {
                return CoreOutcome::fail(fail(ActivationPolicyError::Malformed, line.number,
                                              "unexpected top-level value for " + std::string(key)));
            }
            continue;
        }

        if (section == "corpus")
        {
            if (line.text == "repositories:" && line.indent == 2)
            {
                subsection = "repositories";
                continue;
            }
            if (subsection == "repositories")
            {
                std::string_view item;
                if (line.indent == 4 && list_item(line.text, item))
                {
                    close_repo(line.number);
                    if (close_error)
                    {
                        return CoreOutcome::fail(*close_error);
                    }
                    corpus_repo = CorpusRepository {};
                    std::string_view key;
                    std::string_view value;
                    if (split_key(item, key, value) && key == "repository")
                    {
                        corpus_repo->repository = unquote(value);
                        filter_indent           = 4;
                        in_filters              = false;
                        continue;
                    }
                    return CoreOutcome::fail(fail(ActivationPolicyError::Malformed, line.number,
                                                  "repository list entry must start with repository:"));
                }
                if (corpus_repo && line.text == "path_filters:")
                {
                    filter_indent = line.indent; // items follow DEEPER than this key
                    in_filters    = true;
                    continue;
                }
                if (corpus_repo && in_filters && line.indent > filter_indent &&
                    list_item(line.text, item))
                {
                    corpus_repo->path_filters.push_back(unquote(item));
                    continue;
                }
                return CoreOutcome::fail(
                    fail(ActivationPolicyError::Malformed, line.number, "unexpected corpus line"));
            }
            return CoreOutcome::fail(
                fail(ActivationPolicyError::Malformed, line.number, "unexpected corpus line"));
        }

        if (section == "capabilities")
        {
            std::string_view item;
            if (list_item(line.text, item))
            {
                close_capability(line.number);
                capability = CapabilitySurface {};
                std::string_view key;
                std::string_view value;
                if (split_key(item, key, value) && key == "repository")
                {
                    capability->repository = unquote(value);
                    continue;
                }
                return CoreOutcome::fail(fail(ActivationPolicyError::Malformed, line.number,
                                              "capability entry must start with repository:"));
            }
            if (capability)
            {
                std::string_view key;
                std::string_view value;
                if (split_key(line.text, key, value) && key == "path")
                {
                    capability->path = unquote(value);
                    continue;
                }
            }
            return CoreOutcome::fail(
                fail(ActivationPolicyError::Malformed, line.number, "unexpected capability line"));
        }

        if (section == "budgets" || section == "latency_objectives")
        {
            std::string_view key;
            std::string_view value;
            if (!split_key(line.text, key, value))
            {
                return CoreOutcome::fail(
                    fail(ActivationPolicyError::Malformed, line.number, "expected key: value"));
            }
            const auto number = parse_u64(value);
            if (!number)
            {
                return CoreOutcome::fail(
                    fail(ActivationPolicyError::Malformed, line.number, "expected integer"));
            }
            if (section == "budgets")
            {
                if (key == "compact_core_max_bytes")
                {
                    core.compact_core_max_bytes = *number;
                }
                else if (key == "task_payload_max_bytes")
                {
                    core.task_payload_max_bytes = *number;
                }
                else if (key == "inlined_body_max_bytes")
                {
                    core.inlined_body_max_bytes = *number;
                }
                else if (key == "supporting_share_max_percent")
                {
                    core.supporting_share_max_percent = *number;
                }
                else
                {
                    return CoreOutcome::fail(fail(ActivationPolicyError::Malformed, line.number,
                                                  "unknown budget key"));
                }
            }
            else
            {
                if (key == "cold_rebuild_max_ms")
                {
                    core.cold_rebuild_max_ms = *number;
                }
                else if (key == "warm_activation_p95_max_ms")
                {
                    core.warm_activation_p95_max_ms = *number;
                }
                else
                {
                    return CoreOutcome::fail(fail(ActivationPolicyError::Malformed, line.number,
                                                  "unknown latency key"));
                }
            }
            continue;
        }

        if (section == "registry")
        {
            std::string_view key;
            std::string_view value;
            if (!split_key(line.text, key, value))
            {
                return CoreOutcome::fail(
                    fail(ActivationPolicyError::Malformed, line.number, "expected key: value"));
            }
            const std::string text = unquote(value);
            if (key == "selector_schema_version")
            {
                const auto version = parse_u64(value);
                if (!version || *version != 1)
                {
                    return CoreOutcome::fail(fail(ActivationPolicyError::UnknownVocabulary,
                                                  line.number, "selector schema must be 1"));
                }
                core.selector_schema_version = static_cast<u32>(*version);
            }
            else if (key == "activation_policy_path")
            {
                core.activation_policy_path = text;
            }
            else if (key == "task_schema_repository")
            {
                core.task_schema_repository = text;
            }
            else if (key == "task_schema_path")
            {
                core.task_schema_path = text;
            }
            else if (key == "fixtures_repository")
            {
                core.fixtures_repository = text;
            }
            else if (key == "fixtures_path")
            {
                core.fixtures_path = text;
            }
            else
            {
                return CoreOutcome::fail(
                    fail(ActivationPolicyError::Malformed, line.number, "unknown registry key"));
            }
            continue;
        }

        if (section == "consumer_profiles")
        {
            std::string_view item;
            if (list_item(line.text, item))
            {
                core.consumer_profiles.push_back(unquote(item));
                continue;
            }
            return CoreOutcome::fail(fail(ActivationPolicyError::Malformed, line.number,
                                          "consumer_profiles must be a list"));
        }

        return CoreOutcome::fail(
            fail(ActivationPolicyError::Malformed, line.number, "unknown section " + section));
    }
    close_repo(lines.empty() ? 0 : lines.back().number);
    if (close_error)
    {
        return CoreOutcome::fail(*close_error);
    }
    close_capability(lines.empty() ? 0 : lines.back().number);
    if (close_error)
    {
        return CoreOutcome::fail(*close_error);
    }

    if (core.schema_version != 1 || core.corpus.empty() || core.compact_core_max_bytes == 0 ||
        core.task_payload_max_bytes == 0 || core.inlined_body_max_bytes == 0 ||
        core.supporting_share_max_percent == 0 || core.activation_policy_path.empty() ||
        core.consumer_profiles.empty() || core.cold_rebuild_max_ms == 0 ||
        core.warm_activation_p95_max_ms == 0 || core.selector_schema_version != 1 ||
        core.task_schema_repository.empty() || core.task_schema_path.empty() ||
        core.fixtures_repository.empty() || core.fixtures_path.empty())
    {
        return CoreOutcome::fail(
            fail(ActivationPolicyError::MissingField, 0, "core is missing required fields"));
    }
    return CoreOutcome(std::move(core));
}

// --- cognition-activation-policy.yaml --------------------------------------
PolicyOutcome parse_activation_policy(std::string_view bytes)
{
    if (bytes.size() > max_activation_policy_bytes)
    {
        return PolicyOutcome::fail(
            fail(ActivationPolicyError::BoundExceeded, 0, "policy too large"));
    }
    const std::vector<Line> lines = tracked_lines(bytes);
    ActivationPolicy policy;
    std::optional<ActivationRule> rule;
    std::string field;    // source | selectors | expected_controls | independent_evidence | explicit_ids | path_filters
    std::string list_key; // active list inside selectors
    usize list_indent = 0;

    auto close_rule = [&]() {
        if (rule)
        {
            policy.rules.push_back(std::move(*rule));
            rule.reset();
        }
        field.clear();
        list_key.clear();
    };

    for (const Line& line : lines)
    {
        if (line.indent == 0)
        {
            close_rule();
            std::string_view key;
            std::string_view value;
            if (!split_key(line.text, key, value))
            {
                return PolicyOutcome::fail(
                    fail(ActivationPolicyError::Malformed, line.number, "expected key: value"));
            }
            if (key == "schema_version")
            {
                const auto version = parse_u64(value);
                if (!version || *version != 1)
                {
                    return PolicyOutcome::fail(fail(ActivationPolicyError::UnknownVocabulary,
                                                    line.number, "schema_version must be 1"));
                }
                policy.schema_version = 1;
            }
            else if (key == "rules" && value.empty())
            {
                // rule list follows
            }
            else
            {
                return PolicyOutcome::fail(
                    fail(ActivationPolicyError::Malformed, line.number, "unknown top-level key"));
            }
            continue;
        }

        std::string_view item;
        // indent-2 list entry = the NEXT rule (closes the previous one even
        // while its nested field is still active)
        if (line.indent == 2 && list_item(line.text, item))
        {
            close_rule();
            rule = ActivationRule {};
            std::string_view key;
            std::string_view value;
            if (split_key(item, key, value) && key == "rule_id")
            {
                rule->rule_id = unquote(value);
                continue;
            }
            return PolicyOutcome::fail(fail(ActivationPolicyError::Malformed, line.number,
                                            "rule entry must start with rule_id:"));
        }
        if (!rule)
        {
            return PolicyOutcome::fail(
                fail(ActivationPolicyError::Malformed, line.number, "expected rule entry"));
        }

        // inside a rule
        // indent-4 rule keys (and nested-section headers) always dispatch:
        // a new key at this level ends the previous nested section
        if (line.indent == 4)
        {
            list_key.clear();
            std::string_view key;
            std::string_view value;
            if (!split_key(line.text, key, value))
            {
                return PolicyOutcome::fail(
                    fail(ActivationPolicyError::Malformed, line.number, "expected key or list"));
            }
            const std::string key_text(key);
            if (key_text == "priority_class")
            {
                const auto parsed = parse_priority_class(value);
                if (!parsed)
                {
                    return PolicyOutcome::fail(fail(ActivationPolicyError::UnknownVocabulary,
                                                    line.number, "unknown priority_class"));
                }
                rule->priority_class = *parsed;
            }
            else if (key_text == "lifecycle")
            {
                const auto parsed = parse_lifecycle(value);
                if (!parsed)
                {
                    return PolicyOutcome::fail(fail(ActivationPolicyError::UnknownVocabulary,
                                                    line.number, "unknown lifecycle"));
                }
                rule->lifecycle = *parsed;
            }
            else if (value.empty() &&
                     (key_text == "source" || key_text == "selectors" ||
                      key_text == "expected_controls" || key_text == "independent_evidence" ||
                      key_text == "explicit_ids"))
            {
                field = key_text;
            }
            else
            {
                return PolicyOutcome::fail(fail(ActivationPolicyError::Malformed, line.number,
                                                "unknown rule key " + key_text));
            }
            continue;
        }

        if (field == "source")
        {
            std::string_view key;
            std::string_view value;
            if (split_key(line.text, key, value))
            {
                const std::string key_text(key);
                const std::string text = unquote(value);
                if (key_text == "repository")
                {
                    rule->source.repository = text;
                }
                else if (key_text == "path")
                {
                    rule->source.path = text;
                }
                else if (key_text == "anchor")
                {
                    rule->source.anchor = text;
                }
                else
                {
                    return PolicyOutcome::fail(
                        fail(ActivationPolicyError::Malformed, line.number, "unknown source key"));
                }
                continue;
            }
            return PolicyOutcome::fail(
                fail(ActivationPolicyError::Malformed, line.number, "expected source key"));
        }

        if (field == "selectors")
        {
            std::string_view key;
            std::string_view value;
            if (split_key(line.text, key, value) &&
                in(key, { "phases", "risk", "repositories", "path_prefixes", "languages",
                          "boundary_kinds", "explicit_ids" }))
            {
                if (!value.empty())
                {
                    // STRICT BLOCK STYLE: a selector list header with an inline
                    // value is flow syntax; discarding it would silently turn
                    // the dimension into an undeclared wildcard.
                    return PolicyOutcome::fail(
                        fail(ActivationPolicyError::Malformed, line.number,
                             "selector list carries an inline value (block style required)"));
                }
                list_key    = std::string(key);
                list_indent = line.indent;
                continue;
            }
            if (!list_key.empty() && line.indent > list_indent)
            {
                if (!list_item(line.text, item))
                {
                    return PolicyOutcome::fail(
                        fail(ActivationPolicyError::Malformed, line.number, "expected list item"));
                }
                const std::string value_text = unquote(item);
                if (list_key == "phases")
                {
                    if (!is_schema_v1_phase(value_text))
                    {
                        return PolicyOutcome::fail(fail(ActivationPolicyError::UnknownVocabulary,
                                                        line.number, "unknown phase"));
                    }
                    rule->selectors.phases.push_back(value_text);
                }
                else if (list_key == "risk")
                {
                    if (!is_schema_v1_risk(value_text))
                    {
                        return PolicyOutcome::fail(fail(ActivationPolicyError::UnknownVocabulary,
                                                        line.number, "unknown risk"));
                    }
                    rule->selectors.risk.push_back(value_text);
                }
                else if (list_key == "repositories")
                {
                    rule->selectors.repositories.push_back(value_text);
                }
                else if (list_key == "path_prefixes")
                {
                    rule->selectors.path_prefixes.push_back(value_text);
                }
                else if (list_key == "languages")
                {
                    if (!is_schema_v1_language(value_text))
                    {
                        return PolicyOutcome::fail(fail(ActivationPolicyError::UnknownVocabulary,
                                                        line.number, "unknown language"));
                    }
                    rule->selectors.languages.push_back(value_text);
                }
                else if (list_key == "boundary_kinds")
                {
                    if (!is_schema_v1_boundary_kind(value_text))
                    {
                        return PolicyOutcome::fail(fail(ActivationPolicyError::UnknownVocabulary,
                                                        line.number, "unknown boundary kind"));
                    }
                    rule->selectors.boundary_kinds.push_back(value_text);
                }
                else if (list_key == "explicit_ids")
                {
                    rule->selectors.explicit_ids.push_back(value_text);
                }
                continue;
            }
            return PolicyOutcome::fail(
                fail(ActivationPolicyError::Malformed, line.number, "unexpected selector line"));
        }

        if (field == "expected_controls" || field == "independent_evidence" ||
            field == "explicit_ids")
        {
            if (list_item(line.text, item))
            {
                const std::string value_text = unquote(item);
                if (field == "expected_controls")
                {
                    rule->expected_controls.push_back(value_text);
                }
                else if (field == "independent_evidence")
                {
                    if (!is_schema_v1_evidence(value_text))
                    {
                        return PolicyOutcome::fail(fail(ActivationPolicyError::UnknownVocabulary,
                                                        line.number, "unknown evidence class"));
                    }
                    rule->independent_evidence.push_back(value_text);
                }
                else
                {
                    rule->selectors.explicit_ids.push_back(value_text);
                }
                continue;
            }
            return PolicyOutcome::fail(
                fail(ActivationPolicyError::Malformed, line.number, "expected list item"));
        }

        return PolicyOutcome::fail(
            fail(ActivationPolicyError::Malformed, line.number, "unexpected rule line"));
    }
    close_rule();

    if (policy.schema_version != 1 || policy.rules.empty())
    {
        return PolicyOutcome::fail(
            fail(ActivationPolicyError::MissingField, 0, "policy is missing required fields"));
    }
    if (policy.rules.size() > max_activation_policy_rules)
    {
        return PolicyOutcome::fail(fail(ActivationPolicyError::BoundExceeded, 0, "too many rules"));
    }
    for (const ActivationRule& entry : policy.rules)
    {
        if (entry.rule_id.empty() || entry.source.repository.empty() || entry.source.path.empty())
        {
            return PolicyOutcome::fail(
                fail(ActivationPolicyError::MissingField, 0, "rule missing id or source"));
        }
        // rule_id charset (the registered schema's pattern): uppercase
        // letters, digits, dashes; anything else fails closed.
        const bool id_ok = !entry.rule_id.empty() &&
                           entry.rule_id.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-") ==
                               std::string::npos;
        if (!id_ok || entry.rule_id.front() == '-')
        {
            return PolicyOutcome::fail(fail(ActivationPolicyError::Malformed, 0,
                                            "rule_id charset (uppercase/digits/dashes): " +
                                                entry.rule_id));
        }
        if (entry.source.anchor.empty())
        {
            return PolicyOutcome::fail(
                fail(ActivationPolicyError::MissingField, 0,
                     "rule '" + entry.rule_id + "' has no source anchor"));
        }
        if (entry.expected_controls.empty())
        {
            return PolicyOutcome::fail(
                fail(ActivationPolicyError::MissingField, 0,
                     "rule '" + entry.rule_id + "' declares no expected_controls"));
        }
    }
    return PolicyOutcome(std::move(policy));
}
} // namespace qiven::runtime::cognition
