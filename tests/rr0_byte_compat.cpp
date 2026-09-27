// ============================================================================
// rr0_byte_compat.cpp — RR-0 byte-representation remediation goldens
// (qiven-runtime docs/design/rr0-byte-representation.md §3/§4.2)
//
// Consolidation replaces the MECHANICS of every inventoried duplicate
// (local put/get -> foundation codecs + ByteBuilder) and NEVER the
// representation. This test pins each format's serialized bytes to
// fixtures captured at the PRE-consolidation head (98c24c6 semantics):
//
//   framing        full frame bytes (fixed key/header/body)
//   decision       action digest preimage bytes (via action_digest_of)
//   scope          ResourceScope::digest preimage bytes
//   state_store    serialize_test_state_image bytes (+ round trip)
//   manifest       manifest_identity digest preimage bytes
//   journal        audit_events rows (payloads + chain hashes) for a fixed
//                  command sequence with explicit clocks and fixed ids
//
// Capture mode (run ONCE at the pre-consolidation head by the RR-0 batch):
//   rr0-byte-compat --capture <fixture-dir>
// Compare mode (default; wired into the gate): every computed value must
// equal the committed fixture byte-for-byte.
//
// identity.cpp's put_u64_le has NO golden: SortableId128 embeds the wall
// clock, so the bytes are not reproducible. Its mechanic is replaced by
// foundation encode_le_u64 and stays covered by the foundation endian
// suite plus the structural equivalence in the consolidation diff.
// ============================================================================

#include <qiven/contracts.hpp>
#include <qiven/runtime/adapter/manifest.hpp>
#include <qiven/runtime/auth.hpp>
#include <qiven/runtime/decision.hpp>
#include <qiven/runtime/identity.hpp>
#include <qiven/runtime/ipc/framing.hpp>
#include <qiven/runtime/journal/runtime_journal.hpp>
#include <qiven/runtime/observed_action.hpp>
#include <qiven/runtime/reconciliation.hpp>
#include <qiven/runtime/resolver.hpp>
#include <qiven/runtime/scope.hpp>
#include <qiven/runtime/state_store.hpp>

#include <sqlite3.h>

#include <array>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace
{
using qiven::SHA256Digest;
using qiven::u32;
using qiven::u64;
using qiven::usize;
using qiven::runtime::ContentDigest;
using qiven::runtime::TokenHash;
using qiven::runtime::journal::JournalOpenIntent;
using qiven::runtime::journal::RuntimeJournal;

constexpr qiven::u64 t0 = 2'000'000;

constexpr char hex_digit(unsigned value) noexcept
{
    return static_cast<char>(value < 10 ? '0' + value : 'a' + (value - 10));
}

std::string hex_of(std::span<const std::byte> bytes)
{
    std::string out;
    out.resize(bytes.size() * 2);
    for (usize i = 0; i < bytes.size(); ++i)
    {
        const auto b   = static_cast<unsigned char>(bytes[i]);
        out[2 * i]     = hex_digit(b >> 4);
        out[2 * i + 1] = hex_digit(b & 0x0Fu);
    }
    return out;
}

std::string hex_of(const SHA256Digest& digest)
{
    return hex_of({ digest.data(), digest.size() });
}

ContentDigest digest_of(char fill)
{
    ContentDigest digest;
    digest.sha256.fill(static_cast<std::byte>(fill));
    return digest;
}

bool write_text(const std::filesystem::path& path, std::string_view text)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(out);
}

std::optional<std::string> read_text(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return std::nullopt;
    in.seekg(0, std::ios::end);
    const auto size = in.tellg();
    if (size < 0)
        return std::nullopt;
    std::string text;
    text.resize(static_cast<usize>(size));
    in.seekg(0, std::ios::beg);
    if (!text.empty())
    {
        in.read(text.data(), static_cast<std::streamsize>(text.size()));
    }
    if (!in)
        return std::nullopt;
    return text;
}

// ---- fixed inputs ---------------------------------------------------------

qiven::runtime::auth::SecretKey fixed_key()
{
    qiven::runtime::auth::SecretKey key {};
    for (usize i = 0; i < key.size(); ++i)
    {
        key[i] = static_cast<std::byte>(0x5A ^ i);
    }
    return key;
}

