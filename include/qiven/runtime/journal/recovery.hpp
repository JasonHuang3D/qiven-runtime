#pragma once

// ============================================================================
// journal/recovery.hpp — recovery skeleton + reconciliation seam +
// crash-injection framework (MVP-1)
// (production-MVP architecture §11.2, §13.2, §16.3; cpp-design §7.5;
// batch design docs/design/mvp1-journal.md §3.5-§3.6)
//
// Recovery re-inspects authoritative state instead of retrying blindly
// (ARCH §11.2). MVP-1 has no external effects yet, so authoritative
// inspection IS the journal; the IReconciliationInspector seam is where
// MVP-6 installs real Git ref/object inspection. The default inspector is
// fail-closed: unknown external state → IndeterminateConflict + barrier —
// never a guess ("The process restarted" is not a recovery assertion).
//
// The crash-injection framework compiles to NOTHING in production builds
// (cpp-design §7.5): without QIVEN_RUNTIME_TEST_CRASH_POINTS every
// trigger() is an empty inline function. The self-spawning crash test
// compiles src/journal/*.cpp with the macro defined so the armed hooks
// live inside the process that dies (batch design §3.6).
// ============================================================================

#include <qiven/runtime/identity.hpp>
#include <qiven/runtime/journal/schema.hpp>
#include <qiven/runtime/port/runtime_journal.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace qiven::runtime::journal
{
// One unresolved dispatch as recovery sees it (state ∈ prepared/dispatched
// with no outcome row — ARCH §13.2 step 6).
struct PendingDispatch
{
    SortableId128 id {};
    ControlTransactionId transaction {};
    ContentDigest plan_digest {};
    std::string state;
};

// The inspector's verdict for one pending dispatch (ARCH §11.2 classes).
struct DispatchResolution
{
    port::RecoveryClassification classification =
        port::RecoveryClassification::IndeterminateConflict;
    std::optional<std::string> reconstructed_status; // classification Reconstructed
    std::optional<std::string> ref_observed;         // evidence carried into the outcome
};

// MVP-6 seam: classify a pending dispatch by inspecting AUTHORITATIVE
// state (Git refs/objects per the dispatch plan). Implementations must be
// deterministic and side-effect-free.
class IReconciliationInspector
{
public:
    virtual ~IReconciliationInspector()                                                = default;
    [[nodiscard]] virtual DispatchResolution classify(const PendingDispatch& dispatch) = 0;
};

// The default: fail-closed. Without a real inspector the runtime cannot
// prove any external effect, so an unresolved dispatch is
// IndeterminateConflict plus a barrier (batch design §3.5 step 7).
class FailClosedInspector final : public IReconciliationInspector
{
public:
    [[nodiscard]] DispatchResolution classify(const PendingDispatch& dispatch) override;
};

// ---------------------------------------------------------------------------
// Crash-injection framework (test-guarded; cpp-design §7.5)
// ---------------------------------------------------------------------------
namespace crash
{
// The full ARCH §16.3 point set. The six journal-transaction points plus
// after_outcome_commit have live trigger sites in MVP-1; the seven
// external-effect points are declared but unarmed until their operations
// exist (MVP-5/6). Arming an unarmed point never silently passes: the
// child completes without dying and the driving test fails.
enum class CrashPoint : u8
{
    none = 0,
    after_create_tx,         // §16.3 point 1 (journal live)
    after_evidence_persist,  // point 2 (live)
    after_decision_persist,  // point 3 (live)
    after_token_consume,     // point 4 (live)
    after_lease_fence,       // point 5 (live)
    after_dispatch_commit,   // point 6 (live)
    after_candidate_tree,    // point 7 (unarmed until MVP-5)
    after_validator,         // point 8 (unarmed until MVP-5)
    after_commit_object,     // point 9 (unarmed until MVP-5)
    after_ref_cas,           // point 10 (unarmed until MVP-5)
    after_outcome_commit,    // point 12 (live)
    after_ack_persist,       // point 11/12 IPC half (unarmed until MVP-3)
    after_projection_update, // point 13 (unarmed until MVP-6)
};

enum class CrashMode : u8
{
    none,
    die_before_commit, // mutations staged, COMMIT not yet issued → rollback on death
    die_after_commit,  // COMMIT durable, command not yet returned → effect persists
};

enum class Phase : u8
{
    before_commit,
    after_commit,
};

// The child exit code carries the crash-point index so the driving test
// proves WHERE the process died, not merely that it died.
inline constexpr u32 crash_exit_base = 0xE0000000u;

#if defined(QIVEN_RUNTIME_TEST_CRASH_POINTS)

// Arm one point+mode (process-global, test-build-only state).
void arm(CrashPoint point, CrashMode mode) noexcept;

// Called at every trigger site; dies (TerminateProcess — no CRT death
// notifications, the closest in-process approximation of power loss) when
// the call matches the armed point and phase. Never returns in that case;
// always returns when unarmed or non-matching.
void trigger(CrashPoint point, Phase phase) noexcept;

#else

inline void trigger(CrashPoint /*point*/, Phase /*phase*/) noexcept
{
    // Production build: fully inert, optimized away.
}

#endif
} // namespace crash
} // namespace qiven::runtime::journal
