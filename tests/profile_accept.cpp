// ============================================================================
// profile_accept — file-backed DeploymentProfile acceptance rules
// (MVP-2 batch design section 6): fail-closed acceptance classes plus a
// load of the SHIPPED accepted instance.
// ============================================================================

#include <qiven/contracts.hpp>
#include <qiven/runtime/host/deployment_profile.hpp>
#include <qiven/runtime/scope.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace
{
using qiven::runtime::host::load_profile_file;
using qiven::runtime::port::ProfileAcceptErrorKind;

const std::string valid_profile =
    "schema: qiven-deployment-profile-v1\n"
    "profile_id: test-profile\n"
    "revision: 7\n"
    "control_version: 2\n"
    "resolver_registry_revision: 1\n"
    "classifier_contract_revision: 1\n"
    "conformance_evidence: evidence/audits/test-run.md\n"
    "freshness_window_ms: 604800000\n"
    "governed_paths:\n"
    "  - state/active-work.yaml\n"
    "  - memory/records\n"
    "actors:\n"
    "  - adapter: 13644155810937356244\n"
    "    session_token: 1\n"
    "    credential_token: 1\n"
    "capabilities:\n"
    "  - id: 1\n"
    "    adapter: 13644155810937356244\n"
    "    operation_class: FileSystemWrite\n"
    "    can_mutate_world: true\n"
    "    can_publish_claims: false\n"
    "    requires_execution_authority: true\n"
    "mediation:\n"
    "  - capability: 1\n"
    "    kind: ActionInterception\n"
    "claims:\n"
    "  tool_mediated: true\n"
    "  free_text: false\n"
    "cognition:\n"
    "  repository: https://example.invalid/qiven-context.git\n"
    "  authorized_ref: refs/heads/main\n"
    "  policy_path: runtime/invocation-policy.yaml\n"
    "  policy_sha256: 0000000000000000000000000000000000000000000000000000000000000000\n"
    "  source_paths:\n"
    "    - runtime/invocation-policy.yaml\n"
    "git_executable: C:/Program Files/Git/cmd/git.exe\n"
    "git_min_version: 2.40\n";

std::filesystem::path write_fixture(const std::filesystem::path& dir, const std::string& name,
                                    const std::string& content)
{
    std::filesystem::create_directories(dir);
    const std::filesystem::path file = dir / name;
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << content;
    return file;
}
} // namespace

