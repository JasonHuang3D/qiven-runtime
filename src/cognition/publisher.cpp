#include <qiven/runtime/cognition/publisher.hpp>

#include <qiven/runtime/cognition/bundle.hpp>
#include <qiven/runtime/jsonx/json_codec.hpp>

#include <qiven/context/persistence.hpp>

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>

namespace qiven::runtime::cognition
{
namespace
{
using port::BundleError;
using port::BundleErrorKind;
using PublishOutcome = qiven::Result<PublishResult, BundleError>;

BundleError unverifiable(std::string detail)
{
    return BundleError { BundleErrorKind::SourceUnverifiable, std::move(detail) };
}

BundleError malformed(std::string detail)
{
    return BundleError { BundleErrorKind::MalformedManifest, std::move(detail) };
}

std::string rfc3339_utc(u64 now_ms)
{
    const std::time_t seconds = static_cast<std::time_t>(now_ms / 1000);
    std::tm utc {};
    gmtime_s(&utc, &seconds);
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02dZ", utc.tm_year + 1900,
                  utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec);
    return buffer;
}

// One bounded git invocation. Every failure mode maps to SourceUnverifiable:
// the exact source could not be read, and the publisher must not guess.
qiven::Result<std::string, BundleError> git(const PublishRequest& request,
                                            const std::vector<std::string>& args)
{
    processx::ProcessSpec spec;
    spec.executable = request.git_executable;
    spec.argv       = { "git", "-C", request.repo_root.string() };
    spec.argv.insert(spec.argv.end(), args.begin(), args.end());
    spec.working_dir = request.repo_root;
    spec.deadline_ms = 60'000; // plumbing on a bounded tree; generous ceiling

    auto run = request.runner->run(spec);
    if (!run.is_ok())
    {
        return qiven::Result<std::string, BundleError>::fail(
            unverifiable("git " + args.front() + ": " + run.reason().message));
    }
    if (run.value().end == processx::ProcessRun::End::TimedOut)
    {
        return qiven::Result<std::string, BundleError>::fail(
            unverifiable("git " + args.front() + " exceeded its deadline"));
    }
    if (run.value().exit_code != 0)
    {
        std::string tail = run.value().err.substr(run.value().err.size() > 400
                                                      ? run.value().err.size() - 400
                                                      : 0);
        return qiven::Result<std::string, BundleError>::fail(
            unverifiable("git " + args.front() + " exited " +
                         std::to_string(run.value().exit_code) + ": " + tail));
    }
    return qiven::Result<std::string, BundleError>(std::move(run.value().out));
}

std::string trim(std::string value)
{
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r' ||
                              value.back() == ' ' || value.back() == '\t'))
    {
        value.pop_back();
    }
    usize begin = 0;
    while (begin < value.size() &&
           (value[begin] == '\n' || value[begin] == '\r' || value[begin] == ' '))
    {
        ++begin;
    }
    return value.substr(begin);
}

bool is_hex_oid(std::string_view text)
{
    if (text.size() != 40)
    {
        return false;
    }
    return text.find_first_not_of("0123456789abcdef") == std::string_view::npos;
}

// Does `path` fall under the declared source selection? A declared file
// matches exactly; a declared directory matches by path COMPONENTS
// (never string prefix — "state/current.md.md" is not under
// "state/current.md").
bool selected_by(const std::string& path, const std::vector<std::string>& declared)
{
    for (const auto& entry : declared)
    {
        if (path == entry)
        {
            return true;
        }
        if (path.size() > entry.size() && path.compare(0, entry.size(), entry) == 0 &&
            path[entry.size()] == '/')
        {
            return true;
        }
    }
    return false;
}

