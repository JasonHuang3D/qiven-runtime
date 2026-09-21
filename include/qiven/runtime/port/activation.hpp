#pragma once

// ============================================================================
// port/activation.hpp — activation boundary (component ADL §13/§66)
//
// Activation events (session start, prompt submission, task/workflow
// transition, explicit declared intent) are an OPTIMIZATION and
// PREPARATION opportunity: they may preload cognition and mint trusted
// activation receipts. Correctness NEVER depends on activation predicting
// every later action — when a requirement becomes knowable only at
// proposal time, the action boundary (interception) stays authoritative.
//
// An activation receipt (§66) proves cognition was injected before a
// specific proposal: subject, canonical revision, actor/session,
// injection event identity.
// ============================================================================

#include <qiven/runtime/identity.hpp>

#include <qiven/context/cognition.hpp>

#include <string>

namespace qiven::runtime::port
{
enum class ActivationKind : u8
{
    SessionStart,
    PromptSubmission,
    TaskTransition,
    WorkflowTransition,
    DeclaredIntent,
};

struct ActivationEvent
{
    ActivationKind kind = ActivationKind::SessionStart;
    std::string detail;
};

// §66: cognition injected at activation may satisfy a later requirement
// only through such a receipt — preload without proof is not evidence.
struct ActivationReceipt
{
    std::string subject;
    qiven::context::RevisionId canonical_revision;
    u64 actor_token     = 0;
    u64 session_token   = 0;
    u64 injection_event = 0; // identity of the activation event

    [[nodiscard]] bool complete() const noexcept
    {
        return !subject.empty() && !canonical_revision.value.empty() && actor_token != 0 && session_token != 0 &&
               injection_event != 0;
    }
};

class IActivationBoundaryPort
{
public:
    virtual ~IActivationBoundaryPort() = default;

    // Record an activation; returns the injection-event identity the
    // receipt must cite (0 = activation not tracked).
    [[nodiscard]] virtual u64 notify_activation(const ActivationEvent& event) = 0;
};
} // namespace qiven::runtime::port