std::string frame_bytes_hex()
{
    qiven::runtime::ipc::FrameHeader header;
    header.flags          = 0xA5C3;
    header.request_id     = 0x1122334455667788ull;
    header.connection_seq = 7;
    const qiven::runtime::ipc::FrameCodec codec { fixed_key() };
    const std::string frame = codec.encode(header, "rr0-golden-body");
    return hex_of({ reinterpret_cast<const std::byte*>(frame.data()), frame.size() });
}

std::string decision_action_hex()
{
    const std::array<std::byte, 5> arguments {
        std::byte { 0x01 },
        std::byte { 0x02 },
        std::byte { 0x03 },
        std::byte { 0x04 },
        std::byte { 0xFF },
    };
    const auto action          = qiven::runtime::observe_action(qiven::runtime::AdapterInstanceId { 0x1111222233334444ull },
                                                                qiven::runtime::HarnessSessionId { 0x99ull },
                                                                qiven::runtime::ActorInstanceId { 0x77ull },
                                                                qiven::runtime::CapabilityId { 0x1234ull },
                                                                "build",
                                                                "state/active-work.yaml",
                                                                arguments);
    const ContentDigest digest = qiven::runtime::action_digest_of(action);
    return hex_of(digest.sha256);
}

// The remaining decision preimage encodings (intents / requirements /
// evidence) through one fixed-input bind_allow call. The digests are
// deterministic for fixed inputs (only the decision id and token MAC
// embed wall-clock ids and the CSPRNG nonce; neither feeds these three).
std::string decision_set_digests_hex()
{
    using qiven::context::PreparationPacket;
    using qiven::context::PreparedRequirement;
    using qiven::context::RequirementBoundary;
    using qiven::context::RequirementStatus;
    using qiven::runtime::SortableIdMinter;
    using qiven::runtime::TransactionMinter;
    using qiven::runtime::resolver::EvidenceReceipt;

    constexpr std::array<std::byte, 2> blob { std::byte { 0x42 }, std::byte { 0x99 } };
    auto action  = qiven::runtime::observe_action(qiven::runtime::AdapterInstanceId { 0x1111222233334444ull },
                                                  qiven::runtime::HarnessSessionId { 0x99ull },
                                                  qiven::runtime::ActorInstanceId { 0x77ull },
                                                  qiven::runtime::CapabilityId { 0x1234ull },
                                                  "build",
                                                  "state/active-work.yaml",
                                                  blob);
    auto intents = qiven::runtime::make_intent_set(
        { qiven::runtime::IntentClassification { qiven::context::ActionIntent {},
                                                 qiven::runtime::ClassificationBasis::Mechanical } });
    if (!intents.is_ok())
        return "SCAFFOLD-FAILED";

    const qiven::runtime::CorrelationKey correlation { qiven::runtime::RuntimeGenerationId { 1 },
                                                       qiven::runtime::AdapterInstanceId { 0x1111222233334444ull },
                                                       qiven::runtime::HarnessSessionId { 0x99ull },
                                                       qiven::runtime::ActorInstanceId { 0x77ull },
                                                       qiven::runtime::HarnessActionId { 1 } };

    PreparationPacket packet;
    PreparedRequirement satisfied;
    satisfied.requirement.subject = "search";
    satisfied.boundary            = RequirementBoundary::BeforeExecution;
    satisfied.status              = RequirementStatus::Satisfied;
    packet.requirements.push_back(satisfied);

    TransactionMinter tx_minter;
    auto transaction = qiven::runtime::begin_control_transaction(
        tx_minter.next(), std::nullopt, correlation, std::move(action), std::move(intents).value(),
        qiven::runtime::port::PinnedCognition {}, std::move(packet));
    if (!qiven::runtime::evaluate_before_judgment(transaction) ||
        !qiven::runtime::evaluate_before_execution(transaction) ||
        transaction.phase != qiven::runtime::TransactionPhase::CognitiveAllowed)
    {
        return "SCAFFOLD-FAILED";
    }

    qiven::runtime::profile::ProfileBuilder builder;
    qiven::runtime::profile::GovernedActorSet actors;
    qiven::runtime::profile::ActorBinding binding;
    binding.adapter          = qiven::runtime::AdapterInstanceId { 1 };
    binding.session_token    = 1;
    binding.credential_token = 1;
    actors.actors.push_back(binding);
    auto profile = builder.set_name("rr0-golden-profile")
                       .set_revision(qiven::runtime::ProfileRevision { 5 })
                       .set_actor_set(std::move(actors))
                       .set_conformance_evidence("rr0-golden")
                       .build();
    if (!profile.is_ok())
        return "SCAFFOLD-FAILED";
    qiven::runtime::GenerationMinter generation_minter;
    auto generation = generation_minter.mint(std::move(profile).value(), {}, {});

    EvidenceReceipt receipt {};
    receipt.id.fnv                       = 0xFEEDBEEF01234567ull;
    receipt.requirement.subject          = "search";
    receipt.resolver.name                = "rr0-golden-resolver";
    receipt.resolver.version             = 3;
    receipt.subject                      = "evidence-subject";
    receipt.source                       = "evidence-source";
    receipt.source_revision.value        = "abc123def456";
    receipt.source_path                  = "corpus/golden.jsonl";
    receipt.resolver_build               = "rr0-golden-build";
    receipt.content_digest               = digest_of(0x11);
    receipt.issued_at_ms                 = 1'500'000;
    receipt.expires_at_ms                = 2'000'000;
    receipt.policy_digest                = digest_of(0x22);
    receipt.generation                   = qiven::runtime::RuntimeGenerationId { 1 };
    receipt.reusable_across_transactions = true;
    const std::vector<EvidenceReceipt> evidence { receipt };

    SortableIdMinter ids;
    const auto decision = qiven::runtime::bind_allow(transaction, generation,
                                                     qiven::runtime::ResourceScope {}.digest(), 9,
                                                     evidence, fixed_key(), ids, 1'000'000, 3'600'000);
    if (!decision.is_ok())
        return "SCAFFOLD-FAILED";

    std::string out;
    out += hex_of(decision.value().intent_set_digest.sha256);
    out += "\n";
    out += hex_of(decision.value().requirement_set_digest.sha256);
    out += "\n";
    out += hex_of(decision.value().evidence_set_digest.sha256);
    out += "\n";
    return out;
}

