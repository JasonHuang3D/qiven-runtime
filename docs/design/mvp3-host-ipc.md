# MVP-3 — RuntimeHost, IPC, and the Mechanical Execution Boundary (batch design)

Status: **batch design for MVP-3** (design-first standard; extends
`docs/architecture/runtime-production-mvp-cpp-design.md` — **DESIGN** —
§4, §10, §11, §3 topology; implementing
`docs/architecture/runtime-production-mvp-architecture.md` — **ARCH** —
§6.1, §12, §13.2, §15 MVP-3, §16.4).

Batch: MVP-3 of OBL-20260922T155800Z-9A7B41. Exit gate (ARCH §15):

1. a non-owner client cannot connect;
2. replayed envelopes are rejected;
3. host timeout or disconnect maps to deny inside the hard-governed
   scope;
4. a timed-out child process and its process tree are reclaimed;
5. large stdout/stderr cannot deadlock RuntimeHost or consume unbounded
   memory.

---

## 1. Basis

- ARCH §6.1 (RuntimeHost duties, user-level, no silent exit with an
  unresolved dispatch), §12 (named pipe, owner ACL, DPAPI secret,
  nonce/timestamp/HMAC, framing bounds, identity boundary, path/child
  defenses), §13.2 (startup order; `status`/`doctor` before recovery
  completes, `HostRecovering` otherwise), §15 MVP-3 work list, §16.4
  security tests.
- DESIGN §4 (threads/lifecycle, time authority), §5 (IPC 60–69, process
  70–79, host 100–109), §10 (pipe/secret/framing/handshake), §11
  (process runner hardening), §16 test spine (`pipe_frame_security`,
  `process_runner_bounds`).
- MVP-2 landed: publisher/bundle/profile/generation activation, jsonx
  first form (IPC bodies reuse it), processx first form.

### Decisions recorded against DESIGN (deltas, not silent deviations)

1. **JSON bodies carry request/response kinds, not full ARCH §8 phase
   traffic.** MVP-3's protocol surface is `hello`, `status`, `doctor`,
   and a mutation-placeholder kind that ALWAYS returns typed
   `HostRecovering` (61) — the governed mutation pipeline (classify →
   bind → dispatch) is MVP-4/5/6 work. The framing/security envelope
   and the fail-closed deny mapping are complete and tested NOW; the
   phase kinds join in their batches without framing changes.
2. **Client executable validation is install-record-based, per DESIGN
   §10.** The host records the allowed client image path set in its
   install record (`runtime_meta`) at first boot (runtimectl + the host
   itself); `GetNamedPipeClientProcessId` → `QueryFullProcessImageName`
   mismatches are typed 62 denials. A genuine cross-USER connect cannot
   be exercised on a single-user machine from the test suite — the ACL
   construction (owner-only SDDL, verified by reading the pipe's security
   descriptor) plus the design's SDDL string are the local proof; the
   live cross-user test is named for the MVP-7 owner runbook (H1 class).
3. **`status`/`doctor` are served in-process by the host and mirrored
   over IPC.** `doctor` runs journal `integrity_check` + audit-chain
   verify + ACTIVE bundle verification (bounded; DESIGN §7.1) and
   reports — it never mutates (no self-healing; quarantine clears only
   through an operator decision, MVP-1 law).

## 2. Module map

```text
include/qiven/runtime/
  ipc/framing.hpp        frame layout, HMAC, nonce/timestamp window,
                         replay cache (bounded LRU), size/depth caps
  ipc/protocol.hpp       envelope kinds + jsonx encode/decode + typed
                         IPC errors (61-65)
  ipc/named_pipe_server.hpp  owner-ACL pipe instance, client identity
                         validation, DPAPI secret (CryptProtectData)
  host/runtime_host.hpp  composition root + lifecycle states
src/ipc/{framing,protocol,named_pipe_server}.cpp
src/host/runtime_host.cpp
apps/runtime_host_main.cpp    the production composition entrypoint
tests/pipe_frame_security.cpp  exit gates 1(local part)/2 + §16.4 rows
tests/process_runner_bounds.cpp exit gates 4/5 + allowlist/env rows
tests/host_lifecycle.cpp       startup order, HostRecovering, status/doctor
tests/headers/ipc_*.cpp, headers/runtime_host.cpp
```

