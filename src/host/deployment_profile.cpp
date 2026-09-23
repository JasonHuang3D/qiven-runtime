#include <qiven/runtime/host/deployment_profile.hpp>

#include <qiven/runtime/scope.hpp>

#include <charconv>
#include <fstream>
#include <optional>

namespace qiven::runtime::host
{
namespace
{
using port::ProfileAcceptError;
using port::ProfileAcceptErrorKind;

ProfileAcceptError malformed(std::string detail, usize line = 0)
{
    if (line != 0)
    {
        detail += " (line " + std::to_string(line) + ")";
    }
    return ProfileAcceptError { ProfileAcceptErrorKind::Malformed, std::move(detail) };
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

std::optional<adapter::OperationClass> parse_operation_class(std::string_view name)
{
    using O = adapter::OperationClass;
    for (const auto& [candidate, value] :
         std::initializer_list<std::pair<std::string_view, O>> {
             { "Observe", O::Observe },
             { "FileSystemRead", O::FileSystemRead },
             { "FileSystemWrite", O::FileSystemWrite },
             { "ProcessLaunch", O::ProcessLaunch },
             { "NetworkAccess", O::NetworkAccess },
             { "ClaimChannel", O::ClaimChannel } })
    {
        if (name == candidate)
        {
            return value;
        }
    }
    return std::nullopt;
}

std::optional<profile::MediationKind> parse_mediation_kind(std::string_view name)
{
    using M = profile::MediationKind;
    if (name == "ActionInterception")
    {
        return M::ActionInterception;
    }
    if (name == "ClaimChannelOnly")
    {
        return M::ClaimChannelOnly;
    }
    if (name == "Unmediated")
    {
        return M::Unmediated;
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
    return Field { std::move(key), std::move(value) };
}
} // namespace

ProfileFileResult load_profile_file(const std::filesystem::path& file)
{
    std::error_code ec;
    if (!std::filesystem::exists(file, ec) || ec)
    {
        return ProfileFileResult::fail(
            ProfileAcceptError { ProfileAcceptErrorKind::Unavailable, file.string() });
    }
    std::ifstream input(file, std::ios::binary);
    if (!input)
    {
        return ProfileFileResult::fail(
            ProfileAcceptError { ProfileAcceptErrorKind::Unavailable, file.string() });
    }
    std::string bytes((std::istreambuf_iterator<char>(input)),
                      std::istreambuf_iterator<char> {});
    if (bytes.size() > max_profile_bytes)
    {
        return ProfileFileResult::fail(malformed("profile exceeds the bounded size"));
    }

    ProfileFile profile;
    std::vector<profile::ActorBinding> actors;
    std::vector<adapter::CapabilityDescriptor> capabilities;
    std::vector<profile::MediationEntry> mediation;
    profile::ClaimChannelCoverage claims;

    enum class Section : u8
    {
        None,
        GovernedPaths,
        Actors,
        Capabilities,
        Mediation,
        Claims,
        Cognition,
        CognitionSources,
        ToolInventory
    };
    Section section  = Section::None;
    bool schema_seen = false;

    usize line_number = 0;
    usize line_begin  = 0;
    bool ok           = true;
    std::string failing_line;
    auto fail_line = [&]() {
        ok = false;
    };
    while (line_begin <= bytes.size() && ok)
    {
        // bytes is std::string: take the view ONCE — substr on the view
        // returns views; substr on the string would return dangling
        // temporaries under the string_view.
        const std::string_view bytes_view(bytes);
        const usize newline = bytes_view.find('\n', line_begin);
        std::string_view line =
            newline == std::string_view::npos
                ? bytes_view.substr(line_begin)
                : bytes_view.substr(line_begin, newline - line_begin);
        ++line_number;
        failing_line.assign(line);
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
            fail_line();
            break;
        }
        const bool blank_or_comment = line.empty() || line.front() == '#';
        if (blank_or_comment)
        {
            continue;
        }

        if (line.front() != ' ')
        {
            auto field = split_field(line);
            if (!field)
            {
                fail_line();
                break;
            }
            const bool empty_value = field->value.empty();
            if (field->key == "schema")
            {
                schema_seen = field->value == "qiven-deployment-profile-v1";
                ok          = schema_seen;
                section     = Section::None;
            }
            else if (field->key == "profile_id")
            {
                profile.profile_id = field->value;
                ok                 = !field->value.empty();
                section            = Section::None;
            }
            else if (field->key == "revision" || field->key == "control_version" ||
                     field->key == "resolver_registry_revision" ||
                     field->key == "classifier_contract_revision" ||
                     field->key == "freshness_window_ms")
            {
                const auto value = parse_u64(field->value);
                ok               = value.has_value();
                if (ok)
                {
                    if (field->key == "revision")
                    {
                        profile.revision = *value;
                    }
                    else if (field->key == "control_version")
                    {
                        profile.control_version = *value;
                    }
                    else if (field->key == "resolver_registry_revision")
                    {
                        profile.resolver_registry_revision = *value;
                    }
                    else if (field->key == "classifier_contract_revision")
                    {
                        profile.classifier_contract_revision = *value;
                    }
                    else
                    {
                        profile.freshness_window_ms = *value;
                        ok                          = *value > 0;
                    }
                }
                section = Section::None;
            }
            else if (field->key == "conformance_evidence")
            {
                profile.conformance_evidence = field->value;
                section                      = Section::None;
            }
            else if (field->key == "git_executable")
            {
                profile.git_executable = field->value;
                section                = Section::None;
            }
            else if (field->key == "git_min_version")
            {
                // "2.55" -> (2 << 16) | 55; a plain integer is also accepted.
                const usize dot = field->value.find('.');
                if (dot == std::string_view::npos)
                {
                    const auto version = parse_u64(field->value);
                    ok                 = version.has_value();
                    if (ok)
                    {
                        profile.git_min_version_major_minor = *version << 16;
                    }
                }
                else
                {
                    const auto major = parse_u64(field->value.substr(0, dot));
                    const auto minor = parse_u64(field->value.substr(dot + 1));
                    ok               = major.has_value() && minor.has_value();
                    if (ok)
                    {
                        profile.git_min_version_major_minor = (*major << 16) | *minor;
                    }
                }
                section = Section::None;
            }
            else if (field->key == "governed_paths")
            {
                ok      = empty_value;
                section = Section::GovernedPaths;
            }
            else if (field->key == "actors")
            {
                ok      = empty_value;
                section = Section::Actors;
            }
            else if (field->key == "capabilities")
            {
                ok      = empty_value;
                section = Section::Capabilities;
            }
            else if (field->key == "mediation")
            {
                ok      = empty_value;
                section = Section::Mediation;
            }
            else if (field->key == "claims")
            {
                ok      = empty_value;
                section = Section::Claims;
            }
            else if (field->key == "cognition")
            {
                ok      = empty_value;
                section = Section::Cognition;
            }
            else if (field->key == "tool_inventory")
            {
                ok      = empty_value;
                section = Section::ToolInventory;
            }
            else if (field->key == "record_launcher")
            {
                profile.record_launcher = field->value;
                section                 = Section::None;
            }
            else
            {
                fail_line();
            }
            continue;
        }

        // Indented content.
        usize indent = 0;
        while (indent < line.size() && line[indent] == ' ')
        {
            ++indent;
        }
        if (indent > 6)
        {
            fail_line();
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

        switch (section)
        {
        case Section::GovernedPaths:
            if (!is_list_row || body.empty() || body.find(' ') != std::string_view::npos)
            {
                fail_line();
                break;
            }
            profile.governed_paths.emplace_back(body);
            break;

        case Section::Actors:
        {
            if (is_list_row)
            {
                actors.emplace_back();
            }
            else if (actors.empty())
            {
                fail_line();
                break;
            }
            if (!field)
            {
                fail_line();
                break;
            }
            const auto value = parse_u64(field->value);
            if (!value)
            {
                fail_line();
                break;
            }
            if (field->key == "adapter")
            {
                actors.back().adapter.fnv = *value;
            }
            else if (field->key == "session_token")
            {
                actors.back().session_token = *value;
            }
            else if (field->key == "credential_token")
            {
                actors.back().credential_token = *value;
            }
            else
            {
                fail_line();
            }
            break;
        }

        case Section::Capabilities:
        {
            if (is_list_row)
            {
                capabilities.emplace_back();
            }
            else if (capabilities.empty())
            {
                fail_line();
                break;
            }
            if (!field)
            {
                fail_line();
                break;
            }
            auto& capability = capabilities.back();
            if (field->key == "id" || field->key == "adapter")
            {
                const auto value = parse_u64(field->value);
                if (!value)
                {
                    fail_line();
                    break;
                }
                if (field->key == "id")
                {
                    capability.id.value = *value;
                }
                else
                {
                    capability.adapter.fnv = *value;
                }
            }
            else if (field->key == "operation_class")
            {
                const auto value = parse_operation_class(field->value);
                if (!value)
                {
                    fail_line();
                    break;
                }
                capability.operation_class = *value;
            }
            else if (field->key == "can_mutate_world" || field->key == "can_publish_claims" ||
                     field->key == "requires_execution_authority")
            {
                const auto value = parse_bool(field->value);
                if (!value)
                {
                    fail_line();
                    break;
                }
                if (field->key == "can_mutate_world")
                {
                    capability.can_mutate_world = *value;
                }
                else if (field->key == "can_publish_claims")
                {
                    capability.can_publish_claims = *value;
                }
                else
                {
                    capability.requires_execution_authority = *value;
                }
            }
            else
            {
                fail_line();
            }
            break;
        }

        case Section::Mediation:
        {
            if (is_list_row)
            {
                mediation.emplace_back();
            }
            else if (mediation.empty())
            {
                fail_line();
                break;
            }
            if (!field)
            {
                fail_line();
                break;
            }
            if (field->key == "capability")
            {
                const auto value = parse_u64(field->value);
                if (!value)
                {
                    fail_line();
                    break;
                }
                mediation.back().capability.value = *value;
            }
            else if (field->key == "kind")
            {
                const auto value = parse_mediation_kind(field->value);
                if (!value)
                {
                    fail_line();
                    break;
                }
                mediation.back().kind = *value;
            }
            else
            {
                fail_line();
            }
            break;
        }

        case Section::Claims:
        {
            if (is_list_row || !field)
            {
                fail_line();
                break;
            }
            const auto value = parse_bool(field->value);
            if (!value)
            {
                fail_line();
                break;
            }
            if (field->key == "tool_mediated")
            {
                claims.tool_mediated = *value;
            }
            else if (field->key == "free_text")
            {
                claims.free_text = *value;
            }
            else
            {
                fail_line();
            }
            break;
        }

        case Section::Cognition:
        {
            if (is_list_row || !field)
            {
                fail_line();
                break;
            }
            if (field->key == "source_paths")
            {
                if (!field->value.empty())
                {
                    fail_line();
                    break;
                }
                section = Section::CognitionSources;
                break;
            }
            if (field->value.empty())
            {
                fail_line();
                break;
            }
            if (field->key == "repository")
            {
                profile.cognition.repository = field->value;
            }
            else if (field->key == "authorized_ref")
            {
                profile.cognition.authorized_ref = field->value;
            }
            else if (field->key == "policy_path")
            {
                profile.cognition.policy_path = field->value;
            }
            else if (field->key == "policy_sha256")
            {
                profile.cognition.policy_sha256 = field->value;
            }
            else
            {
                fail_line();
            }
            break;
        }

        case Section::CognitionSources:
        {
            if (!is_list_row || body.empty() || body.find(' ') != std::string_view::npos)
            {
                fail_line();
                break;
            }
            profile.cognition.source_paths.emplace_back(body);
            // A following indented "key: value" leaves the sub-list.
            if (body.find(':') != std::string_view::npos)
            {
                fail_line();
            }
            break;
        }

        case Section::ToolInventory:
        {
            if (is_list_row)
            {
                profile.tool_inventory.emplace_back();
            }
            else if (profile.tool_inventory.empty())
            {
                fail_line();
                break;
            }
            if (!field)
            {
                fail_line();
                break;
            }
            if (field->key == "tool")
            {
                if (field->value.empty())
                {
                    fail_line();
                    break;
                }
                profile.tool_inventory.back().tool = field->value;
            }
            else if (field->key == "capability")
            {
                const auto value = parse_u64(field->value);
                if (!value)
                {
                    fail_line();
                    break;
                }
                profile.tool_inventory.back().capability = *value;
            }
            else if (field->key == "extraction")
            {
                const auto& value = field->value;
                if (value != "command" && value != "file_path")
                {
                    fail_line();
                    break;
                }
                profile.tool_inventory.back().extraction = value;
            }
            else if (field->key == "detector")
            {
                const auto& value = field->value;
                if (value != "exact_path" && value != "conservative_text_reference")
                {
                    fail_line();
                    break;
                }
                profile.tool_inventory.back().detector = value;
            }
            else
            {
                fail_line();
            }
            break;
        }

        case Section::None:
            fail_line();
            break;
        }
    }

    if (!ok)
    {
        return ProfileFileResult::fail(malformed(
            "line is outside the bounded profile shape: '" + failing_line + "'",
            line_number));
    }
    if (!schema_seen)
    {
        return ProfileFileResult::fail(malformed("schema header is required"));
    }
    if (profile.profile_id.empty() || profile.revision == 0 || profile.control_version == 0)
    {
        return ProfileFileResult::fail(malformed("identity and revision fields are required"));
    }
    if (profile.conformance_evidence.empty())
    {
        return ProfileFileResult::fail(
            ProfileAcceptError { ProfileAcceptErrorKind::EvidenceMissing,
                                 "acceptance requires conformance evidence (ADL section 7)" });
    }
    if (profile.governed_paths.empty())
    {
        return ProfileFileResult::fail(
            ProfileAcceptError { ProfileAcceptErrorKind::ScopeAmbiguous,
                                 "governed scope must be an explicit non-empty enumeration" });
    }
    for (const auto& path : profile.governed_paths)
    {
        if (path.find('*') != std::string::npos || path.find("..") != std::string::npos ||
            path.empty() || path.front() == '/')
        {
            return ProfileFileResult::fail(
                ProfileAcceptError { ProfileAcceptErrorKind::ScopeAmbiguous,
                                     "governed paths must be explicit (no globs): " + path });
        }
    }
    if (profile.cognition.repository.empty() || profile.cognition.authorized_ref.empty() ||
        profile.cognition.policy_path.empty() || profile.cognition.policy_sha256.empty() ||
        profile.cognition.source_paths.empty())
    {
        return ProfileFileResult::fail(malformed("cognition delivery fields are required"));
    }
    if (profile.cognition.policy_sha256.size() != 64)
    {
        return ProfileFileResult::fail(malformed("policy_sha256 must be 64 hex characters"));
    }

    // Structural consistency: mediation entries reference declared
    // capabilities; every declared capability is accounted for.
    for (const auto& entry : mediation)
    {
        bool found = false;
        for (const auto& capability : capabilities)
        {
            if (capability.id.value == entry.capability.value)
            {
                found = true;
                break;
            }
        }
        if (!found)
        {
            return ProfileFileResult::fail(
                malformed("mediation references undeclared capability " +
                          std::to_string(entry.capability.value)));
        }
    }
    for (const auto& capability : capabilities)
    {
        bool found = false;
        for (const auto& entry : mediation)
        {
            if (entry.capability.value == capability.id.value)
            {
                found = true;
                break;
            }
        }
        if (!found)
        {
            return ProfileFileResult::fail(malformed("capability " +
                                                     std::to_string(capability.id.value) +
                                                     " has no mediation entry (gaps are explicit, not silent)"));
        }
    }

    // MVP-4 complete-mediation precondition (batch design section 3.5):
    // every ActionInterception-mediated FileSystemWrite capability has a
    // tool_inventory row, and every inventory row references a declared,
    // intercepted capability. An unclaimed mediation gap is explicit.
    if (!profile.tool_inventory.empty())
    {
        for (const auto& row : profile.tool_inventory)
        {
            const profile::MediationEntry* entry            = nullptr;
            const adapter::CapabilityDescriptor* capability = nullptr;
            for (const auto& candidate : mediation)
            {
                if (candidate.capability.value == row.capability)
                {
                    for (const auto& declared : capabilities)
                    {
                        if (declared.id.value == row.capability)
                        {
                            capability = &declared;
                            break;
                        }
                    }
                    entry = &candidate;
                    break;
                }
            }
            if (entry == nullptr || capability == nullptr)
            {
                return ProfileFileResult::fail(
                    malformed("tool_inventory references undeclared capability " +
                              std::to_string(row.capability)));
            }
            if (entry->kind != profile::MediationKind::ActionInterception ||
                capability->operation_class != adapter::OperationClass::FileSystemWrite)
            {
                return ProfileFileResult::fail(
                    malformed("tool_inventory capability " + std::to_string(row.capability) +
                              " must be an intercepted FileSystemWrite capability"));
            }
        }
        // Deliberately NO downward completeness check here (design §3.5
        // correction, found by profile_accept at batch time): capabilities
        // 1-3 are the mediated qiven-record path, not harness tool
        // surfaces — requiring an inventory row for them is not well
        // typed. Completeness over the HARNESS write surface is the
        // inventory itself plus the H1 real-tool proof; the record-path
        // capabilities are guarded by the typed request surface (MVP-5).
    }

    // The scope value (normalized) and the profile build.
    ResourceScope::Builder scope_builder;
    for (const auto& path : profile.governed_paths)
    {
        scope_builder.add_path(path);
    }
    auto scope = scope_builder.build();
    if (!scope.is_ok())
    {
        return ProfileFileResult::fail(
            ProfileAcceptError { ProfileAcceptErrorKind::ScopeAmbiguous,
                                 "governed scope normalization failed: " + scope.reason().message });
    }

    profile::GovernedActorSet actor_set;
    actor_set.revision = profile.revision;
    actor_set.actors   = std::move(actors);
    profile::CapabilityUniverse universe;
    universe.revision     = profile.revision;
    universe.capabilities = std::move(capabilities);
    profile::MediationInventory inventory;
    inventory.revision = profile.revision;
    inventory.entries  = std::move(mediation);

    profile::ProfileBuilder builder;
    builder.set_name(profile.profile_id)
        .set_revision(qiven::runtime::ProfileRevision { profile.revision })
        .set_actor_set(std::move(actor_set))
        .set_capability_universe(std::move(universe))
        .set_mediation_inventory(std::move(inventory))
        .set_claims(claims)
        .set_control_version(profile.control_version)
        .set_conformance_evidence(profile.conformance_evidence);
    auto built = builder.build();
    if (!built.is_ok())
    {
        return ProfileFileResult::fail(malformed("profile build failed: " + built.reason().message));
    }
    built.value().resolver_registry_revision   = profile.resolver_registry_revision;
    built.value().classifier_contract_revision = profile.classifier_contract_revision;
    profile.built                              = std::move(built.value());
    return ProfileFileResult(std::move(profile));
}

port::ProfileAcceptResult FileProfileSource::accept(const port::ProfileAcceptRequest& request) const
{
    auto loaded = load_profile_file(request.source);
    if (!loaded.is_ok())
    {
        return port::ProfileAcceptResult::fail(loaded.reason());
    }
    const ProfileFile& file = loaded.value();
    if (file.profile_id != request.profile_id)
    {
        return port::ProfileAcceptResult::fail(
            ProfileAcceptError { ProfileAcceptErrorKind::Malformed,
                                 "loaded profile identity '" + file.profile_id +
                                     "' is not the requested '" + request.profile_id + "'" });
    }
    if (request.expected_revision != 0 && file.revision != request.expected_revision)
    {
        return port::ProfileAcceptResult::fail(
            ProfileAcceptError { ProfileAcceptErrorKind::VersionMismatch,
                                 "profile revision " + std::to_string(file.revision) +
                                     " is not the expected " +
                                     std::to_string(request.expected_revision) });
    }
    if (request.conformance_evidence.empty() ||
        request.conformance_evidence != file.conformance_evidence)
    {
        return port::ProfileAcceptResult::fail(
            ProfileAcceptError { ProfileAcceptErrorKind::EvidenceMissing,
                                 "the acceptance run's evidence must be present and match the accepted instance" });
    }
    if (request.governed_scope.empty())
    {
        return port::ProfileAcceptResult::fail(
            ProfileAcceptError { ProfileAcceptErrorKind::ScopeAmbiguous,
                                 "the asserted governed scope is empty" });
    }
    // Both sides through the same normalization (ResourceScope sorts and
    // de-duplicates): file order is not semantic.
    ResourceScope::Builder file_scope_builder;
    for (const auto& path : file.governed_paths)
    {
        file_scope_builder.add_path(path);
    }
    auto file_scope = file_scope_builder.build();
    if (!file_scope.is_ok() ||
        request.governed_scope.governed_paths() != file_scope.value().governed_paths())
    {
        return port::ProfileAcceptResult::fail(
            ProfileAcceptError { ProfileAcceptErrorKind::ScopeAmbiguous,
                                 "the asserted governed scope does not match the accepted instance" });
    }
    return port::ProfileAcceptResult(file.built);
}
} // namespace qiven::runtime::host
