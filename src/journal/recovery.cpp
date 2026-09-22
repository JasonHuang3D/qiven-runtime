// ============================================================================
// journal/recovery.cpp — fail-closed inspector + crash-injection framework
// (MVP-1; production-MVP architecture §11.2, §13.2, §16.3; cpp-design §7.5;
// batch design docs/design/mvp1-journal.md §3.5-§3.6)
//
// The recovery walk itself (RuntimeJournal::recover_at) lives in
// runtime_journal.cpp with the shared SQL helpers it needs; this TU owns
// the reconciliation seam's default and the crash hooks. Production builds
// (no QIVEN_RUNTIME_TEST_CRASH_POINTS) contain zero hook state: trigger()
// is an empty inline function in the header and nothing here references
// process-global test state.
// ============================================================================

#include <qiven/runtime/journal/recovery.hpp>

#if defined(QIVEN_RUNTIME_TEST_CRASH_POINTS)
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
#endif

namespace qiven::runtime::journal
{
DispatchResolution FailClosedInspector::classify(const PendingDispatch& /*dispatch*/)
{
    // Without a real inspector the runtime cannot prove any external
    // effect, so an unresolved dispatch is a conflict plus a barrier —
    // never a guess (ARCH §11.2; batch design §3.5 step 7).
    DispatchResolution resolution;
    resolution.classification       = port::RecoveryClassification::IndeterminateConflict;
    resolution.reconstructed_status = std::nullopt;
    resolution.ref_observed         = std::nullopt;
    return resolution;
}

#if defined(QIVEN_RUNTIME_TEST_CRASH_POINTS)

namespace crash
{
namespace
{
// Process-global armed state — test builds only (production has none).
CrashPoint g_armed_point = CrashPoint::none;
CrashMode g_armed_mode   = CrashMode::none;
} // namespace

void arm(CrashPoint point, CrashMode mode) noexcept
{
    g_armed_point = point;
    g_armed_mode  = mode;
}

void trigger(CrashPoint point, Phase phase) noexcept
{
    if (g_armed_point == CrashPoint::none || g_armed_point != point)
    {
        return;
    }
    const bool fire =
        (phase == Phase::before_commit && g_armed_mode == CrashMode::die_before_commit) ||
        (phase == Phase::after_commit && g_armed_mode == CrashMode::die_after_commit);
    if (!fire)
    {
        return;
    }
    // TerminateProcess on self: no CRT death notifications, no unwinding —
    // the closest in-process approximation of power loss on Windows
    // (strictly harder than _exit; batch design §3.6).
    const u32 exit_code = crash_exit_base + static_cast<u32>(point);
    TerminateProcess(GetCurrentProcess(), exit_code);
}
} // namespace crash

#endif
} // namespace qiven::runtime::journal
