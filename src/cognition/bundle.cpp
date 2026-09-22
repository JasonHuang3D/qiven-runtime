#include <qiven/runtime/cognition/bundle.hpp>

#include <qiven/hashing_sha256.hpp>
#include <qiven/runtime/jsonx/json_codec.hpp>

#include <fstream>
#include <iterator>
#include <utility>

namespace qiven::runtime::cognition
{
namespace
{
using port::BundleError;
using port::BundleErrorKind;
using StoreBytes = qiven::Result<std::string, BundleError>;

BundleError unavailable(std::string detail)
{
    return BundleError { BundleErrorKind::Unavailable, std::move(detail) };
}

BundleError malformed(std::string detail)
{
    return BundleError { BundleErrorKind::MalformedManifest, std::move(detail) };
}

BundleError digest_mismatch(std::string detail)
{
    return BundleError { BundleErrorKind::DigestMismatch, std::move(detail) };
}

template <typename T>
qiven::Result<T, BundleError> fail_with(BundleError error)
{
    return qiven::Result<T, BundleError>::fail(std::move(error));
}

StoreBytes read_file_bytes(const std::filesystem::path& file, u64 cap)
{
    std::error_code ec;
    if (!std::filesystem::exists(file, ec) || ec)
    {
        return StoreBytes::fail(unavailable(file.string()));
    }
    const auto size = std::filesystem::file_size(file, ec);
    if (ec)
    {
        return StoreBytes::fail(unavailable(file.string()));
    }
    if (static_cast<u64>(size) > cap)
    {
        return StoreBytes::fail(malformed(file.string() + " exceeds its bounded size"));
    }
    std::ifstream input(file, std::ios::binary);
    if (!input)
    {
        return StoreBytes::fail(unavailable(file.string()));
    }
    std::string bytes(static_cast<usize>(size), '\0');
    if (size > 0)
    {
        input.read(bytes.data(), static_cast<std::streamsize>(size));
        if (!input)
        {
            return StoreBytes::fail(unavailable(file.string()));
        }
    }
    return StoreBytes(std::move(bytes));
}

int hex_nibble(char c) noexcept
{
    if (c >= '0' && c <= '9')
    {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f')
    {
        return c - 'a' + 10;
    }
    return -1;
}
} // namespace

ContentDigest digest_of(std::string_view bytes)
{
    ContentDigest digest;
    digest.sha256 = qiven::sha256(bytes);
    return digest;
}

ContentDigest digest_of_file(const std::filesystem::path& file)
{
    std::ifstream input(file, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(input)),
                      std::istreambuf_iterator<char> {});
    return digest_of(bytes);
}

std::string hex_lower(std::span<const std::byte> bytes)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const std::byte b : bytes)
    {
        const auto value = static_cast<unsigned char>(b);
        out.push_back(digits[value >> 4]);
        out.push_back(digits[value & 0x0F]);
    }
    return out;
}

qiven::Result<ContentDigest, BundleError> digest_from_hex(std::string_view hex)
{
    if (hex.size() != 64)
    {
        return fail_with<ContentDigest>(malformed("digest hex must be 64 characters"));
    }
    ContentDigest digest;
    for (usize i = 0; i < 32; ++i)
    {
        const int high = hex_nibble(hex[2 * i]);
        const int low  = hex_nibble(hex[2 * i + 1]);
        if (high < 0 || low < 0)
        {
            return fail_with<ContentDigest>(
                malformed("digest hex contains a non-hex character"));
        }
        digest.sha256[i] = static_cast<std::byte>((high << 4) | low);
    }
    return qiven::Result<ContentDigest, BundleError>(digest);
}

BundleStore::BundleStore(std::filesystem::path runtime_root) :
m_bundles_root(std::move(runtime_root) / "bundles")
{
}

qiven::Result<ContentDigest, BundleError> BundleStore::active_digest() const
{
    const std::filesystem::path pointer = m_bundles_root / "ACTIVE";
    auto bytes                          = read_file_bytes(pointer, 128);
    if (!bytes.is_ok())
    {
        return fail_with<ContentDigest>(bytes.reason());
    }
    std::string_view line = bytes.value();
    while (!line.empty() &&
           (line.back() == '\n' || line.back() == '\r' || line.back() == ' '))
    {
        line.remove_suffix(1);
    }
    auto digest = digest_from_hex(line);
    if (!digest.is_ok())
    {
        return fail_with<ContentDigest>(digest.reason());
    }
    return digest;
}

