#pragma once

// ============================================================================
// cognition/task_descriptor.hpp — CA-1 task normalization (design §1;
// acceptance protocol §7.1 descriptor law)
//
// ONE fixed, condition-blind normalizer: derives a typed TaskDescriptor
// from a neutral task envelope (objective, repository, revision, changed
// paths, declared phase, declared risk). Judgment-bearing fields stay
// EMPTY unless mechanically derivable: path-prefix → subsystem/boundary
// mapping, file-extension → language mapping, textual IDs from the
// objective. No curator enrichment path exists in code (Profile C's
// pairing rule); observed vs claimed provenance is kept distinct (the
// envelope marks which facts were mechanically observed).
// ============================================================================

#include <qiven/runtime/cognition/activation_policy.hpp>
#include <qiven/types.hpp>

#include <string>
#include <vector>

namespace qiven::runtime::cognition
{
enum class TaskPhase : u8
{
    Specify        = 0,
    Design         = 1,
    Implementation = 2,
    Review         = 3,
    Acceptance     = 4,
};

enum class TaskRisk : u8
{
    R0 = 0,
    R1 = 1,
    R2 = 2,
    R3 = 3,
};

// The neutral envelope both acceptance conditions receive (§7.1).
struct TaskEnvelope
{
    std::string objective;  // task statement verbatim
    std::string repository; // primary repository name
    std::string revision;   // declared revision
    std::vector<std::string> changed_paths;
    TaskPhase phase     = TaskPhase::Design;
    TaskRisk risk       = TaskRisk::R2;
    bool phase_observed = false; // mechanically observed vs participant-claimed
    bool risk_observed  = false;
};

// The typed descriptor protected selection evaluates (selector-schema v1
// task facts). Fields a fixed normalizer cannot derive stay empty.
struct TaskDescriptor
{
    std::string objective;
    std::string repository;
    std::string revision;
    TaskPhase phase     = TaskPhase::Design;
    TaskRisk risk       = TaskRisk::R2;
    bool phase_observed = false;
    bool risk_observed  = false;
    std::vector<std::string> changed_paths;
    std::vector<std::string> path_prefixes; // leading path components
    std::vector<std::string> languages;     // extension-mapped, schema-v1 values
    std::vector<std::string> explicit_ids;  // ADR-00NN / OBL-… / MEM-… found in the objective
    // Judgment-bearing: EMPTY unless mechanically derived (stays empty in v1)
    std::vector<std::string> boundary_kinds;

    // Deterministic canonical JSON (sorted keys, no whitespace): the
    // task digest input — byte-identical envelope ⇒ byte-identical
    // descriptor ⇒ byte-identical digest.
    [[nodiscard]] std::string canonical_json() const;
    [[nodiscard]] std::string digest_hex() const;
};

// Extension → schema-v1 language mapping (exact vocabulary).
[[nodiscard]] std::optional<std::string> language_of_extension(std::string_view path) noexcept;

// Textual ID extraction from free text: ADR-00NN, OBL-*, MEM-* tokens.
[[nodiscard]] std::vector<std::string> extract_explicit_ids(std::string_view text);

// The fixed normalizer. Deterministic; never invents judgment fields.
[[nodiscard]] TaskDescriptor normalize_task(const TaskEnvelope& envelope);

[[nodiscard]] std::string_view phase_text(TaskPhase phase) noexcept;
[[nodiscard]] std::string_view risk_text(TaskRisk risk) noexcept;
} // namespace qiven::runtime::cognition
