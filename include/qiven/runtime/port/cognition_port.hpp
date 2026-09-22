#pragma once

// ============================================================================
// port/cognition_port.hpp — pin one exact cognition revision (component ADL
// §23, design §7; obligation C-20 canonical-upstream direction)
//
// The canonical read side exposes ONE authoritative capability: pin an exact
// revision for a control transaction. A pin returns an immutable
// PinnedCognition — value snapshot, integrity identities, and the fencing
// lineage of the materialization — that a transaction binds exactly.
// Deriving fresh cognition (reading a NEWER revision) invalidates
// unconsumed decisions at revision level (ADL §41); the staleness
// enforcement itself lands with DecisionBinding (RCA-7), where revisions
// are compared.
//
// Day-one shape notes (recorded deviations from the design sketch, not
// silent ones):
//   - revision is content-addressed ("sha256:<hex>" of the pinned bytes):
//     the draft's storage RevisionId exists only under a real store; a
//     content-addressed identity is stable, comparable and honest today.
//   - the pin OWNS the snapshot (shared_ptr kept alive); generation-owned
//     non-owning views arrive with RuntimeHost composition.
//   - policy_digest is SHA-256 over a canonical policy preimage (a
//     default-valued Snapshot carrying exactly the pinned InvocationPolicy,
//     serialized with the draft's canonical serializer): a deterministic
//     function of the policy alone.
// ============================================================================

#include <qiven/result.hpp>
#include <qiven/runtime/identity.hpp>

#include <qiven/context/cognition.hpp>
#include <qiven/context/persistence.hpp>

#include <filesystem>
#include <memory>
#include <string>

namespace qiven::runtime::port
{
enum class PinErrorKind : u8
{
    FileUnavailable,
    EmptySource,
    Truncated,
    BadVersion,
    BadEnum,
    ResourceAbuse,
    DigestMismatch,
    Unknown,
};

struct PinError
{
    PinErrorKind kind = PinErrorKind::Unknown;
    std::string detail;
};

struct PinnedCognition
{
    qiven::context::RevisionId revision; // content-addressed day-one
    // REVISION identity, distinct from content identity (production-MVP
    // §4 invariant 3): the source revision this cognition was constructed
    // from (e.g. the bundle's git commit oid). Empty day-one for direct
    // file pins, where revision and content identity coincide.
    std::string source_revision;
    qiven::context::SnapshotDigest snapshot_digest; // draft identity of the pinned bytes
    ContentDigest snapshot_digest_sha256;           // integrity-grade identity of the bytes
    ContentDigest policy_digest;                    // canonical policy preimage digest
    qiven::context::Epoch epoch { 0 };              // materialization fencing lineage

    std::shared_ptr<const qiven::context::Snapshot> snapshot; // immutable, pin-owned

    // Revision-level staleness primitive: two pins of the same exact
    // content carry the same revision identity.
    [[nodiscard]] bool same_revision(const PinnedCognition& other) const noexcept
    {
        return revision.value == other.revision.value && source_revision == other.source_revision;
    }
};

using PinResult = qiven::Result<PinnedCognition, PinError>;

struct PinRequest
{
    std::filesystem::path file;
    // Content identity precondition (draft content id); empty = no
    // precondition. When set, a mismatch is corruption, not a warning.
    std::string expected_content_id;
};

class CanonicalCognitionPort
{
public:
    virtual ~CanonicalCognitionPort() = default;

    // Pin the exact serialized snapshot carried by the request. Pure read:
    // pinning never mutates canonical cognition (C-20: the runtime proposes;
    // it never writes canonical state).
    [[nodiscard]] virtual PinResult pin(const PinRequest& request) const = 0;
};

// First implementation: a file of draft-canonical serialization (the
// golden-pinned format) read through the draft's own materialization path —
// the frozen library owns parsing; the runtime never reimplements it.
class DraftSnapshotReader final : public CanonicalCognitionPort
{
public:
    [[nodiscard]] PinResult pin(const PinRequest& request) const override;
};
} // namespace qiven::runtime::port
