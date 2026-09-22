// ============================================================================
// apps/runtimectl_main.cpp — qiven-runtimectl, the read-only operational
// client (MVP-2 minimal surface; ARCH section 6.4)
//
// MVP-2 ships exactly two subcommands, both local and read-only:
//   qiven-runtimectl cognition show [--root <qiven-context checkout>]
//   qiven-runtimectl profile show  [--profile <profile file>]
//
// `status` and `doctor` arrive with MVP-3 (they need the host IPC
// endpoint). Exit codes follow the script law: 0 pass, 1 failure,
// 2 usage.
// ============================================================================

#include <qiven/runtime/cognition/bundle.hpp>
#include <qiven/runtime/host/deployment_profile.hpp>
#include <qiven/types.hpp>

#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace
{
constexpr int exit_ok    = 0;
constexpr int exit_fail  = 1;
constexpr int exit_usage = 2;

int usage()
{
    std::cerr << "usage: qiven-runtimectl cognition show [--root <qiven-context checkout>]\n"
              << "       qiven-runtimectl profile show [--profile <profile file>]\n";
    return exit_usage;
}

int cognition_show(const std::vector<std::string>& args)
{
    std::filesystem::path repo_root = std::filesystem::current_path();
    for (std::size_t i = 0; i + 1 < args.size(); ++i)
    {
        if (args[i] == "--root")
        {
            repo_root = args[i + 1];
        }
    }
    const qiven::runtime::cognition::BundleStore store(repo_root / ".qiven" / "runtime");
    auto bundle = store.load_active();
    if (!bundle.is_ok())
    {
        std::cout << "cognition: NO VERIFIED ACTIVE BUNDLE (" << bundle.reason().detail << ")\n";
        return exit_fail;
    }
    const auto& manifest = bundle.value().manifest;
    std::cout << "cognition: ACTIVE bundle verified\n"
              << "  schema           : " << manifest.schema << "\n"
              << "  repository       : " << manifest.repository << "\n"
              << "  source_revision  : " << manifest.source_revision << "  (git commit oid)\n"
              << "  source_tree      : " << manifest.source_tree << "  (git tree oid)\n"
              << "  view             : " << manifest.view << "\n"
              << "  snapshot_format  : " << manifest.snapshot_format << "\n"
              << "  snapshot_sha256  : " << qiven::runtime::to_hex(manifest.snapshot_digest)
              << "  (content identity)\n"
              << "  policy_sha256    : " << qiven::runtime::to_hex(manifest.policy_digest)
              << "  (content identity)\n"
              << "  source_files     : " << manifest.source_files.size() << "\n"
              << "  publisher_build  : " << manifest.publisher_build << "\n"
              << "  published_at     : " << manifest.published_at << "\n"
              << "  bundle_dir       : " << bundle.value().dir.string() << "\n";
    return exit_ok;
}

int profile_show(const std::vector<std::string>& args)
{
    std::filesystem::path file = std::filesystem::current_path() /
                                 "config" / "profiles" / "zcode-jason-context-record-mvp.yaml";
    for (std::size_t i = 0; i + 1 < args.size(); ++i)
    {
        if (args[i] == "--profile")
        {
            file = args[i + 1];
        }
    }
    auto profile = qiven::runtime::host::load_profile_file(file);
    if (!profile.is_ok())
    {
        std::cout << "profile: NOT ACCEPTABLE (" << profile.reason().detail << ")\n";
        return exit_fail;
    }
    const auto& value = profile.value();
    std::cout << "profile: accepted\n"
              << "  profile_id    : " << value.profile_id << "\n"
              << "  revision      : " << value.revision << "\n"
              << "  control       : " << value.control_version << "\n"
              << "  resolver_reg  : " << value.resolver_registry_revision << "\n"
              << "  classifier    : " << value.classifier_contract_revision << "\n"
              << "  evidence      : " << value.conformance_evidence << "\n"
              << "  governed paths (" << value.governed_paths.size() << "):\n";
    for (const auto& path : value.governed_paths)
    {
        std::cout << "    " << path << "\n";
    }
    std::cout << "  cognition repository : " << value.cognition.repository << "\n"
              << "  authorized ref       : " << value.cognition.authorized_ref << "\n"
              << "  policy path          : " << value.cognition.policy_path << "\n"
              << "  policy sha256        : " << value.cognition.policy_sha256 << "\n"
              << "  source paths (" << value.cognition.source_paths.size() << ")\n"
              << "  git executable       : " << value.git_executable.string() << "\n";
    return exit_ok;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        return usage();
    }
    const std::string object = argv[1];
    const std::string verb   = argv[2];
    const std::vector<std::string> args(argv + 3, argv + argc);
    if (object == "cognition" && verb == "show")
    {
        return cognition_show(args);
    }
    if (object == "profile" && verb == "show")
    {
        return profile_show(args);
    }
    return usage();
}