Changed: `processx` (Job Object kill-on-close always; `env_allowlist`;
`allowed_executables` path+SHA-256 preflight → typed 74), `apps/
runtimectl_main.cpp` (+ `status`, `doctor` via the pipe client in
`ipc/protocol.hpp`), `qiven-runtime-app` RETIRED (ARCH §14.2 row 1:
the bootstrap printer is superseded by the RuntimeHost composition
root; CMake target, deploy products and smoke updated), root
`CMakeLists.txt`, `.qiven/deploy.json`, `tests/CMakeLists.txt`.

Out of scope: SessionStart refresh loop / remote fetch (the host
publishes from the local checkout's `refs/heads/main` at boot; bounded
remote fetch lands with MVP-4's SessionStart wiring), hook adapter
(MVP-4), mutation pipeline (MVP-5/6), CBOR (deferred, DESIGN §19).

## 3. Contracts

### 3.1 Framing (DESIGN §10)

```cpp
namespace qiven::runtime::ipc {
// wire frame: [u32 magic 'QVR1'][u16 proto=1][u16 flags][u64 body_len]
//             [u64 request_id][u64 connection_seq][32B hmac][body]
struct FrameHeader { u32 magic; u16 proto; u16 flags; u64 body_len;
                     u64 request_id; u64 connection_seq; };
inline constexpr u32 frame_magic = 0x51565231; // "QVR1"
inline constexpr u64 max_body_bytes = 1 * 1024 * 1024;
inline constexpr u64 max_frame_bytes = max_body_bytes + 64;

struct VerifiedFrame { FrameHeader header; std::string body; };

class FrameCodec {           // secret injected; stateless encode
public:
    explicit FrameCodec(SecretKey key);
    [[nodiscard]] std::string encode(const FrameHeader&, std::string_view body) const;
    // verify: magic/proto/body cap/HMAC over header+body; typed 62/63/64
    [[nodiscard]] qiven::Result<VerifiedFrame> decode(std::string_view bytes) const;
};

class ReplayGuard {          // per-connection: seq strictly increasing,
public:                      // nonce LRU of the last 256, ±120 s window
    void note_wall_clock(u64 now_ms);          // D-8 clamp: window checked
    [[nodiscard]] bool accept(u64 connection_seq, std::span<const std::byte> nonce,
                              u64 timestamp_ms, u64 now_ms);
};
}
```

The nonce is carried in the first 16 body bytes of `hello` (CSPRNG,
client-side); `ReplayGuard` binds (client id, nonce). HMAC covers the
full header (excluding the HMAC field) + body with the DPAPI-protected
256-bit secret. Timestamps validate against the JOURNAL wall clock
(`ReplayGuard::note_wall_clock` at boot and per request), not only peer
time (DESIGN review note 3).

### 3.2 Protocol (kinds, bounds, typed errors)

`hello {client_kind, client_build, deadline_ms}` → `hello_ack {host_build,
install_id, boot_epoch}`; `status {}` → `status {state, install_id,
boot_epoch, generation, bundle_revision, journal_events, quarantined}`;
`doctor {}` → `doctor {integrity, audit_chain, bundle_active,
findings[]}`; `mutation` (any payload) → typed error 61 while state !=
Running (MVP-3 always). Errors: `{"error":{"code":61..65,"detail":...}}`.
Encode/decode via jsonx (u64/string/bool/array-of-strings only); unknown
kind, unknown field → typed 64 (fail closed, ARCH §12.1). Deadline: each
request carries `deadline_ms` (≤ 5000); the host answers or returns the
typed 65 denial BEFORE the deadline (hook mapping, DESIGN §10).

### 3.3 Named-pipe server (DESIGN §10/§12.3)

`\\.\pipe\qiven-runtime-<install-id>-v1` — a FLAT single-segment name.
**Recorded delta on ARCH §12.1's `\\.\pipe\qiven-runtime\<install-id>\v1`:**
NPFS prunes the intermediate namespace directories of a multi-segment pipe
name once no listening instance exists, so next-instance creation at
accept() time fails ERROR_PATH_NOT_FOUND (found live by the
host-lifecycle test, 2026-09-23). The flat name preserves every security
property: the pipe NAME is not the security boundary — the owner-only
DACL is. `FILE_FLAG_FIRST_PIPE_INSTANCE | PIPE_REJECT_REMOTE_CLIENTS` on
the first instance; DACL from SDDL `D:P(A;;GA;;;OW)` (owner-only
generic-all; the SDDL literal is pinned by test). First connection:
`GetNamedPipeClientProcessId` → `OpenProcess` →
`QueryFullProcessImageNameW` → compare to the install record (jsonx file
`.qiven/runtime/clients.json`, digest-recorded; first boot writes the
record with the current executables and the host validates every
connect). Secret: 256-bit CSPRNG at first boot → `CryptProtectData`
CurrentUser → `.qiven/runtime/client.secret.dpapi`; loaded via
`CryptUnprotectData`. Client secret acquisition for SAME-USER tools:
`runtimectl` reads the DPAPI blob directly (same user scope — the MVP's
single-Windows-user boundary, ARCH §3.1).

### 3.4 RuntimeHost (composition root; ARCH §13.2)

```cpp
class RuntimeHost {
public:
    struct Boot { std::filesystem::path repo_root; std::string build_id; u64 now_ms(); };
    enum class State { Starting, Running, Draining, Stopped };
    [[nodiscard]] static qiven::Result<std::unique_ptr<RuntimeHost>> boot(const Boot&);
    [[nodiscard]] State state() const noexcept;
    [[nodiscard]] StatusSnapshot status() const;      // served pre/post recovery
    [[nodiscard]] DoctorReport doctor() const;        // bounded; read-only
    // IPC mutation surface: while Starting → typed 61 (HostRecovering)
    [[nodiscard]] qiven::Result<protocol::Reply, protocol::Error> handle(const protocol::Request&);
    void request_shutdown();                          // drains; no dispatch exists yet
    ~RuntimeHost();                                   // checkpoint_wal + release
};
```

Boot order (each typed; ARCH §13.2 journal+bundle scope for MVP-3):
process mutex `Local\qiven-runtime-<install-id>` (101 on held) →
journal open (`CreateNew` first boot / `OpenExisting`) + `recover_at` →
install record + DPAPI secret ensure → profile accept (shipped
instance; scoped to `--root`'s checkout) → publisher publish from
`refs/heads/main` (local refs only) → bundle `load_active` +
`pin_from_bundle` → `activate_generation` → State::Running. Failure at
any step: typed denial, host stays `Starting`, `status`/`doctor`
remain servable, mutations deny (fail-closed). Singleton: mutex held
for the host lifetime; a second instance fails typed 101.

Shutdown: stop accepting → drain → `checkpoint_wal()` → State::Stopped.
No dispatch exists in MVP-3, so "never exit with an unresolved
dispatch" is vacuously satisfied and the drain is bounded by the
request deadline.

### 3.5 processx hardening (DESIGN §11)

- `CreateProcessW` + `PROC_THREAD_ATTRIBUTE_JOB_LIST`: Job Object with
  `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` — the handle closes on every
  path including deadline kill → TREE reclamation (exit gate 4).
- `ProcessSpec::env_allowlist: std::vector<std::pair<std::string,std::string>>`
  — the COMPLETE child environment (nothing inherited; SystemRoot/
  SystemDrive always injected by the runner as floor).
- `ProcessSpec::allowed_executables: std::vector<AllowlistedExecutable>`
  (`{path, expected_sha256_hex}`) — preflight SHA-256 of the target
  image; mismatch/absent entry → typed 74 (exit-gate row: git/validator
  executable replacement, §16.4).
- Output caps + deadline ladder unchanged (MVP-2 first form, proven).

### 3.6 runtimectl extensions

`status [--root]`: DPAPI secret → connect pipe → hello → status →
human render (exit 1 typed when the host is unreachable — an
uninstalled/stopped host is never reported as governed, ARCH §15
MVP-4 row 5 discipline applied early). `doctor [--root]`: same path,
renders findings; both honor `deadline_ms` 3000.

### 3.7 Error codes (DESIGN §5)

60–69 IPC: 61 host-recovering, 62 auth (HMAC/identity), 63 replay,
64 frame/protocol, 65 deadline. 100–109 host: 101 singleton, 102
generation, 103 quarantine.

## 4. Concurrency and lifecycle

Host runs its control logic on ONE thread (GR-4); the pipe server uses
overlapped IO with a single accept + per-connection servicing on the
control thread through a bounded ingress (existing pattern); reader
threads exist only inside `ProcessRunner` calls (joined per call). The
process mutex is OS-held (abandoned-mutex takeover is safe: the journal
boot epoch + stale-marking make the recovery path idempotent — MVP-1
law). No background timers: freshness is evaluated at request time.

## 5. Failure modes (fail-closed)

| Failure class | Behavior |
| --- | --- |
| pipe creation denied (exists) | typed 101 (second host) — mutex fires first in practice |
| HMAC mutation / wrong secret | typed 62; connection dropped |
| replayed (seq/nonce/timestamp) envelope | typed 63; connection dropped |
| oversize body / unknown kind / unknown field / bad UTF-8 | typed 64 |
| request past deadline | typed 65 answer before the deadline |
| mutation kind while Starting | typed 61 (HostRecovering) |
| client image not in install record | typed 62 |
| executable preflight mismatch | typed 74; no spawn |
| child tree survives deadline | impossible by Job Object; if the job call fails → typed 71 spawn-failure (fail closed, no run) |
| DPAPI blob unreadable | typed 62 path: host re-mints secret + records audit (first-boot semantics; existing clients fail closed until re-provisioned — single-user MVP accepts this) |
| journal/bundle failure at boot | host stays Starting; status/doctor serve the failure detail; mutations deny |

## 6. Test spine (each row names its exit-gate proof)

| Test | Proves (ARCH §15 MVP-3 / §16.4) |
| --- | --- |
| `pipe_frame_security` | **gate 2**: seq regression, nonce reuse, timestamp outside ±120 s (clamped to journal wall clock), HMAC bit-flip (62/63/64 each); oversize body → 64; **gate 1 (local part)**: pipe DACL is owner-only (security-descriptor readback asserts the SDDL shape) + client image mismatch → 62; the live cross-user connect is the recorded H1 runbook item |
| `process_runner_bounds` | **gate 4**: `cmd /c ping -n 30` tree killed at the deadline (elapsed ≪ child runtime, no surviving ping); **gate 5**: >4 MiB flood → typed 73 (bounded memory, no deadlock); executable preflight (wrong digest → 74); env allowlist (child `set` output contains ONLY allowed vars) |
| `host_lifecycle` | boot order against a real workroot journal + a published bundle fixture: second host → 101; mutation kind → 61 before Running; status transitions Starting→Running; doctor reports chain+bundle verify; shutdown checkpoints (journal reopens clean) |

## 7. Dependencies and deployment

No new third-party slots. Deployment: `qiven-runtime-app.exe` leaves the
bundle (retired); `qiven-runtime-host.exe` + `qiven-runtimectl.exe`
join; smoke = `qiven-runtime-host.exe --help` (exit 0) +
`qiven-runtimectl.exe status --root <smoke>` (exit 1, typed unreachable
— proving the honesty row). Runbook pointer (§17.3 coverage) lands in
the bundle README extras.

## 8. Deferrals (falsifiable, constitution §15)

| # | Deferral | Failure signal / revisit trigger |
| --- | --- | --- |
| H-1 | live cross-user pipe connect test | MVP-7 owner runbook execution (H1 class; single-user MVP cannot exercise it) |
| H-2 | bounded remote fetch + freshness window enforcement at SessionStart | MVP-4 SessionStart wiring (publisher API already takes refs; freshness fields parsed since MVP-2) |
| H-3 | connection-thread pool >1 / concurrent clients | measured contention from real hook traffic (MVP-4+) |
| H-4 | `shutdown` request kind over IPC | MVP-4 (the hook lifecycle needs it; MVP-3 host is console-scoped) |
| H-5 | DPAPI secret rotation | first evidence of blob migration pain across OS user profile changes |

## 9. Review record

Self-review 2026-09-23 (v19 session, designation
`jason-extended-cognition`), performed against the design ALONE, before
any implementation code existed in the branch:

1. Checked every exit-gate row 1–5 against a named test (§6) — gate 1
   split into its locally-provable part (DACL + identity validation)
   and the recorded H1 live-test remainder (H-1); no row is silently
   claimed.
2. Reconciled the mutation-surface scope: ARCH's MVP-3 list asks for
   request/response schemas and the host, not the governed mutation
   pipeline; §1.1 records the 61-always placeholder so MVP-4/5 extend
   kinds without framing changes.
3. Verified the DPAPI same-user secret distribution model against the
   single-Windows-user boundary (ARCH §3.1) — cross-user distribution
   is out of profile by construction; recorded as such, not assumed.
4. Job Object failure path checked against fail-closed: a job that
   cannot be created denies the spawn (71), never runs unmanaged.
5. Confirmed the bootstrap-app retirement does not orphan the deploy
   smoke (replacement smoke named) and that `--help` is a real exit-0
   contract, not a hang.
6. Startup order cross-checked against ARCH §13.2 rows 1, 2, 4, 8, 10,
   11, 12 (the Git-reconciliation row 7 has no dispatches to reconcile
   in MVP-3; recovery's default fail-closed inspector owns that seam —
   MVP-1 law, unchanged).
