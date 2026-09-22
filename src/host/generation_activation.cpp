#include <qiven/runtime/host/generation_activation.hpp>

namespace qiven::runtime::host
{
qiven::Result<RuntimeGenerationId> activate_generation(journal::RuntimeJournal& runtime_journal,
                                                       const ContentDigest& bundle_digest,
                                                       const ContentDigest& profile_digest,
                                                       std::string_view build_id,
                                                       u64 now_ms)
{
    auto max_id = runtime_journal.max_generation_id();
    if (!max_id.is_ok())
    {
        return qiven::Result<RuntimeGenerationId>::fail(max_id.reason());
    }
    RuntimeGenerationId id { max_id.value() + 1 };
    auto advanced = runtime_journal.advance_generation(id, bundle_digest, profile_digest, build_id,
                                                       now_ms);
    if (!advanced.is_ok())
    {
        return qiven::Result<RuntimeGenerationId>::fail(advanced.reason());
    }
    return qiven::Result<RuntimeGenerationId>(id);
}
} // namespace qiven::runtime::host
