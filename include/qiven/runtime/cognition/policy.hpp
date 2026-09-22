#pragma once

// ============================================================================
// cognition/policy.hpp — strict parser for the canonical invocation-policy
// machine instance (MVP-2 batch design section 3.3; ARCH section 7.3;
// qiven-context runtime/invocation-policy.yaml, schema
// qiven-invocation-policy-v1)
//
// The runtime parses EXACTLY the strict subset this header names — a
// bounded line parser (cpp-design D-4: the runtime never gains a general
// YAML parser). Block style only, comments as whole lines, one nesting
// level under `policy`, `resolvers`, `freshness`, `enforcement`; list
// rows are `- key: value` blocks. Anything else — unknown keys, unknown
// enum vocabulary, flow style, tabs, deeper nesting, oversize input —
// fails closed with a typed error naming the line.
//
// Enum vocabularies are the frozen v4 draft names (qiven-context-draft
// pin 7583724); the conversion tables live in the implementation and are
// the single place a string becomes a draft enum value.
// ============================================================================

#include <qiven/result.hpp>
#include <qiven/types.hpp>

#include <qiven/context/cognition.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace qiven::runtime::cognition
{
inline constexpr usize max_policy_bytes = 256 * 1024;
inline constexpr usize max_rules        = 512;

inline constexpr i32 err_policy_malformed = 64; // frame/protocol class (cpp-design section 5)

struct PolicyParseError
{
    i32 code   = err_policy_malformed;
    usize line = 0;
    std::string detail;
};

struct ParsedResolver
{
    qiven::context::RequirementKind kind = qiven::context::RequirementKind::MandatoryRecall;
    std::string type;
    u64 min_version = 1;
};

struct ParsedPolicy
{
    qiven::context::InvocationPolicy invocation; // present=true with the parsed rules
    std::vector<ParsedResolver> resolvers;
    u64 evidence_ttl_ms     = 0;
    u64 bundle_freshness_ms = 0;
    // ARCH section 8.2 default law: an unsatisfied BeforeJudgment
    // requirement re-deliberates; BeforeExecution denies. Parsed values
    // may only restate these (the file is a machine instance, not a
    // policy-authoring surface).
    bool redeliberate_on_before_judgment = true;
    bool deny_on_before_execution        = true;
};

using PolicyParseResult = qiven::Result<ParsedPolicy, PolicyParseError>;

// Parse the canonical file bytes. The happy path mirrors the frozen v4
// default_invocation_policy rows the qiven-context validator pins; this
// parser accepts the vocabulary and shape (the equality pin lives in
// qiven-context's own validation — the runtime never re-derives policy
// law, it loads the accepted instance).
[[nodiscard]] PolicyParseResult parse_invocation_policy(std::string_view bytes);
} // namespace qiven::runtime::cognition