std::string scope_digest_hex()
{
    auto built = qiven::runtime::ResourceScope::Builder {}
                     .add_path("qiven/a/b")
                     .add_path("qiven/c")
                     .add_path("qiven/d/e/f-longer-path-entry")
                     .build();
    if (!built.is_ok())
        return "SCOPE-BUILD-FAILED";
    return hex_of(built.value().digest().sha256);
}

std::string state_image_hex()
{
    std::unordered_set<TokenHash> consumed;
    TokenHash first;
    for (usize i = 0; i < first.value.size(); ++i)
    {
        first.value[i] = static_cast<std::byte>(0x10 + i);
    }
    TokenHash second;
    for (usize i = 0; i < second.value.size(); ++i)
    {
        second.value[i] = static_cast<std::byte>(0xE0 - i);
    }
    consumed.insert(first);
    consumed.insert(second);

    qiven::runtime::ReconciliationBarrier barriers;
    barriers.raise(qiven::runtime::EffectScope { 0x0A0B0C0D0E0F1011ull },
                   qiven::runtime::ControlTransactionId { 1 });
    barriers.raise(qiven::runtime::EffectScope { 0xF1E2D3C4B5A69788ull },
                   qiven::runtime::ControlTransactionId { 1 });

    const std::vector<std::byte> image =
        qiven::runtime::serialize_test_state_image(consumed, barriers);

    // Round trip must hold under the same mechanics (get_u32/get_u64 side).
    const auto restored = qiven::runtime::deserialize_test_state_image(
        { image.data(), image.size() });
    if (!restored || restored->consumed_token_hashes.size() != 2 ||
        restored->barrier_scopes.size() != 2)
    {
        return "ROUND-TRIP-FAILED";
    }
    return hex_of({ image.data(), image.size() });
}

