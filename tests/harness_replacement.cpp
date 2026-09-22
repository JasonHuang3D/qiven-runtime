// ============================================================================
// harness_replacement.cpp — C-22 proof: changing the harness preserves
// core component semantics AFTER adapter conformance (component ADL
// §89 RCA-16, §22).
//
// Two distinct harness adapters (harness-alpha, harness-beta) each pass
// the manifest handshake against ONE accepted profile (both bindings in
// the GovernedActorSet; identical capability surfaces). The SAME
// control scenario proposed through each harness produces IDENTICAL
// control outcomes: same derived requirements, same phases, same
// dispositions. Harness identity lives in adapter data and correlation
// keys — never in control semantics.
// ============================================================================

#include <qiven/runtime/adapter/handshake.hpp>
#include <qiven/runtime/port/cognition_port.hpp>
#include <qiven/runtime/state.hpp>

#include <qiven/context/persistence.hpp>
#include <qiven/contracts.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>

namespace
{
using qiven::u64;
using qiven::usize;
using qiven::context::ActionKind;
using qiven::context::InvocationRule;
using qiven::context::RequirementBoundary;
using qiven::context::RequirementKind;
using qiven::context::Snapshot;
using qiven::runtime::BoundedIngressQueue;
using qiven::runtime::ControlCore;
using qiven::runtime::Executor;
using qiven::runtime::IngressMessage;
using qiven::runtime::StructuralFacts;
using qiven::runtime::TransactionMinter;
using qiven::runtime::TransactionPhase;
using qiven::runtime::profile::ClaimChannelCoverage;
using qiven::runtime::profile::DeploymentProfile;
using qiven::runtime::profile::GovernedActorSet;
using qiven::runtime::profile::ProfileBuilder;

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
    void write(const qiven::context::Bytes& bytes) const
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

qiven::runtime::port::PinnedCognition pin_ruled()
{
    Snapshot snapshot;
    snapshot.invocation.present = true;
    InvocationRule rule {};
    rule.action      = ActionKind::BeginTask;
    rule.requirement = RequirementKind::VerifyCanonical;
    rule.subject     = "canonical source";
    rule.boundary    = RequirementBoundary::BeforeJudgment;
    snapshot.invocation.rules.push_back(rule);

    static std::atomic<u64> counter { 0 };
    const TempFile file("qiven-rca16-" + std::to_string(counter.fetch_add(1)) + ".bin");
    file.write(qiven::context::serialize_snapshot(snapshot));
    qiven::runtime::port::DraftSnapshotReader reader;
    auto pinned = reader.pin(qiven::runtime::port::PinRequest { file.path(), "" });
    QIVEN_VERIFY(pinned.is_ok());
    return std::move(pinned).value();
}

// One accepted profile admitting BOTH harness adapters with identical
// capability surfaces.
DeploymentProfile dual_harness_profile()
{
    const qiven::runtime::AdapterInstanceId alpha { qiven::fnv1a64("harness-alpha") };
    const qiven::runtime::AdapterInstanceId beta { qiven::fnv1a64("harness-beta") };

    GovernedActorSet actors;
    actors.revision = 1;
    qiven::runtime::profile::ActorBinding alpha_binding;
    alpha_binding.adapter          = alpha;
    alpha_binding.session_token    = 1;
    alpha_binding.credential_token = 1;
    actors.actors.push_back(alpha_binding);
    qiven::runtime::profile::ActorBinding beta_binding;
    beta_binding.adapter          = beta;
    beta_binding.session_token    = 1;
    beta_binding.credential_token = 2;
    actors.actors.push_back(beta_binding);

    qiven::runtime::profile::CapabilityUniverse universe;
    universe.revision = 1;
    for (const auto adapter : { alpha, beta })
    {
        qiven::runtime::adapter::CapabilityDescriptor observe {};
        observe.id              = qiven::runtime::CapabilityId { adapter.fnv };
        observe.adapter         = adapter;
        observe.operation_class = qiven::runtime::adapter::OperationClass::Observe;
        universe.capabilities.push_back(observe);
    }

    auto built = ProfileBuilder {}
                     .set_name("dual-harness")
                     .set_revision(qiven::runtime::ProfileRevision { 2 })
                     .set_actor_set(std::move(actors))
                     .set_capability_universe(std::move(universe))
                     .set_claims(ClaimChannelCoverage { true, false })
                     .set_control_version(1)
                     .set_conformance_evidence("rca-16")
                     .build();
    QIVEN_VERIFY(built.is_ok());
    return std::move(built).value();
}

// The same exact proposal content routed through one harness adapter.
std::vector<std::string> run_through(const char* harness_name, const DeploymentProfile& profile,
                                     const qiven::runtime::port::PinnedCognition& cognition)
{
    TransactionMinter minter;
    Executor pool { 2, 8 };
    BoundedIngressQueue ingress { 16 };
    ControlCore core { minter, pool,
                       [](const qiven::runtime::RequirementIdentity& identity) {
                           return std::make_optional(qiven::runtime::resolver::make_evidence_receipt(
                               identity, qiven::runtime::resolver::ResolverIdentity { "loopback", 1 }, identity.subject,
                               "harness-proof", std::span<const std::byte>()));
                       },
                       cognition, StructuralFacts {} };

    const qiven::runtime::AdapterInstanceId adapter { qiven::fnv1a64(harness_name) };
    IngressMessage proposal;
    proposal.kind        = IngressMessage::Kind::Proposal;
    proposal.correlation = qiven::runtime::CorrelationKey { qiven::runtime::RuntimeGenerationId { 1 }, adapter,
                                                            qiven::runtime::HarnessSessionId { 1 },
                                                            qiven::runtime::ActorInstanceId { 1 },
                                                            qiven::runtime::HarnessActionId { 1 } };
    const std::byte blob[] { std::byte { 0x60 } };
    proposal.action = qiven::runtime::observe_action(adapter, qiven::runtime::HarnessSessionId { 1 },
                                                     qiven::runtime::ActorInstanceId { 1 },
                                                     qiven::runtime::CapabilityId { adapter.fnv }, "op", "target",
                                                     std::span<const std::byte>(blob, 1));
    QIVEN_VERIFY(ingress.try_push(std::move(proposal)));

    for (int spin = 0; spin < 400; ++spin)
    {
        core.drain(ingress);
        const auto views = core.views();
        if (!views.empty() && (views[0].phase == TransactionPhase::CognitiveAllowed ||
                               views[0].phase == TransactionPhase::Denied))
        {
            QIVEN_VERIFY(views[0].phase == TransactionPhase::CognitiveAllowed);
            const auto* transaction = core.transaction(views[0].id);
            QIVEN_VERIFY(transaction != nullptr);
            std::vector<std::string> requirements;
            for (const auto& prepared : transaction->packet.requirements)
            {
                requirements.push_back(prepared.requirement.subject);
            }
            return requirements;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    QIVEN_VERIFY(false && "pipeline did not settle");
    return {};
}
} // namespace

int main()
{
    const auto cognition            = pin_ruled();
    const DeploymentProfile profile = dual_harness_profile();

    // adapter conformance FIRST (C-22 precondition): both harnesses pass
    // the manifest handshake against the accepted profile
    {
        qiven::runtime::adapter::AdapterManifest alpha;
        alpha.adapter_name     = "harness-alpha";
        alpha.credential_token = 1;
        qiven::runtime::adapter::CapabilityDescriptor observe {};
        observe.id              = qiven::runtime::CapabilityId { qiven::fnv1a64("harness-alpha") };
        observe.adapter         = qiven::runtime::AdapterInstanceId { qiven::fnv1a64("harness-alpha") };
        observe.operation_class = qiven::runtime::adapter::OperationClass::Observe;
        alpha.capabilities.push_back(observe);

        qiven::runtime::adapter::AdapterManifest beta = alpha;
        beta.adapter_name                             = "harness-beta";
        beta.credential_token                         = 2;
        for (auto& capability : beta.capabilities)
        {
            capability.adapter.fnv = qiven::fnv1a64("harness-beta");
            capability.id          = qiven::runtime::CapabilityId { qiven::fnv1a64("harness-beta") };
        }

        const auto alpha_result = qiven::runtime::adapter::admit_against_profile(alpha, profile);
        const auto beta_result  = qiven::runtime::adapter::admit_against_profile(beta, profile);
        QIVEN_VERIFY(alpha_result.admitted);
        QIVEN_VERIFY(beta_result.admitted);
    }

    // C-22: the same scenario through harness-alpha and harness-beta
    // derives IDENTICAL requirements - core semantics are unchanged by
    // the harness swap
    {
        const auto via_alpha = run_through("harness-alpha", profile, cognition);
        const auto via_beta  = run_through("harness-beta", profile, cognition);
        QIVEN_VERIFY(via_alpha.size() == via_beta.size());
        QIVEN_VERIFY(!via_alpha.empty());
        for (usize i = 0; i < via_alpha.size(); ++i)
        {
            QIVEN_VERIFY(via_alpha[i] == via_beta[i]);
        }
    }

    // harness identity never leaks into semantics: classification of the
    // same action content through either adapter yields the same floor
    // (the adapter field is identity, not classification input)
    {
        const std::byte blob[] { std::byte { 0x61 } };
        const auto alpha_action = qiven::runtime::observe_action(
            qiven::runtime::AdapterInstanceId { qiven::fnv1a64("harness-alpha") }, qiven::runtime::HarnessSessionId { 1 },
            qiven::runtime::ActorInstanceId { 1 }, qiven::runtime::CapabilityId { 1 }, "op", "target",
            std::span<const std::byte>(blob, 1));
        const auto beta_action = qiven::runtime::observe_action(
            qiven::runtime::AdapterInstanceId { qiven::fnv1a64("harness-beta") }, qiven::runtime::HarnessSessionId { 1 },
            qiven::runtime::ActorInstanceId { 1 }, qiven::runtime::CapabilityId { 1 }, "op", "target",
            std::span<const std::byte>(blob, 1));

        const auto alpha_set = classify(alpha_action, StructuralFacts {});
        const auto beta_set  = classify(beta_action, StructuralFacts {});
        QIVEN_VERIFY(alpha_set.contains_kind(ActionKind::BeginTask));
        QIVEN_VERIFY(beta_set.contains_kind(ActionKind::BeginTask));
        QIVEN_VERIFY(alpha_set.members().size() == beta_set.members().size());
    }

    std::printf("[ OK ] harness-replacement\n");
    return 0;
}
