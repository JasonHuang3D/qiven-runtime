#pragma once

// ============================================================================
// host/generation_activation.hpp — journal-backed RuntimeGeneration
// activation (MVP-2 batch design section 3.6; ARCH section 7.4)
//
// A bundle, profile, or governing build change creates a NEW generation;
// never an in-place edit. Activation is one journal command
// (advance_generation) extended in MVP-2 to atomically stale-mark every
// UNCONSUMED decision bound under an older generation (ARCH §15 MVP-2
// exit gate 4): the old tokens can no longer be consumed, consumed
// decisions are untouched, and already-dispatched actions stay attached
// to their original generation.
//
// The caller (RuntimeHost, MVP-3) owns WHEN activation happens; this is
// the composition step between a verified bundle + accepted profile and
// a generation the session pins.
// ============================================================================

#include <qiven/result.hpp>
#include <qiven/runtime/identity.hpp>
#include <qiven/runtime/journal/runtime_journal.hpp>

#include <string_view>

namespace qiven::runtime::host
{
// Mints id = journal MAX(generations.id) + 1 and advances the journal.
// Errors are the journal's typed codes (51-55); 53 when the journal
// rejects the transition.
[[nodiscard]] qiven::Result<RuntimeGenerationId> activate_generation(
    journal::RuntimeJournal& runtime_journal,
    const ContentDigest& bundle_digest,
    const ContentDigest& profile_digest,
    std::string_view build_id,
    u64 now_ms);
} // namespace qiven::runtime::host