std::string manifest_identity_hex()
{
    qiven::runtime::adapter::AdapterManifest manifest;
    manifest.adapter_name     = "rr0-golden-adapter";
    manifest.manifest_version = 2;
    qiven::runtime::adapter::CapabilityDescriptor read_cap;
    read_cap.id                 = qiven::runtime::CapabilityId { 0x0A0B0C0Dull };
    read_cap.adapter            = qiven::runtime::AdapterInstanceId { 0x1111ull };
    read_cap.operation_class    = qiven::runtime::adapter::OperationClass::Observe;
    read_cap.can_mutate_world   = false;
    read_cap.can_publish_claims = true;
    qiven::runtime::adapter::CapabilityDescriptor write_cap;
    write_cap.id                           = qiven::runtime::CapabilityId { 0x0E0F1011ull };
    write_cap.adapter                      = qiven::runtime::AdapterInstanceId { 0x1111ull };
    write_cap.operation_class              = qiven::runtime::adapter::OperationClass::FileSystemWrite;
    write_cap.can_mutate_world             = true;
    write_cap.requires_execution_authority = true;
    manifest.capabilities.push_back(read_cap);
    manifest.capabilities.push_back(write_cap);
    return hex_of(qiven::runtime::adapter::manifest_identity(manifest).sha256);
}

// The fixed journal command sequence. Every clock and every caller-minted
// id is explicit — no wall clock, no minters.
bool run_journal_sequence(const std::filesystem::path& file, std::string& rows)
{
    auto opened = RuntimeJournal::open(file, JournalOpenIntent::CreateNew, t0);
    if (!opened.is_ok())
        return false;
    auto journal = std::move(opened.value());

    if (!journal->advance_generation(qiven::runtime::RuntimeGenerationId { 1 }, digest_of(0x01),
                                     digest_of(0x02), "rr0-golden-build", t0 + 1)
             .is_ok())
        return false;

    if (!journal
             ->open_session({ qiven::runtime::RuntimeGenerationId { 1 },
                              std::optional<qiven::u64> { 7 }, "zcode" },
                            t0 + 2)
             .is_ok())
        return false;

    qiven::runtime::journal::TransactionOpen open;
    open.correlation    = "gen=1;adapter=1;session=1;actor=7;action=1";
    open.request_digest = digest_of(0x03);
    const auto tx       = journal->open_transaction(open, t0 + 3);
    if (!tx.is_ok())
        return false;

    const ContentDigest evidence[] = { digest_of(0x04), digest_of(0x05) };
    if (!journal->accept_evidence(tx.value(), evidence, t0 + 4).is_ok())
        return false;

    qiven::runtime::journal::DecisionBind bind;
    for (usize i = 0; i < bind.id.bytes.size(); ++i)
    {
        bind.id.bytes[i] = static_cast<std::byte>(0x40 + i);
    }
    bind.token_hash.value.fill(static_cast<std::byte>(0x7E));
    bind.transaction    = tx.value();
    bind.generation     = qiven::runtime::RuntimeGenerationId { 1 };
    bind.binding_digest = digest_of(0x06);
    bind.expires_ms     = t0 + 1'000'000;
    if (!journal->bind_decision(bind, t0 + 5).is_ok())
        return false;

    qiven::runtime::journal::DispatchPrepared dispatch;
    for (usize i = 0; i < dispatch.id.bytes.size(); ++i)
    {
        dispatch.id.bytes[i] = static_cast<std::byte>(0x80 - i);
    }
    dispatch.transaction = tx.value();
    dispatch.plan_digest = digest_of(0x07);
    if (!journal->record_dispatch_prepared(dispatch, t0 + 7).is_ok())
        return false;

    qiven::runtime::journal::OutcomeRecord outcome;
    outcome.dispatch     = dispatch.id;
    outcome.status       = std::string(qiven::runtime::journal::tx_state::succeeded);
    outcome.ref_observed = std::optional<std::string> { "abc123" };
    if (!journal->record_outcome(outcome, t0 + 8).is_ok())
        return false;

    if (!journal->verify_audit_chain().is_ok())
        return false;

    // Read the audit rows back through SQLite directly (the tampering
    // channel a real fault would use; also the most honest byte view).
    // The fixture carries seq|kind|payload — the payload bytes pin every
    // put_u32/u64/str/bytes/digest/id mechanic exactly. The event_hash
    // column is deliberately NOT fixtured: row 1 (journal_created) embeds
    // the per-database random install id, and every later hash chains
    // from it — chain integrity is instead proven by verify_audit_chain()
    // above. The install id itself (the only nondeterministic bytes) is
    // masked at a fixed offset: 4-byte length prefix, then 32 bytes.
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(file.string().c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
    {
        sqlite3_close_v2(db);
        return false;
    }
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db,
                           "SELECT seq, kind, hex(payload) FROM audit_events ORDER BY seq",
                           -1, &stmt, nullptr) != SQLITE_OK)
    {
        sqlite3_finalize(stmt);
        sqlite3_close_v2(db);
        return false;
    }
    rows.clear();
    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        rows += std::to_string(sqlite3_column_int64(stmt, 0));
        rows += '|';
        rows += reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        rows += '|';
        std::string kind    = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        std::string payload = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        if (kind == "journal_created" && payload.size() >= 8 + 64)
        {
            // mask the 32-byte install id behind the row-1 length prefix
            for (usize i = 8; i < 8 + 64; ++i)
            {
                payload[i] = '*';
            }
        }
        rows += payload;
        rows += '\n';
    }
    sqlite3_finalize(stmt);
    sqlite3_close_v2(db);
    return !rows.empty();
}

