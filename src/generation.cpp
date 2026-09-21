#include <qiven/runtime/generation.hpp>

#include <utility>

namespace qiven::runtime
{
RuntimeGeneration GenerationMinter::mint(profile::DeploymentProfile profile,
                                         std::vector<adapter::AdapterManifest> admitted_adapters,
                                         qiven::context::Snapshot minimum_cognition)
{
    // The only construction path: a fresh monotonic id every mint. There
    // are no setters on RuntimeGeneration — a changed profile reaches the
    // host through mint() again, never through mutation of a live
    // generation (ADL §6, C-09).
    RuntimeGeneration generation;
    generation.id                = RuntimeGenerationId { m_next_value++ };
    generation.profile           = std::move(profile);
    generation.admitted_adapters = std::move(admitted_adapters);
    generation.minimum_cognition = std::move(minimum_cognition);
    return generation;
}
} // namespace qiven::runtime
