// ============================================================================
// policy_parse — cognition::parse_invocation_policy strict subset
// (MVP-2 batch design section 6: parser subset + fail-closed classes)
// ============================================================================

#include <qiven/contracts.hpp>
#include <qiven/runtime/cognition/policy.hpp>

#include <cstdio>
#include <string>

namespace
{
using qiven::runtime::cognition::parse_invocation_policy;

// The canonical machine instance's rule block (mirrors qiven-context
// runtime/invocation-policy.yaml; the equality pin lives in that
// repository's validator — here the SHAPE is under test).
const std::string canonical = R"policy(schema: qiven-invocation-policy-v1
version: 1

policy:
  present: true
  rules:
    - action: CreateCppSymbol
      requirement: MandatoryRecall
      boundary: BeforeJudgment
      subject: naming policy
      blocking: true
    - action: Publish
      requirement: RequestReview
      boundary: BeforeExecution
      subject: H2 exact-delta review
      blocking: true

resolvers:
  - requirement: MandatoryRecall
    type: snapshot_resolver
    min_version: 1
  - requirement: RequestReview
    type: review_receipt_resolver
    min_version: 1

freshness:
  evidence_ttl_ms: 900000
  bundle_freshness_ms: 604800000

enforcement:
  unsatisfied_before_judgment: redeliberate
  unsatisfied_before_execution: deny
)policy";
} // namespace

int main()
{
    {
        auto parsed = parse_invocation_policy(canonical);
        QIVEN_VERIFY(parsed.is_ok());
        QIVEN_VERIFY(parsed.value().invocation.present);
        QIVEN_VERIFY(parsed.value().invocation.rules.size() == 2);
        const auto& first = parsed.value().invocation.rules[0];
        QIVEN_VERIFY(first.action == qiven::context::ActionKind::CreateCppSymbol);
        QIVEN_VERIFY(first.requirement == qiven::context::RequirementKind::MandatoryRecall);
        QIVEN_VERIFY(first.boundary == qiven::context::RequirementBoundary::BeforeJudgment);
        QIVEN_VERIFY(first.subject == "naming policy");
        QIVEN_VERIFY(first.blocking);
        QIVEN_VERIFY(!first.claimClass.has_value());
        QIVEN_VERIFY(parsed.value().resolvers.size() == 2);
        QIVEN_VERIFY(parsed.value().resolvers[0].type == "snapshot_resolver");
        QIVEN_VERIFY(parsed.value().evidence_ttl_ms == 900000);
        QIVEN_VERIFY(parsed.value().bundle_freshness_ms == 604800000);
        QIVEN_VERIFY(parsed.value().redeliberate_on_before_judgment);
        QIVEN_VERIFY(parsed.value().deny_on_before_execution);
    }

    // A rule with a claim class parses (the vocabulary axis exists).
    {
        const std::string text = R"policy(schema: qiven-invocation-policy-v1
version: 1
policy:
  present: true
  rules:
    - action: MakeCanonicalClaim
      claim: CanonicalFact
      requirement: VerifyCanonical
      boundary: BeforeJudgment
      subject: canonical record
      blocking: false
resolvers:
  - requirement: VerifyCanonical
    type: git_tree_resolver
    min_version: 2
freshness:
  evidence_ttl_ms: 1
  bundle_freshness_ms: 2
enforcement:
  unsatisfied_before_judgment: deny
  unsatisfied_before_execution: deny
)policy";
        auto parsed            = parse_invocation_policy(text);
        QIVEN_VERIFY(parsed.is_ok());
        QIVEN_VERIFY(parsed.value().invocation.rules[0].claimClass.has_value());
        QIVEN_VERIFY(*parsed.value().invocation.rules[0].claimClass ==
                     qiven::context::ClaimClass::CanonicalFact);
        QIVEN_VERIFY(!parsed.value().invocation.rules[0].blocking);
        QIVEN_VERIFY(!parsed.value().redeliberate_on_before_judgment);
        QIVEN_VERIFY(parsed.value().resolvers[0].min_version == 2);
    }

    // Fail-closed classes: each malformed variant must deny.
    {
        const auto deny = [](const std::string& text) {
            return !parse_invocation_policy(text).is_ok();
        };
        QIVEN_VERIFY(deny("schema: qiven-invocation-policy-v2\nversion: 1\n"));
        QIVEN_VERIFY(deny(""));
        QIVEN_VERIFY(deny("version: 1\n")); // schema header required
        QIVEN_VERIFY(deny(canonical + "\nrogue_key: 1\n"));
        QIVEN_VERIFY(deny(std::string(canonical).replace(
            canonical.find("CreateCppSymbol"), 15, "NotAnAction")));
        QIVEN_VERIFY(deny(std::string(canonical).replace(
            canonical.find("BeforeJudgment"), 15, "BeforeJudgement")));
        QIVEN_VERIFY(deny(std::string(canonical).replace(
            canonical.find("unsatisfied_before_judgment: redeliberate"), 42,
            "unsatisfied_before_judgment: maybe")));
        QIVEN_VERIFY(deny(std::string(canonical).replace(
            canonical.find("evidence_ttl_ms: 900000"), 23, "evidence_ttl_ms: 0")));
        QIVEN_VERIFY(deny(std::string(canonical).replace(
            canonical.find("present: true"), 13, "present: false")));
        // Flow-style rows are outside the bounded shape.
        QIVEN_VERIFY(deny(std::string(canonical).replace(
            canonical.find("    - action: CreateCppSymbol"), 30,
            "    - { action: CreateCppSymbol }")));
        // Tabs are rejected outright.
        QIVEN_VERIFY(deny(std::string(canonical).replace(canonical.find("version: 1"), 9,
                                                         "version:\t1")));
        // A rules header with zero rows is a fail-closed instance.
        QIVEN_VERIFY(deny("schema: qiven-invocation-policy-v1\nversion: 1\npolicy:\n  present: true\n  rules:\nresolvers: []\nfreshness:\n  evidence_ttl_ms: 1\n  bundle_freshness_ms: 1\nenforcement:\n  unsatisfied_before_judgment: deny\n  unsatisfied_before_execution: deny\n"));
        // Oversize input fails the bound.
        QIVEN_VERIFY(deny(std::string(qiven::runtime::cognition::max_policy_bytes + 1, '#')));
    }

    std::printf("[ OK ] policy-parse\n");
    return 0;
}
