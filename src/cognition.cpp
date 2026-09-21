#include <qiven/runtime/port/cognition_port.hpp>

#include <qiven/hashing_sha256.hpp>

#include <fstream>
#include <iterator>
#include <utility>

namespace qiven::runtime::port
{
namespace
{
PinError map_deserialize_error(const qiven::context::DeserializeError& error)
{
    using qiven::context::DeserializeError;
    PinErrorKind kind = PinErrorKind::Unknown;
    switch (error.kind)
    {
    case DeserializeError::Kind::Truncated: kind = PinErrorKind::Truncated; break;
    case DeserializeError::Kind::BadVersion: kind = PinErrorKind::BadVersion; break;
    case DeserializeError::Kind::BadEnum: kind = PinErrorKind::BadEnum; break;
    case DeserializeError::Kind::ResourceAbuse: kind = PinErrorKind::ResourceAbuse; break;
    case DeserializeError::Kind::DigestMismatch: kind = PinErrorKind::DigestMismatch; break;
    }
    return PinError { kind, error.detail };
}

std::string content_addressed_revision(const ContentDigest& digest)
{
    return "sha256:" + to_hex(digest);
}
} // namespace

PinResult DraftSnapshotReader::pin(const PinRequest& request) const
{
    std::error_code ec;
    if (!std::filesystem::exists(request.file, ec) || ec)
    {
        return PinResult::fail(PinError { PinErrorKind::FileUnavailable, request.file.string() });
    }

    // Read the exact bytes ONCE: the same buffer is the integrity preimage
    // and the deserialization input — the pinned digest can never describe
    // different bytes than were parsed.
    qiven::context::Bytes bytes;
    {
        std::ifstream input(request.file, std::ios::binary);
        if (!input)
        {
            return PinResult::fail(PinError { PinErrorKind::FileUnavailable, request.file.string() });
        }
        char chunk[8192];
        while (input.read(chunk, sizeof chunk) || input.gcount() > 0)
        {
            const auto count = static_cast<std::size_t>(input.gcount());
            bytes.insert(bytes.end(), reinterpret_cast<const std::byte*>(chunk),
                         reinterpret_cast<const std::byte*>(chunk) + count);
            if (!input)
            {
                break;
            }
        }
    }

    qiven::context::CognitionSource source;
    source.kind           = qiven::context::CognitionSourceKind::HandoffArtifact;
    source.inlineBytes    = bytes;
    source.expectedDigest = request.expected_content_id;

    qiven::context::DeserializeError error;
    qiven::context::CognitionHandle handle = qiven::context::QivenContext::create_cognition(source, &error);
    if (!handle || !handle->state)
    {
        // corruption fails closed through the typed channel; never an assert
        return PinResult::fail(map_deserialize_error(error));
    }

    PinnedCognition pinned;
    pinned.snapshot_digest        = handle->digest;
    pinned.snapshot_digest_sha256 = ContentDigest { sha256(bytes.data(), bytes.size()) };
    pinned.revision.value         = content_addressed_revision(pinned.snapshot_digest_sha256);

    // Canonical policy preimage: a default Snapshot carrying exactly the
    // pinned InvocationPolicy, serialized with the draft's serializer. A
    // deterministic function of the policy content alone.
    qiven::context::Snapshot policy_only {};
    policy_only.invocation                   = handle->state->invocation;
    const qiven::context::Bytes policy_bytes = qiven::context::serialize_snapshot(policy_only);
    pinned.policy_digest                     = ContentDigest { sha256(policy_bytes.data(), policy_bytes.size()) };

    pinned.epoch    = handle->epoch;
    pinned.snapshot = handle->state;
    return PinResult(std::move(pinned));
}
} // namespace qiven::runtime::port
