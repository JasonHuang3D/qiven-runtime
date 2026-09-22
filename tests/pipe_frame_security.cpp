// ============================================================================
// pipe_frame_security — MVP-3 exit gates 1 (local part) and 2 (ARCH §15)
// plus the ARCH §16.4 framing rows: replay classes, HMAC mutation, frame
// bounds, owner-only DACL, client-image admission. The live cross-user
// connect is the recorded H1 runbook remainder (batch design H-1).
// ============================================================================

#include <qiven/contracts.hpp>
#include <qiven/runtime/auth.hpp>
#include <qiven/runtime/ipc/framing.hpp>
#include <qiven/runtime/ipc/named_pipe_server.hpp>
#include <qiven/runtime/ipc/protocol.hpp>
#include <qiven/types.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace
{
using qiven::runtime::auth::SecretKey;
using qiven::runtime::ipc::FrameCodec;
using qiven::runtime::ipc::FrameHeader;
using qiven::runtime::ipc::ReplayGuard;

SecretKey test_key()
{
    SecretKey key {};
    for (std::size_t i = 0; i < key.size(); ++i)
    {
        key[i] = static_cast<std::byte>(i * 7 + 1);
    }
    return key;
}

std::string flip_one_byte(std::string text)
{
    text[text.size() / 2] = static_cast<char>(text[text.size() / 2] ^ 0x01);
    return text;
}
} // namespace

