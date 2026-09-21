#include <qiven/runtime/port/cognition_port.hpp>

#include <qiven/context/persistence.hpp>
#include <qiven/contracts.hpp>
#include <qiven/hashing_sha256.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace
{
using qiven::usize;
using qiven::context::Bytes;
using qiven::context::InvocationRule;
using qiven::context::Snapshot;
using qiven::runtime::port::CanonicalCognitionPort;
using qiven::runtime::port::DraftSnapshotReader;
using qiven::runtime::port::PinErrorKind;
using qiven::runtime::port::PinRequest;
using qiven::runtime::port::PinResult;

class TempFile
{
public:
    explicit TempFile(std::string name) :
    m_path(std::filesystem::temp_directory_path() / name)
    {
        std::error_code ignored;
        std::filesystem::remove(m_path, ignored);
    }
    ~TempFile()
    {
        std::error_code ignored;
        std::filesystem::remove(m_path, ignored);
    }

    void write(const Bytes& bytes)
    {
        std::ofstream out(m_path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return m_path;
    }

private:
    std::filesystem::path m_path;
};

Bytes bytes_of(const Snapshot& snapshot)
{
    return qiven::context::serialize_snapshot(snapshot);
}
} // namespace

int main()
{
    // a snapshot with a real invocation policy serializes to a file a pin
    // can materialize; the pin reports the exact bytes it holds
    {
        Snapshot snapshot;
        snapshot.invocation.present = true;
        InvocationRule rule {};
        rule.subject = "rca-3 pin test";
        snapshot.invocation.rules.push_back(rule);

        const Bytes canonical = bytes_of(snapshot);
        TempFile file("qiven-rca3-pin-ok.bin");
        file.write(canonical);

        DraftSnapshotReader reader;
        const PinResult result = reader.pin(PinRequest { file.path(), "" });
        QIVEN_VERIFY(result.is_ok());
        const auto& pinned = result.value();

        QIVEN_VERIFY(pinned.snapshot != nullptr);
        QIVEN_VERIFY(pinned.revision.value.rfind("sha256:", 0) == 0);
        QIVEN_VERIFY(pinned.snapshot_digest_sha256 ==
                     qiven::runtime::ContentDigest { qiven::sha256(canonical.data(), canonical.size()) });

        // roundtrip identity: re-serializing the pinned value tree reproduces
        // the exact canonical bytes that were pinned
        const Bytes roundtrip = bytes_of(*pinned.snapshot);
        QIVEN_VERIFY(roundtrip.size() == canonical.size());
        bool identical = true;
        for (usize i = 0; i < canonical.size(); ++i)
        {
            identical = identical && canonical[i] == roundtrip[i];
        }
        QIVEN_VERIFY(identical);
    }

    // pin is stable: two pins of the same file carry the same revision
    // identity and the same policy digest (the staleness primitive)
    {
        Snapshot snapshot;
        snapshot.invocation.present = true;
        TempFile file("qiven-rca3-pin-stable.bin");
        file.write(bytes_of(snapshot));

        DraftSnapshotReader reader;
        const PinResult first  = reader.pin(PinRequest { file.path(), "" });
        const PinResult second = reader.pin(PinRequest { file.path(), "" });
        QIVEN_VERIFY(first.is_ok() && second.is_ok());
        QIVEN_VERIFY(first.value().same_revision(second.value()));
        QIVEN_VERIFY(first.value().policy_digest == second.value().policy_digest);
        QIVEN_VERIFY(first.value().snapshot_digest_sha256 == second.value().snapshot_digest_sha256);
    }

    // a different invocation policy produces a different policy digest
    // (frozen policy derivation is a function of the policy content)
    {
        Snapshot plain;
        plain.invocation.present = true;

        Snapshot ruled;
        ruled.invocation.present = true;
        InvocationRule rule {};
        rule.subject = "changes the policy preimage";
        ruled.invocation.rules.push_back(rule);

        TempFile plain_file("qiven-rca3-pin-plain.bin");
        TempFile ruled_file("qiven-rca3-pin-ruled.bin");
        plain_file.write(bytes_of(plain));
        ruled_file.write(bytes_of(ruled));

        DraftSnapshotReader reader;
        const PinResult plain_pin = reader.pin(PinRequest { plain_file.path(), "" });
        const PinResult ruled_pin = reader.pin(PinRequest { ruled_file.path(), "" });
        QIVEN_VERIFY(plain_pin.is_ok() && ruled_pin.is_ok());
        QIVEN_VERIFY(plain_pin.value().policy_digest != ruled_pin.value().policy_digest);
        QIVEN_VERIFY(!plain_pin.value().same_revision(ruled_pin.value()));
    }

    // a missing file is a typed failure, never an exception or assert
    {
        DraftSnapshotReader reader;
        const PinResult result = reader.pin(
            PinRequest { std::filesystem::temp_directory_path() / "qiven-rca3-pin-missing.bin", "" });
        QIVEN_VERIFY(!result.is_ok());
        QIVEN_VERIFY(result.reason().kind == PinErrorKind::FileUnavailable);
    }

    // corrupt bytes fail closed through the typed channel (DR-009 echo)
    {
        Snapshot snapshot;
        snapshot.invocation.present = true;
        Bytes corrupt               = bytes_of(snapshot);
        if (corrupt.size() > 8)
        {
            for (usize i = 0; i < 8; ++i)
            {
                corrupt[corrupt.size() - 1 - i] = std::byte { 0xFF };
            }
        }

        TempFile file("qiven-rca3-pin-corrupt.bin");
        file.write(corrupt);

        DraftSnapshotReader reader;
        const PinResult result = reader.pin(PinRequest { file.path(), "" });
        QIVEN_VERIFY(!result.is_ok());
        QIVEN_VERIFY(result.reason().kind != PinErrorKind::FileUnavailable);
        QIVEN_VERIFY(result.reason().kind != PinErrorKind::Unknown);
    }

    // a declared content-id precondition that does not match is corruption
    // (DigestMismatch), not a warning
    {
        Snapshot snapshot;
        TempFile file("qiven-rca3-pin-digest.bin");
        file.write(bytes_of(snapshot));

        DraftSnapshotReader reader;
        const PinResult result = reader.pin(PinRequest { file.path(), "deadbeef" });
        QIVEN_VERIFY(!result.is_ok());
        QIVEN_VERIFY(result.reason().kind == PinErrorKind::DigestMismatch);
    }

    // the reader satisfies the port interface (transactions bind the port,
    // not the implementation)
    {
        Snapshot snapshot;
        TempFile file("qiven-rca3-pin-port.bin");
        file.write(bytes_of(snapshot));

        DraftSnapshotReader reader;
        const CanonicalCognitionPort& port = reader;
        const PinResult result             = port.pin(PinRequest { file.path(), "" });
        QIVEN_VERIFY(result.is_ok());
    }

    std::printf("[ OK ] cognition-pin\n");
    return 0;
}
