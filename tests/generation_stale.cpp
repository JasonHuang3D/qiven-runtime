// ============================================================================
// generation_stale — MVP-2 exit gate 4 (ARCH section 15): a profile or
// bundle update creates a new generation and invalidates old unconsumed
// tokens; consumed decisions stay consumed; boot recovery never
// resurrects a stale token.
// ============================================================================

#include <qiven/contracts.hpp>
#include <qiven/runtime/host/generation_activation.hpp>
#include <qiven/runtime/identity.hpp>
#include <qiven/runtime/journal/runtime_journal.hpp>

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

namespace
{
using qiven::runtime::ContentDigest;
using qiven::runtime::DecisionId;
using qiven::runtime::SortableIdMinter;
using qiven::runtime::TokenHash;
using qiven::runtime::host::activate_generation;
using qiven::runtime::journal::JournalOpenIntent;
using qiven::runtime::journal::RuntimeJournal;

constexpr qiven::u64 t0 = 3'000'000;

ContentDigest digest_of(qiven::u64 seed)
{
    ContentDigest digest;
    const std::string text = "generation-stale-" + std::to_string(seed);
    digest.sha256          = qiven::sha256(text);
    return digest;
}

qiven::Result<DecisionId> bind_under(RuntimeJournal& journal,
                                     qiven::runtime::ControlTransactionId transaction,
                                     qiven::runtime::RuntimeGenerationId generation,
                                     qiven::u64 now_ms, SortableIdMinter& minter)
{
    qiven::runtime::journal::DecisionBind bind;
    bind.id             = minter.next();
    bind.transaction    = transaction;
    bind.generation     = generation;
    bind.expires_ms     = now_ms + 600'000;
    bind.binding_digest = digest_of(static_cast<qiven::u64>(transaction.value));
    // Hash-only storage (production-MVP section 10.3): the ledger keeps the
    // SHA-256 of a token value, never the value.
    TokenHash hash;
    hash.value      = qiven::sha256(std::string("token-") + std::to_string(transaction.value));
    bind.token_hash = hash;
    auto bound      = journal.bind_decision(bind, now_ms);
    if (!bound.is_ok())
    {
        return qiven::Result<DecisionId>::fail(bound.reason());
    }
    return bind.id;
}
} // namespace

int main()
{
    const std::filesystem::path workroot = QIVEN_RUNTIME_TEST_WORKROOT;
    const std::filesystem::path dir      = workroot / "generation-stale";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::filesystem::path file = dir / "journal.sqlite3";

    SortableIdMinter minter;

    auto opened = RuntimeJournal::open(file, JournalOpenIntent::CreateNew, t0);
    QIVEN_VERIFY(opened.is_ok());
    std::unique_ptr<RuntimeJournal> journal = std::move(opened.value());

    // Generation 1 activates (bundle digest A, profile digest P).
    auto g1 = activate_generation(*journal, digest_of(1), digest_of(100), "test-build", t0 + 1);
    QIVEN_VERIFY(g1.is_ok());
    QIVEN_VERIFY(g1.value().value == 1);

    // Two transactions bind decisions under g1; one is consumed.
    auto tx1 = journal->open_transaction({ {}, "corr-1", digest_of(11), std::nullopt }, t0 + 2);
    QIVEN_VERIFY(tx1.is_ok());
    auto tx2 = journal->open_transaction({ {}, "corr-2", digest_of(12), std::nullopt }, t0 + 3);
    QIVEN_VERIFY(tx2.is_ok());
    auto decision_a = bind_under(*journal, tx1.value(), g1.value(), t0 + 4, minter);
    QIVEN_VERIFY(decision_a.is_ok());
    auto decision_b = bind_under(*journal, tx2.value(), g1.value(), t0 + 5, minter);
    QIVEN_VERIFY(decision_b.is_ok());
    QIVEN_VERIFY(journal->consume_decision(decision_a.value(), t0 + 6).is_ok());
    QIVEN_VERIFY(journal->decision_state_of(decision_a.value()).value() == "consumed");
    QIVEN_VERIFY(journal->decision_state_of(decision_b.value()).value() == "bound");

    // A BUNDLE update creates a new generation: every unconsumed decision
    // from older generations becomes stale (same transaction); consumed
    // decisions are untouched.
    auto g2 = activate_generation(*journal, digest_of(2), digest_of(100), "test-build", t0 + 10);
    QIVEN_VERIFY(g2.is_ok());
    QIVEN_VERIFY(g2.value().value == 2);
    QIVEN_VERIFY(journal->decision_state_of(decision_b.value()).value() == "stale");
    QIVEN_VERIFY(journal->decision_state_of(decision_a.value()).value() == "consumed");
    // The stale token can no longer be consumed (typed 53-class denial).
    QIVEN_VERIFY(!journal->consume_decision(decision_b.value(), t0 + 11).is_ok());

    // A PROFILE update does the same to anything bound under g2.
    auto tx3 = journal->open_transaction({ {}, "corr-3", digest_of(13), std::nullopt }, t0 + 12);
    QIVEN_VERIFY(tx3.is_ok());
    auto decision_c = bind_under(*journal, tx3.value(), g2.value(), t0 + 13, minter);
    QIVEN_VERIFY(decision_c.is_ok());
    auto g3 = activate_generation(*journal, digest_of(2), digest_of(101), "test-build", t0 + 14);
    QIVEN_VERIFY(g3.is_ok());
    QIVEN_VERIFY(g3.value().value == 3);
    QIVEN_VERIFY(journal->decision_state_of(decision_c.value()).value() == "stale");
    QIVEN_VERIFY(!journal->consume_decision(decision_c.value(), t0 + 15).is_ok());

    // Boot recovery does not resurrect stale tokens.
    auto recovery = journal->recover_at(t0 + 20);
    QIVEN_VERIFY(recovery.is_ok());
    QIVEN_VERIFY(journal->decision_state_of(decision_b.value()).value() == "stale");
    QIVEN_VERIFY(journal->decision_state_of(decision_c.value()).value() == "stale");
    QIVEN_VERIFY(journal->decision_state_of(decision_a.value()).value() == "consumed");
    QIVEN_VERIFY(journal->verify_audit_chain().is_ok());

    // Reopen across process death (close + reopen): the audit chain still
    // verifies and max_generation_id drives monotonic activation.
    journal.reset();
    auto reopened = RuntimeJournal::open(file, JournalOpenIntent::OpenExisting, t0 + 30);
    QIVEN_VERIFY(reopened.is_ok());
    journal = std::move(reopened.value());
    auto g4 = activate_generation(*journal, digest_of(3), digest_of(101), "test-build", t0 + 31);
    QIVEN_VERIFY(g4.is_ok());
    QIVEN_VERIFY(g4.value().value == 4);
    QIVEN_VERIFY(journal->verify_audit_chain().is_ok());

    std::printf("[ OK ] generation-stale\n");
    return 0;
}