std::string journal_rows_hex()
{
    const auto root = std::filesystem::path(QIVEN_RUNTIME_TEST_WORKROOT) / "rr0";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    std::string rows;
    if (!run_journal_sequence(root / "golden.sqlite3", rows))
        return "JOURNAL-SEQUENCE-FAILED";
    return rows;
}

struct Fixture
{
    const char* name;
    std::string (*produce)();
};

const Fixture fixtures[] = {
    { "frame.hex", frame_bytes_hex },
    { "decision-action.sha256", decision_action_hex },
    { "decision-set-digests.txt", decision_set_digests_hex },
    { "scope.sha256", scope_digest_hex },
    { "state-image.hex", state_image_hex },
    { "manifest.sha256", manifest_identity_hex },
    { "journal-audit.txt", journal_rows_hex },
};
} // namespace

int main(int argc, char** argv)
{
    const std::string eol(1, char(10));
    const bool capture = argc > 2 && std::string_view(argv[1]) == "--capture";
    const std::filesystem::path base =
        capture ? std::filesystem::path(argv[2]) : std::filesystem::path(QIVEN_RUNTIME_RR0_FIXTURES);

    usize failed = 0;
    for (const Fixture& fixture : fixtures)
    {
        const std::string produced = fixture.produce();
        const auto path            = base / fixture.name;

        if (capture)
        {
            if (!write_text(path, produced))
            {
                std::printf("[FAIL] capture %s: write failed%s", fixture.name, eol.c_str());
                ++failed;
                continue;
            }
            std::printf("[ OK ] captured %s (%zu bytes)%s", fixture.name, produced.size(), eol.c_str());
            continue;
        }

        const auto expected = read_text(path);
        if (!expected)
        {
            std::printf("[FAIL] %s: fixture missing at %s%s", fixture.name, path.string().c_str(),
                        eol.c_str());
            ++failed;
            continue;
        }
        if (*expected != produced)
        {
            std::printf("[FAIL] %s: byte mismatch%s  expected: %.80s...%s  produced: %.80s...%s",
                        fixture.name, eol.c_str(), expected->c_str(), eol.c_str(), produced.c_str(),
                        eol.c_str());
            ++failed;
            continue;
        }
        std::printf("[ OK ] %s byte-identical (%zu bytes)%s", fixture.name, produced.size(),
                    eol.c_str());
    }

    if (capture)
    {
        std::printf("%s: %zu fixture(s) captured%s", failed == 0 ? "CAPTURE PASS" : "CAPTURE FAIL",
                    std::size(fixtures), eol.c_str());
        return failed == 0 ? 0 : 1;
    }
    std::printf("%s: %zu/%zu format goldens byte-identical%s",
                failed == 0 ? "RR0-BYTE-COMPAT PASS" : "RR0-BYTE-COMPAT FAIL",
                std::size(fixtures) - failed, std::size(fixtures), eol.c_str());
    return failed == 0 ? 0 : 1;
}
