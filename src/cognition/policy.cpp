#include <qiven/runtime/cognition/policy.hpp>

#include <charconv>
#include <optional>

namespace qiven::runtime::cognition
{
namespace
{
PolicyParseError bad(usize line, std::string_view detail)
{
    return PolicyParseError { err_policy_malformed, line, std::string(detail) };
}

// --- frozen v4 vocabulary tables (draft pin 7583724) -----------------------
std::optional<qiven::context::ActionKind> parse_action_kind(std::string_view name)
{
    using K = qiven::context::ActionKind;
    for (const auto& [candidate, value] :
         std::initializer_list<std::pair<std::string_view, K>> {
             { "BeginTask", K::BeginTask },
             { "EnterDomain", K::EnterDomain },
             { "CreateCppSymbol", K::CreateCppSymbol },
             { "IntroducePrimitive", K::IntroducePrimitive },
             { "ModifyArchitecture", K::ModifyArchitecture },
             { "ModifyPublicAPI", K::ModifyPublicAPI },
             { "InvokeTool", K::InvokeTool },
             { "RetryFailure", K::RetryFailure },
             { "MakeCanonicalClaim", K::MakeCanonicalClaim },
             { "MakeLiveClaim", K::MakeLiveClaim },
             { "ModifyReferencedContract", K::ModifyReferencedContract },
             { "Commit", K::Commit },
             { "Publish", K::Publish },
             { "AcceptCandidate", K::AcceptCandidate } })
    {
        if (name == candidate)
        {
            return value;
        }
    }
    return std::nullopt;
}

std::optional<qiven::context::RequirementKind> parse_requirement_kind(std::string_view name)
{
    using K = qiven::context::RequirementKind;
    for (const auto& [candidate, value] :
         std::initializer_list<std::pair<std::string_view, K>> {
             { "MandatoryRecall", K::MandatoryRecall },
             { "SearchLowerLayer", K::SearchLowerLayer },
             { "VerifyCanonical", K::VerifyCanonical },
             { "VerifyLive", K::VerifyLive },
             { "InspectToolContract", K::InspectToolContract },
             { "InspectKnownPit", K::InspectKnownPit },
             { "RunMechanicalCheck", K::RunMechanicalCheck },
             { "RequestReview", K::RequestReview },
             { "AskHuman", K::AskHuman } })
    {
        if (name == candidate)
        {
            return value;
        }
    }
    return std::nullopt;
}

std::optional<qiven::context::RequirementBoundary> parse_boundary(std::string_view name)
{
    using B = qiven::context::RequirementBoundary;
    if (name == "BeforeJudgment")
    {
        return B::BeforeJudgment;
    }
    if (name == "BeforeExecution")
    {
        return B::BeforeExecution;
    }
    return std::nullopt;
}

std::optional<qiven::context::ClaimClass> parse_claim_class(std::string_view name)
{
    using C = qiven::context::ClaimClass;
    for (const auto& [candidate, value] :
         std::initializer_list<std::pair<std::string_view, C>> {
             { "LocalRecall", C::LocalRecall },
             { "CanonicalFact", C::CanonicalFact },
             { "LiveFact", C::LiveFact },
             { "EvidenceInterpretation", C::EvidenceInterpretation },
             { "Judgment", C::Judgment } })
    {
        if (name == candidate)
        {
            return value;
        }
    }
    return std::nullopt;
}

struct Field
{
    std::string key;
    std::string value;
};

std::optional<Field> split_field(std::string_view line)
{
    const usize colon = line.find(':');
    if (colon == std::string_view::npos)
    {
        return std::nullopt;
    }
    std::string key(line.substr(0, colon));
    std::string value(line.substr(colon + 1));
    while (!value.empty() && (value.back() == ' ' || value.back() == '\r'))
    {
        value.pop_back();
    }
    while (!value.empty() && value.front() == ' ')
    {
        value.erase(value.begin());
    }
    if (!value.empty() && value.front() == '"' && value.back() == '"' && value.size() >= 2)
    {
        value = value.substr(1, value.size() - 2);
    }
    return Field { std::move(key), std::move(value) };
}

std::optional<u64> parse_u64(std::string_view text)
{
    if (text.empty())
    {
        return std::nullopt;
    }
    u64 value            = 0;
    const auto converted = std::from_chars(text.data(), text.data() + text.size(), value);
    if (converted.ec != std::errc {} || converted.ptr != text.data() + text.size())
    {
        return std::nullopt;
    }
    return value;
}

std::optional<bool> parse_bool(std::string_view text)
{
    if (text == "true")
    {
        return true;
    }
    if (text == "false")
    {
        return false;
    }
    return std::nullopt;
}
} // namespace

PolicyParseResult parse_invocation_policy(std::string_view bytes)
{
    if (bytes.size() > max_policy_bytes)
    {
        return PolicyParseResult::fail(bad(0, "policy input exceeds the bounded size"));
    }

    ParsedPolicy parsed;
    parsed.invocation.present = false;

    enum class Section : u8
    {
        None,
        PolicyHeader,
        Rules,
        Resolvers,
        Freshness,
        Enforcement
    };
    Section section        = Section::None;
    bool schema_seen       = false;
    bool present_seen      = false;
    bool have_evidence_ttl = false;
    bool have_bundle_ttl   = false;
    bool have_enf_bj       = false;
    bool have_enf_be       = false;

    usize line_number = 0;
    usize line_begin  = 0;
    bool ok           = true;
    while (line_begin <= bytes.size() && ok)
    {
        const usize newline = bytes.find('\n', line_begin);
        std::string_view line =
            newline == std::string_view::npos
                ? bytes.substr(line_begin)
                : bytes.substr(line_begin, newline - line_begin);
        ++line_number;
        if (newline == std::string_view::npos)
        {
            line_begin = bytes.size() + 1;
        }
        else
        {
            line_begin = newline + 1;
        }

        while (!line.empty() && line.back() == '\r')
        {
            line.remove_suffix(1);
        }
        if (line.find('\t') != std::string_view::npos)
        {
            ok = false;
            break;
        }
        const bool blank_or_comment = line.empty() || line.front() == '#';

        if (!blank_or_comment && line.front() != ' ')
        {
            auto field = split_field(line);
            if (!field)
            {
                ok = false;
                break;
            }
            const bool empty_value = field->value.empty();
            if (field->key == "schema")
            {
                schema_seen = field->value == "qiven-invocation-policy-v1";
                ok          = schema_seen;
                section     = Section::None;
            }
            else if (field->key == "version")
            {
                const auto version = parse_u64(field->value);
                ok                 = version.has_value() && *version == 1;
            }
            else if (field->key == "policy")
            {
                ok      = empty_value;
                section = Section::PolicyHeader;
            }
            else if (field->key == "resolvers")
            {
                ok      = empty_value;
                section = Section::Resolvers;
            }
            else if (field->key == "freshness")
            {
                ok      = empty_value;
                section = Section::Freshness;
            }
            else if (field->key == "enforcement")
            {
                ok      = empty_value;
                section = Section::Enforcement;
            }
            else
            {
                ok = false; // unknown top-level key
            }
            continue;
        }

        if (blank_or_comment)
        {
            continue;
        }

        // Indented content: strip leading spaces (rule rows sit at 4, their
        // continuation fields at 6; anything past 8 is outside the shape),
        // then the row is either a list head "- key: value" or a scalar
        // continuation "key: value".
        usize indent = 0;
        while (indent < line.size() && line[indent] == ' ')
        {
            ++indent;
        }
        if (indent > 8)
        {
            ok = false;
            break;
        }
        std::string_view body                  = line.substr(indent);
        constexpr std::string_view list_prefix = "- ";
        const bool is_list_row                 = body.rfind(list_prefix, 0) == 0;
        if (is_list_row)
        {
            body.remove_prefix(list_prefix.size());
        }
        auto field = split_field(body);
        if (!field)
        {
            ok = false;
            break;
        }

        switch (section)
        {
        case Section::PolicyHeader:
            if (is_list_row)
            {
                ok = false;
                break;
            }
            if (field->key == "present")
            {
                const auto present = parse_bool(field->value);
                if (present.has_value())
                {
                    parsed.invocation.present = *present;
                    present_seen              = true;
                }
                else
                {
                    ok = false;
                }
            }
            else if (field->key == "rules")
            {
                ok      = field->value.empty();
                section = Section::Rules;
            }
            else
            {
                ok = false;
            }
            break;

        case Section::Rules:
        {
            if (is_list_row)
            {
                if (parsed.invocation.rules.size() >= max_rules)
                {
                    ok = false;
                    break;
                }
                parsed.invocation.rules.emplace_back();
            }
            else if (parsed.invocation.rules.empty())
            {
                ok = false;
                break;
            }
            auto& rule = parsed.invocation.rules.back();
            if (field->key == "action")
            {
                const auto value = parse_action_kind(field->value);
                rule.action      = value.value_or(rule.action);
                ok               = value.has_value();
            }
            else if (field->key == "claim")
            {
                const auto value = parse_claim_class(field->value);
                if (value)
                {
                    rule.claimClass = *value;
                }
                ok = value.has_value();
            }
            else if (field->key == "requirement")
            {
                const auto value = parse_requirement_kind(field->value);
                rule.requirement = value.value_or(rule.requirement);
                ok               = value.has_value();
            }
            else if (field->key == "boundary")
            {
                const auto value = parse_boundary(field->value);
                rule.boundary    = value.value_or(rule.boundary);
                ok               = value.has_value();
            }
            else if (field->key == "subject")
            {
                rule.subject = field->value;
                ok           = !field->value.empty();
            }
            else if (field->key == "blocking")
            {
                const auto value = parse_bool(field->value);
                if (value)
                {
                    rule.blocking = *value;
                }
                ok = value.has_value();
            }
            else
            {
                ok = false;
            }
            break;
        }

        case Section::Resolvers:
        {
            if (is_list_row)
            {
                parsed.resolvers.emplace_back();
            }
            else if (parsed.resolvers.empty())
            {
                ok = false;
                break;
            }
            auto& resolver = parsed.resolvers.back();
            if (field->key == "requirement")
            {
                const auto value = parse_requirement_kind(field->value);
                resolver.kind    = value.value_or(resolver.kind);
                ok               = value.has_value();
            }
            else if (field->key == "type")
            {
                resolver.type = field->value;
                ok            = !field->value.empty();
            }
            else if (field->key == "min_version")
            {
                const auto value = parse_u64(field->value);
                if (value && *value >= 1)
                {
                    resolver.min_version = *value;
                }
                ok = value.has_value() && *value >= 1;
            }
            else
            {
                ok = false;
            }
            break;
        }

        case Section::Freshness:
            if (is_list_row)
            {
                ok = false;
                break;
            }
            if (field->key == "evidence_ttl_ms")
            {
                const auto value = parse_u64(field->value);
                if (value && *value > 0)
                {
                    parsed.evidence_ttl_ms = *value;
                    have_evidence_ttl      = true;
                }
                else
                {
                    ok = false;
                }
            }
            else if (field->key == "bundle_freshness_ms")
            {
                const auto value = parse_u64(field->value);
                if (value && *value > 0)
                {
                    parsed.bundle_freshness_ms = *value;
                    have_bundle_ttl            = true;
                }
                else
                {
                    ok = false;
                }
            }
            else
            {
                ok = false;
            }
            break;

        case Section::Enforcement:
            if (is_list_row)
            {
                ok = false;
                break;
            }
            if (field->key == "unsatisfied_before_judgment")
            {
                if (field->value == "redeliberate")
                {
                    parsed.redeliberate_on_before_judgment = true;
                    have_enf_bj                            = true;
                }
                else if (field->value == "deny")
                {
                    parsed.redeliberate_on_before_judgment = false;
                    have_enf_bj                            = true;
                }
                else
                {
                    ok = false;
                }
            }
            else if (field->key == "unsatisfied_before_execution")
            {
                if (field->value == "deny")
                {
                    parsed.deny_on_before_execution = true;
                    have_enf_be                     = true;
                }
                else
                {
                    ok = false;
                }
            }
            else
            {
                ok = false;
            }
            break;

        case Section::None:
            ok = false;
            break;
        }
    }

    if (!ok)
    {
        return PolicyParseResult::fail(
            bad(line_number, "line is outside the bounded policy shape"));
    }
    if (!schema_seen)
    {
        return PolicyParseResult::fail(bad(0, "schema header is required"));
    }
    if (!present_seen || !parsed.invocation.present)
    {
        return PolicyParseResult::fail(
            bad(0, "policy.present must be present and true (a fail-closed instance is not loadable)"));
    }
    if (parsed.invocation.rules.empty())
    {
        return PolicyParseResult::fail(bad(0, "policy.rules must carry at least one rule"));
    }
    for (const auto& rule : parsed.invocation.rules)
    {
        if (rule.subject.empty())
        {
            return PolicyParseResult::fail(bad(0, "every rule requires a subject"));
        }
    }
    for (const auto& resolver : parsed.resolvers)
    {
        if (resolver.type.empty())
        {
            return PolicyParseResult::fail(bad(0, "every resolver requires a type"));
        }
    }
    if (!have_evidence_ttl || !have_bundle_ttl)
    {
        return PolicyParseResult::fail(bad(0, "both freshness windows are required"));
    }
    if (!have_enf_bj || !have_enf_be)
    {
        return PolicyParseResult::fail(bad(0, "both enforcement behaviors are required"));
    }
    return PolicyParseResult(std::move(parsed));
}
} // namespace qiven::runtime::cognition