void write_file_bytes(const std::filesystem::path& file, std::string_view bytes)
{
    std::filesystem::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// Single-filesystem atomic replace (rename over an existing file).
bool replace_file(const std::filesystem::path& from, const std::filesystem::path& to) noexcept
{
    const std::wstring wide_from = from.wstring();
    const std::wstring wide_to   = to.wstring();
    return MoveFileExW(wide_from.c_str(), wide_to.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
}
} // namespace

PublishOutcome CanonicalCognitionPublisher::publish(const PublishRequest& request) const
{
    if (request.runner == nullptr || request.git_executable.empty() ||
        request.repo_root.empty() || request.ref.empty() || request.policy_path.empty() ||
        request.source_paths.empty() || request.publisher_build.empty())
    {
        return PublishOutcome::fail(malformed("publisher request is missing a required field"));
    }

    // Exact source identity: commit oid and tree oid (revision identity,
    // kept DISTINCT from every content digest — ARCH invariant 3).
    auto commit = git(request, { "rev-parse", request.ref + "^{commit}" });
    if (!commit.is_ok())
    {
        return PublishOutcome::fail(commit.reason());
    }
    const std::string commit_oid = trim(commit.value());
    if (!is_hex_oid(commit_oid))
    {
        return PublishOutcome::fail(unverifiable("ref did not resolve to a commit oid"));
    }
    auto tree = git(request, { "rev-parse", commit_oid + "^{tree}" });
    if (!tree.is_ok())
    {
        return PublishOutcome::fail(tree.reason());
    }
    const std::string tree_oid = trim(tree.value());
    if (!is_hex_oid(tree_oid))
    {
        return PublishOutcome::fail(unverifiable("commit did not resolve to a tree oid"));
    }

    // Exact tree membership: every path, from the tree object only.
    auto listing = git(request, { "ls-tree", "-r", "--name-only", tree_oid });
    if (!listing.is_ok())
    {
        return PublishOutcome::fail(listing.reason());
    }
    std::vector<std::string> selected;
    {
        std::string_view remaining = listing.value();
        while (!remaining.empty())
        {
            usize end = remaining.find('\n');
            const std::string_view line =
                end == std::string_view::npos ? remaining : remaining.substr(0, end);
            if (end == std::string_view::npos)
            {
                remaining = {};
            }
            else
            {
                remaining = remaining.substr(end + 1);
            }
            if (line.empty())
            {
                continue;
            }
            const std::string path(line);
            if (selected_by(path, request.source_paths))
            {
                selected.push_back(path);
            }
        }
    }

    // The policy file is a bundle member in its own right and MUST exist
    // at the commit (ARCH section 7.3: the machine instance is required).
    bool policy_selected = false;
    for (const auto& path : selected)
    {
        if (path == request.policy_path)
        {
            policy_selected = true;
            break;
        }
    }
    if (!policy_selected)
    {
        return PublishOutcome::fail(
            malformed("policy path " + request.policy_path + " is absent from the commit"));
    }
    if (selected.size() > max_source_files)
    {
        return PublishOutcome::fail(unverifiable("source selection exceeds the bounded file count"));
    }

    // Read every member blob from the exact commit (never the worktree).
    u64 total_bytes = 0;
    std::vector<std::pair<std::string, std::string>> members;
    members.reserve(selected.size());
    for (const auto& path : selected)
    {
        auto blob = git(request, { "cat-file", "blob", commit_oid + ":" + path });
        if (!blob.is_ok())
        {
            return PublishOutcome::fail(blob.reason());
        }
        if (blob.value().size() > max_source_file_bytes)
        {
            return PublishOutcome::fail(unverifiable("source file exceeds its bound: " + path));
        }
        total_bytes += blob.value().size();
        if (total_bytes > max_source_total_bytes)
        {
            return PublishOutcome::fail(unverifiable("source selection exceeds the total bound"));
        }
        members.emplace_back(path, std::move(blob.value()));
    }

    // The policy bytes must parse under the strict subset (fail closed:
    // a policy the runtime cannot read is not loadable, whatever its bytes).
    auto policy_bytes = [&]() -> std::string* {
        for (auto& [path, bytes] : members)
        {
            if (path == request.policy_path)
            {
                return &bytes;
            }
        }
        return nullptr;
    }();
    auto parsed_policy = parse_invocation_policy(*policy_bytes);
    if (!parsed_policy.is_ok())
    {
        return PublishOutcome::fail(malformed("invocation policy failed strict parsing at line " +
                                              std::to_string(parsed_policy.reason().line) + ": " +
                                              parsed_policy.reason().detail));
    }

    // Policy-only v8 snapshot: the frozen serializer owns the bytes; every
    // record vector stays EMPTY (exit gate 5 — rich records are NOT
    // lossy-converted; they ride as the byte-exact source files above).
    qiven::context::Snapshot snapshot;
    snapshot.invocation                      = parsed_policy.value().invocation;
    const qiven::context::Bytes snapshot_tlv = qiven::context::serialize_snapshot(snapshot);
    const std::string snapshot_bytes(
        reinterpret_cast<const char*>(snapshot_tlv.data()), snapshot_tlv.size());

    // Manifest (fixed writer field order; bundle identity = its digest).
    port::CognitionBundleManifest manifest;
    manifest.repository      = request.repository_url;
    manifest.source_revision = commit_oid;
    manifest.source_tree     = tree_oid;
    manifest.view            = request.view;
    manifest.snapshot_digest = digest_of(snapshot_bytes);
    manifest.policy_digest   = digest_of(*policy_bytes);
    manifest.publisher_build = request.publisher_build;
    manifest.published_at    = rfc3339_utc(request.now_ms);
    for (auto& [path, bytes] : members)
    {
        manifest.source_files.push_back({ path, digest_of(bytes) });
    }

    namespace jsonx = qiven::runtime::jsonx;
    jsonx::JsonObject document;
    document.emplace_back("schema", jsonx::JsonValue::make_string(manifest.schema));
    document.emplace_back("repository", jsonx::JsonValue::make_string(manifest.repository));
    document.emplace_back("source_revision",
                          jsonx::JsonValue::make_string(manifest.source_revision));
    document.emplace_back("source_tree", jsonx::JsonValue::make_string(manifest.source_tree));
    document.emplace_back("view", jsonx::JsonValue::make_string(manifest.view));
    document.emplace_back("snapshot_format",
                          jsonx::JsonValue::make_number(manifest.snapshot_format));
    document.emplace_back("snapshot_sha256",
                          jsonx::JsonValue::make_string(to_hex(manifest.snapshot_digest)));
    document.emplace_back("policy_sha256",
                          jsonx::JsonValue::make_string(to_hex(manifest.policy_digest)));
    {
        std::vector<jsonx::JsonValue> files;
        for (const auto& source : manifest.source_files)
        {
            files.push_back(jsonx::JsonValue::make_object(
                { { "path", jsonx::JsonValue::make_string(source.path) },
                  { "sha256", jsonx::JsonValue::make_string(to_hex(source.digest)) } }));
        }
        document.emplace_back("source_files", jsonx::JsonValue::make_array(std::move(files)));
    }
    document.emplace_back("publisher_build",
                          jsonx::JsonValue::make_string(manifest.publisher_build));
    document.emplace_back("published_at", jsonx::JsonValue::make_string(manifest.published_at));
    const std::string manifest_bytes = jsonx::write(jsonx::JsonValue::make_object(std::move(document)));

    const ContentDigest bundle_digest   = digest_of(manifest_bytes);
    const std::filesystem::path bundles = request.runtime_root / "bundles";
    std::filesystem::create_directories(bundles);

    // Sweep crash-left staging directories (regenerable class), then stage.
    for (const auto& entry : std::filesystem::directory_iterator(bundles))
    {
        const std::string name = entry.path().filename().string();
        if (name.rfind(".staging-", 0) == 0 || name.rfind(".active-", 0) == 0)
        {
            if (entry.is_directory())
            {
                std::filesystem::remove_all(entry.path());
            }
            else
            {
                std::filesystem::remove(entry.path());
            }
        }
    }

    const std::filesystem::path staging =
        bundles / (".staging-" + std::to_string(GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::remove_all(staging, ec);
    std::filesystem::create_directories(staging / "source", ec);

    for (const auto& [path, bytes] : members)
    {
        write_file_bytes(staging / "source" / std::filesystem::path(path), bytes);
    }
    write_file_bytes(staging / "snapshot.qvs", snapshot_bytes);
    write_file_bytes(staging / "policy.yaml", *policy_bytes);
    write_file_bytes(staging / "manifest.json", manifest_bytes);

    const std::filesystem::path final_dir = bundles / hex_lower(bundle_digest.sha256);
    if (std::filesystem::exists(final_dir, ec))
    {
        // Identical manifest bytes already published: bundles are
        // immutable, reuse the existing directory.
        std::filesystem::remove_all(staging, ec);
    }
    else
    {
        std::filesystem::rename(staging, final_dir, ec);
        if (ec)
        {
            std::filesystem::remove_all(staging, ec);
            return PublishOutcome::fail(
                unverifiable("bundle directory publication failed: " + ec.message()));
        }
    }

    // ACTIVE pointer swap: temp file + atomic replace (ARCH section 7.2).
    const std::filesystem::path active_temp =
        bundles / (".active-" + std::to_string(GetCurrentProcessId()));
    write_file_bytes(active_temp, hex_lower(bundle_digest.sha256) + "\n");
    if (!replace_file(active_temp, bundles / "ACTIVE"))
    {
        return PublishOutcome::fail(
            unverifiable("ACTIVE pointer swap failed (bundle directory remains valid)"));
    }

    PublishResult result;
    result.bundle_dir    = final_dir;
    result.bundle_digest = bundle_digest;
    result.manifest      = std::move(manifest);
    return PublishOutcome(std::move(result));
}
} // namespace qiven::runtime::cognition