int main()
{
    const std::filesystem::path workroot = QIVEN_RUNTIME_TEST_WORKROOT;
    const std::filesystem::path dir      = workroot / "profile-accept";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    // Happy path: loads, builds through ProfileBuilder, carries the
    // cognition-delivery facts.
    {
        auto file   = write_fixture(dir, "valid.yaml", valid_profile);
        auto loaded = load_profile_file(file);
        QIVEN_VERIFY(loaded.is_ok());
        QIVEN_VERIFY(loaded.value().profile_id == "test-profile");
        QIVEN_VERIFY(loaded.value().revision == 7);
        QIVEN_VERIFY(loaded.value().governed_paths.size() == 2);
        QIVEN_VERIFY(loaded.value().cognition.policy_path == "runtime/invocation-policy.yaml");
        QIVEN_VERIFY(loaded.value().built.name == "test-profile");
        QIVEN_VERIFY(loaded.value().built.actors.actors.size() == 1);
        QIVEN_VERIFY(loaded.value().built.mediation.entries.size() == 1);
        QIVEN_VERIFY(loaded.value().built.resolver_registry_revision == 1);
    }

    // Fail-closed classes. Every case replaces an exact-length span; the
    // helper refuses a missing pattern instead of silently mangling.
    {
        const auto swap = [](std::string text, const std::string& from, const std::string& to) {
            const auto pos = text.find(from);
            QIVEN_VERIFY(pos != std::string::npos);
            return text.replace(pos, from.size(), to);
        };
        auto reject = [&](const std::string& content, ProfileAcceptErrorKind kind) {
            const auto file = write_fixture(dir, "case.yaml", content);
            auto loaded     = load_profile_file(file);
            return !loaded.is_ok() && loaded.reason().kind == kind;
        };
        // Globbed governed path -> ScopeAmbiguous.
        QIVEN_VERIFY(reject(swap(valid_profile, "  - memory/records", "  - memory/**"),
                            ProfileAcceptErrorKind::ScopeAmbiguous));
        // Empty governed scope -> ScopeAmbiguous.
        QIVEN_VERIFY(reject(swap(valid_profile, "  - state/active-work.yaml\n  - memory/records", ""),
                            ProfileAcceptErrorKind::ScopeAmbiguous));
        // Missing evidence -> EvidenceMissing.
        QIVEN_VERIFY(reject(swap(valid_profile, "conformance_evidence: evidence/audits/test-run.md",
                                 "conformance_evidence:"),
                            ProfileAcceptErrorKind::EvidenceMissing));
        // Mediation referencing an undeclared capability -> Malformed.
        QIVEN_VERIFY(reject(swap(valid_profile, "  - capability: 1", "  - capability: 99"),
                            ProfileAcceptErrorKind::Malformed));
        // Unknown top-level key -> Malformed.
        QIVEN_VERIFY(reject(valid_profile + std::string("rogue: 1\n"),
                            ProfileAcceptErrorKind::Malformed));
        // Unknown enum -> Malformed.
        QIVEN_VERIFY(reject(swap(valid_profile, "operation_class: FileSystemWrite",
                                 "operation_class: MemoryDeletion"),
                            ProfileAcceptErrorKind::Malformed));
        // Malformed version -> Malformed.
        QIVEN_VERIFY(reject(swap(valid_profile, "git_min_version: 2.40",
                                 "git_min_version: two"),
                            ProfileAcceptErrorKind::Malformed));
    }

    // accept(): the caller-assertion cross-checks.
    {
        using qiven::runtime::host::FileProfileSource;
        const auto file = write_fixture(dir, "accept.yaml", valid_profile);
        FileProfileSource source;

        qiven::runtime::ResourceScope::Builder builder;
        builder.add_path("state/active-work.yaml").add_path("memory/records");
        auto scope = builder.build();
        QIVEN_VERIFY(scope.is_ok());

        qiven::runtime::port::ProfileAcceptRequest request;
        request.profile_id           = "test-profile";
        request.source               = file.string();
        request.governed_scope       = scope.value();
        request.conformance_evidence = "evidence/audits/test-run.md";

        auto accepted = source.accept(request);
        QIVEN_VERIFY(accepted.is_ok());
        QIVEN_VERIFY(accepted.value().name == "test-profile");

        // Wrong identity denies.
        auto wrong_id       = request;
        wrong_id.profile_id = "other-profile";
        QIVEN_VERIFY(!source.accept(wrong_id).is_ok());

        // Revision mismatch denies (VersionMismatch).
        auto wrong_revision              = request;
        wrong_revision.expected_revision = 6;
        auto mismatched                  = source.accept(wrong_revision);
        QIVEN_VERIFY(!mismatched.is_ok());
        QIVEN_VERIFY(mismatched.reason().kind == ProfileAcceptErrorKind::VersionMismatch);

        // Scope mismatch denies (ScopeAmbiguous).
        qiven::runtime::ResourceScope::Builder other;
        other.add_path("state/active-work.yaml");
        auto other_scope = other.build();
        QIVEN_VERIFY(other_scope.is_ok());
        auto wrong_scope           = request;
        wrong_scope.governed_scope = other_scope.value();
        auto scope_denied          = source.accept(wrong_scope);
        QIVEN_VERIFY(!scope_denied.is_ok());
        QIVEN_VERIFY(scope_denied.reason().kind == ProfileAcceptErrorKind::ScopeAmbiguous);

        // Evidence disagreement denies (EvidenceMissing).
        auto wrong_evidence                 = request;
        wrong_evidence.conformance_evidence = "evidence/audits/other-run.md";
        auto evidence_denied                = source.accept(wrong_evidence);
        QIVEN_VERIFY(!evidence_denied.is_ok());
        QIVEN_VERIFY(evidence_denied.reason().kind == ProfileAcceptErrorKind::EvidenceMissing);
    }

    // The SHIPPED accepted instance loads clean (config/profiles/).
    {
        // workroot = <repo>/.generated-temp/runtime/tests/<config> -> the
        // repository root is four levels up.
        std::filesystem::path repo = workroot;
        for (int i = 0; i < 4; ++i)
        {
            repo = repo.parent_path();
        }
        const auto shipped = repo / "config" / "profiles" / "zcode-jason-context-record-mvp.yaml";
        auto loaded        = load_profile_file(shipped);
        QIVEN_VERIFY(loaded.is_ok());
        QIVEN_VERIFY(loaded.value().profile_id == "zcode-jason-context-record-mvp");
        QIVEN_VERIFY(loaded.value().cognition.policy_path == "runtime/invocation-policy.yaml");
        QIVEN_VERIFY(loaded.value().git_executable.string().find("git.exe") !=
                     std::string::npos);
        // Day-one honesty: raw ZCode tool classes stay Unmediated.
        bool saw_unmediated = false;
        for (const auto& entry : loaded.value().built.mediation.entries)
        {
            if (entry.kind == qiven::runtime::profile::MediationKind::Unmediated)
            {
                saw_unmediated = true;
            }
        }
        QIVEN_VERIFY(saw_unmediated);
    }

    std::printf("[ OK ] profile-accept\n");
    return 0;
}