qiven::Result<VerifiedBundle, BundleError> BundleStore::load(
    const ContentDigest& bundle_digest) const
{
    using jsonx::JsonValue;
    using LoadResult = qiven::Result<VerifiedBundle, BundleError>;

    const std::filesystem::path dir = m_bundles_root / hex_lower(bundle_digest.sha256);
    auto manifest_bytes             = read_file_bytes(dir / "manifest.json", 1024 * 1024);
    if (!manifest_bytes.is_ok())
    {
        return LoadResult::fail(manifest_bytes.reason());
    }
    if (digest_of(manifest_bytes.value()) != bundle_digest)
    {
        return LoadResult::fail(
            digest_mismatch("directory name does not match its manifest digest"));
    }

    auto document_value = jsonx::parse(manifest_bytes.value());
    if (!document_value.is_ok())
    {
        return LoadResult::fail(malformed("manifest.json: " + document_value.reason().message));
    }
    const JsonValue& document = document_value.value();

    port::CognitionBundleManifest manifest;
    manifest.schema          = document.string_or("schema", "");
    manifest.repository      = document.string_or("repository", "");
    manifest.source_revision = document.string_or("source_revision", "");
    manifest.source_tree     = document.string_or("source_tree", "");
    manifest.view            = document.string_or("view", "");
    manifest.snapshot_format = static_cast<u32>(document.number_or("snapshot_format", 0));
    manifest.publisher_build = document.string_or("publisher_build", "");
    manifest.published_at    = document.string_or("published_at", "");
    if (manifest.schema != "qiven-cognition-bundle-v1")
    {
        return LoadResult::fail(malformed("unknown schema"));
    }
    if (manifest.snapshot_format != 8)
    {
        return LoadResult::fail(
            BundleError { BundleErrorKind::VersionUnsupported,
                          "snapshot format " + std::to_string(manifest.snapshot_format) +
                              " is not the frozen v8" });
    }
    if (manifest.repository.empty() || manifest.source_revision.empty() ||
        manifest.source_tree.empty() || manifest.view.empty() ||
        manifest.publisher_build.empty() || manifest.published_at.empty())
    {
        return LoadResult::fail(malformed("manifest is missing a required field"));
    }
    for (const char* hex_field : { "snapshot_sha256", "policy_sha256" })
    {
        const std::string hex = document.string_or(hex_field, "");
        auto digest           = digest_from_hex(hex);
        if (!digest.is_ok())
        {
            return LoadResult::fail(digest.reason());
        }
        if (std::string_view(hex_field) == "snapshot_sha256")
        {
            manifest.snapshot_digest = digest.value();
        }
        else
        {
            manifest.policy_digest = digest.value();
        }
    }

    const JsonValue* files = document.find("source_files");
    if (files == nullptr || files->kind != JsonValue::Kind::Array)
    {
        return LoadResult::fail(malformed("source_files array is required"));
    }
    for (const auto& entry : files->array)
    {
        if (entry.kind != JsonValue::Kind::Object)
        {
            return LoadResult::fail(malformed("source_files entries must be objects"));
        }
        const std::string path = entry.string_or("path", "");
        const std::string hex  = entry.string_or("sha256", "");
        auto digest            = digest_from_hex(hex);
        if (path.empty() || !digest.is_ok())
        {
            return LoadResult::fail(malformed("source_files entry is malformed"));
        }
        for (const auto& existing : manifest.source_files)
        {
            if (existing.path == path)
            {
                return LoadResult::fail(malformed("duplicate source_files path " + path));
            }
        }
        manifest.source_files.push_back({ path, digest.value() });
    }
    if (manifest.source_files.empty())
    {
        return LoadResult::fail(malformed("a bundle carries at least the policy source"));
    }

    // Total re-verification: EVERY manifest-named file re-hashed (exit
    // gate 2), then the snapshot and policy members.
    for (const auto& source : manifest.source_files)
    {
        if (source.path.find("..") != std::string::npos || source.path.front() == '/' ||
            source.path.find(':') != std::string::npos)
        {
            return LoadResult::fail(malformed("source path escapes the bundle: " + source.path));
        }
        const std::filesystem::path file =
            dir / "source" / std::filesystem::path(source.path).lexically_normal();
        if (digest_of_file(file) != source.digest)
        {
            return LoadResult::fail(digest_mismatch("source file changed: " + source.path));
        }
    }

    auto snapshot_bytes = read_file_bytes(dir / "snapshot.qvs", 16 * 1024 * 1024);
    if (!snapshot_bytes.is_ok())
    {
        return LoadResult::fail(snapshot_bytes.reason());
    }
    if (digest_of(snapshot_bytes.value()) != manifest.snapshot_digest)
    {
        return LoadResult::fail(digest_mismatch("snapshot.qvs changed"));
    }
    auto policy_bytes = read_file_bytes(dir / "policy.yaml", 1024 * 1024);
    if (!policy_bytes.is_ok())
    {
        return LoadResult::fail(policy_bytes.reason());
    }
    if (digest_of(policy_bytes.value()) != manifest.policy_digest)
    {
        return LoadResult::fail(digest_mismatch("policy.yaml changed"));
    }

    VerifiedBundle verified;
    verified.manifest       = std::move(manifest);
    verified.dir            = dir;
    verified.snapshot_bytes = std::move(snapshot_bytes.value());
    verified.policy_bytes   = std::move(policy_bytes.value());
    return LoadResult(std::move(verified));
}