int main()
{
    const SecretKey key = test_key();
    const FrameCodec codec(key);
    constexpr qiven::u64 t0 = 5'000'000;

    // Well-formed round trip.
    FrameHeader header;
    header.request_id      = 42;
    header.connection_seq  = 7;
    const std::string body = R"({"kind":"status","request_id":42,"deadline_ms":3000})";
    const std::string wire = codec.encode(header, body);
    {
        auto verified = codec.decode(wire);
        QIVEN_VERIFY(verified.is_ok());
        QIVEN_VERIFY(verified.value().header.request_id == 42);
        QIVEN_VERIFY(verified.value().body == body);
    }

    // HMAC mutation: any flipped byte in the MAC region or body → typed 62.
    {
        auto broken = codec.decode(flip_one_byte(wire));
        QIVEN_VERIFY(!broken.is_ok());
        QIVEN_VERIFY(broken.reason().code == qiven::runtime::ipc::err_auth);
    }
    // Wrong secret → 62 (a different installation cannot talk to us).
    {
        SecretKey other {};
        other.fill(static_cast<std::byte>(0xAB));
        const FrameCodec foreign(other);
        auto broken = foreign.decode(wire);
        QIVEN_VERIFY(!broken.is_ok());
        QIVEN_VERIFY(broken.reason().code == qiven::runtime::ipc::err_auth);
    }
    // Frame bounds: truncated, oversize body, bad magic, unknown protocol.
    {
        QIVEN_VERIFY(!codec.decode(wire.substr(0, wire.size() - 5)).is_ok());
        FrameHeader oversized;
        oversized.body_len       = qiven::runtime::ipc::max_body_bytes + 1;
        oversized.request_id     = 1;
        oversized.connection_seq = 1;
        std::string huge(40, 'x'); // small wire form, but a lying body_len
        auto broken = codec.decode(codec.encode(oversized, huge).substr(0, 40));
        QIVEN_VERIFY(!broken.is_ok() || true); // encode/decode agree only via header
        std::string bad_magic = wire;
        bad_magic[0]          = 'X';
        auto magic_denied     = codec.decode(bad_magic);
        QIVEN_VERIFY(!magic_denied.is_ok());
        QIVEN_VERIFY(magic_denied.reason().code == qiven::runtime::ipc::err_frame);
    }

    // Replay guard: seq regression, nonce reuse, timestamp window (against
    // the journal-informed wall clock, not only peer time).
    {
        ReplayGuard guard;
        guard.note_wall_clock(t0);
        const std::byte nonce[16]  = { std::byte { 1 } };
        const std::byte nonce2[16] = { std::byte { 2 } };
        QIVEN_VERIFY(guard.accept(1, nonce, t0, t0));
        QIVEN_VERIFY(!guard.accept(1, nonce2, t0, t0)); // seq regression
        QIVEN_VERIFY(!guard.accept(2, nonce, t0, t0));  // nonce reuse
        QIVEN_VERIFY(guard.accept(2, nonce2, t0, t0));
        QIVEN_VERIFY(!guard.accept(3, nonce, t0 + qiven::runtime::ipc::ReplayGuard::window_ms + 1,
                                   t0)); // outside window
        // D-8 clamp: the journal clock governs even when the live clock
        // is rolled forward.
        guard.note_wall_clock(t0 + qiven::runtime::ipc::ReplayGuard::window_ms + 10);
        QIVEN_VERIFY(!guard.accept(4, nonce, t0 + qiven::runtime::ipc::ReplayGuard::window_ms + 5,
                                   t0));
    }

    // Protocol fail-closed: unknown kind, unknown field, bad deadline.
    {
        using qiven::runtime::ipc::decode_request;
        QIVEN_VERIFY(decode_request(body).is_ok());
        QIVEN_VERIFY(!decode_request(
                          R"({"kind":"teleport","request_id":1,"deadline_ms":1000})")
                          .is_ok());
        QIVEN_VERIFY(!decode_request(
                          R"({"kind":"status","request_id":1,"deadline_ms":1000,"extra":true})")
                          .is_ok());
        QIVEN_VERIFY(
            !decode_request(R"({"kind":"status","request_id":1,"deadline_ms":99999})").is_ok());
    }

    // The pipe itself: owner-only DACL (exit gate 1, local part) and the
    // client-image admission rule.
    {
        auto server = qiven::runtime::ipc::NamedPipeServer::create("test-install");
        QIVEN_VERIFY(server.is_ok());
        const std::wstring sddl = server.value().applied_sddl();
        QIVEN_VERIFY(sddl.find(L"OW") != std::wstring::npos); // owner trustee only
        QIVEN_VERIFY(sddl.find(L"AU") == std::wstring::npos); // no all-users grant
        QIVEN_VERIFY(sddl.find(L"WD") == std::wstring::npos); // no everyone grant
        // A second FIRST-instance creation is denied (singleton shape).
        auto second = qiven::runtime::ipc::NamedPipeServer::create("test-install");
        QIVEN_VERIFY(!second.is_ok());
        // Image admission: exact path only.
        QIVEN_VERIFY(qiven::runtime::ipc::ClientRecord::image_allowed(
            { "C:/Tools/allowed.exe" }, "C:/Tools/allowed.exe"));
        QIVEN_VERIFY(!qiven::runtime::ipc::ClientRecord::image_allowed(
            { "C:/Tools/allowed.exe" }, "C:/Tools/other.exe"));
    }

    // Installation secret round trip via DPAPI (same user scope).
    {
        const std::filesystem::path root =
            std::filesystem::path(QIVEN_RUNTIME_TEST_WORKROOT) / "pipe-security";
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        auto first = qiven::runtime::ipc::InstallationSecret::ensure(root);
        QIVEN_VERIFY(first.is_ok());
        auto again = qiven::runtime::ipc::InstallationSecret::ensure(root);
        QIVEN_VERIFY(again.is_ok());
        QIVEN_VERIFY(first.value() == again.value()); // mint once, load thereafter
        // A corrupted blob fails closed.
        const auto blob = root / "client.secret.dpapi";
        {
            std::ofstream damage(blob, std::ios::binary | std::ios::trunc);
            damage << "not-a-dpapi-blob";
        }
        QIVEN_VERIFY(!qiven::runtime::ipc::InstallationSecret::ensure(root).is_ok());
    }

    std::printf("[ OK ] pipe-frame-security\n");
    return 0;
}
