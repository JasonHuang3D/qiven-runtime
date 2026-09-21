#include <qiven/runtime/observed_action.hpp>

#include <qiven/contracts.hpp>
#include <qiven/hashing_sha256.hpp>

#include <cstdio>
#include <span>

namespace
{
using qiven::runtime::ActorInstanceId;
using qiven::runtime::AdapterInstanceId;
using qiven::runtime::CapabilityId;
using qiven::runtime::HarnessActionId;
using qiven::runtime::HarnessSessionId;

const AdapterInstanceId adapter { qiven::fnv1a64("loopback") };
const HarnessSessionId session { 7 };
const ActorInstanceId actor { 11 };
const CapabilityId capability { 3 };
} // namespace

int main()
{
    // C-01: the observed action is adapter-reported mechanism fact — the
    // factory derives digest and blob together, so the boundary record
    // always binds the exact bytes it retains
    {
        const std::byte arguments[] { std::byte { 0x01 }, std::byte { 0x02 }, std::byte { 0x03 } };
        const auto action = qiven::runtime::observe_action(adapter, session, actor, capability, "write_file",
                                                           "docs/x.md", std::span<const std::byte>(arguments));

        QIVEN_VERIFY(action.adapter.fnv == adapter.fnv);
        QIVEN_VERIFY(action.session.value == 7);
        QIVEN_VERIFY(action.actor.value == 11);
        QIVEN_VERIFY(action.capability.value == 3);
        QIVEN_VERIFY(action.operation == "write_file");
        QIVEN_VERIFY(action.target == "docs/x.md");
        QIVEN_VERIFY(action.argument_blob.size() == 3);

        const qiven::SHA256Digest expected = qiven::sha256(arguments, sizeof arguments);
        QIVEN_VERIFY(action.argument_digest.sha256 == expected);
    }

    // different argument bytes bind different digests; identical bytes are
    // stable across observations (content identity)
    {
        const std::byte a[] { std::byte { 'a' } };
        const std::byte b[] { std::byte { 'b' } };

        const auto first =
            qiven::runtime::observe_action(adapter, session, actor, capability, "op", "t", std::span<const std::byte>(a));
        const auto second =
            qiven::runtime::observe_action(adapter, session, actor, capability, "op", "t", std::span<const std::byte>(b));
        const auto repeat =
            qiven::runtime::observe_action(adapter, session, actor, capability, "op", "t", std::span<const std::byte>(a));

        QIVEN_VERIFY(first.argument_digest != second.argument_digest);
        QIVEN_VERIFY(first.argument_digest == repeat.argument_digest);

        // empty arguments are a valid observation with the empty digest
        const auto empty = qiven::runtime::observe_action(adapter, session, actor, capability, "op", "t", {});
        QIVEN_VERIFY(empty.argument_blob.empty());
        QIVEN_VERIFY(empty.argument_digest.sha256 == qiven::sha256(static_cast<const std::byte*>(nullptr), 0));
    }

    // C-18 composition: the correlation key binds generation + adapter +
    // session + actor + one exact harness action, derived — never guessed —
    // from the observed action
    {
        const qiven::runtime::RuntimeGenerationId generation { 4 };
        const HarnessActionId harness_action { 9001 };

        const std::byte arguments[] { std::byte { 0xFF } };
        const auto action = qiven::runtime::observe_action(adapter, session, actor, capability, "read", "state.bin",
                                                           std::span<const std::byte>(arguments));

        const auto key = qiven::runtime::correlation_key(generation, action, harness_action);
        QIVEN_VERIFY(key.generation.value == 4);
        QIVEN_VERIFY(key.adapter.fnv == adapter.fnv);
        QIVEN_VERIFY(key.session.value == 7);
        QIVEN_VERIFY(key.actor.value == 11);
        QIVEN_VERIFY(key.action.value == 9001);

        // same observed action + same harness action => identical key,
        // stable across recomputation (pre-action through post-action)
        const auto again = qiven::runtime::correlation_key(generation, action, harness_action);
        QIVEN_VERIFY(key == again);
    }

    std::printf("[ OK ] observed-action\n");
    return 0;
}