qiven::Result<VerifiedBundle, BundleError> BundleStore::load_active() const
{
    auto digest = active_digest();
    if (!digest.is_ok())
    {
        return qiven::Result<VerifiedBundle, BundleError>::fail(digest.reason());
    }
    return load(digest.value());
}

port::BundlePin BundleStore::pin_from_bundle(const port::CognitionBundleManifest& manifest) const
{
    auto active = load_active();
    if (!active.is_ok())
    {
        return port::BundlePin::fail(active.reason());
    }
    const port::CognitionBundleManifest& disk = active.value().manifest;
    const bool same                           = disk.schema == manifest.schema && disk.repository == manifest.repository &&
                      disk.source_revision == manifest.source_revision &&
                      disk.source_tree == manifest.source_tree && disk.view == manifest.view &&
                      disk.snapshot_format == manifest.snapshot_format &&
                      disk.snapshot_digest == manifest.snapshot_digest &&
                      disk.policy_digest == manifest.policy_digest &&
                      disk.publisher_build == manifest.publisher_build &&
                      disk.published_at == manifest.published_at &&
                      disk.source_files.size() == manifest.source_files.size();
    if (same)
    {
        for (usize i = 0; i < disk.source_files.size(); ++i)
        {
            if (disk.source_files[i].path != manifest.source_files[i].path ||
                disk.source_files[i].digest != manifest.source_files[i].digest)
            {
                return port::BundlePin::fail(
                    unavailable("the ACTIVE bundle is not the requested manifest"));
            }
        }
    }
    else
    {
        return port::BundlePin::fail(unavailable("the ACTIVE bundle is not the requested manifest"));
    }

    // Pin the verified snapshot through the frozen draft reader. The bytes
    // are digest-proven against the manifest already; the reader owns
    // parsing and produces the draft-side identities.
    port::DraftSnapshotReader reader;
    port::PinRequest request;
    request.file = active.value().dir / "snapshot.qvs";
    auto pinned  = reader.pin(request);
    if (!pinned.is_ok())
    {
        return port::BundlePin::fail(
            malformed("snapshot failed draft materialization: " + pinned.reason().detail));
    }
    if (pinned.value().snapshot_digest_sha256 != disk.snapshot_digest)
    {
        return port::BundlePin::fail(
            digest_mismatch("pinned snapshot digest disagrees with the manifest"));
    }
    pinned.value().source_revision = disk.source_revision;
    return port::BundlePin(port::BundlePinResult { std::move(pinned.value()), disk });
}
} // namespace qiven::runtime::cognition
