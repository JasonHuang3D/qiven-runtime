# Fixture F-03 — Pipe service loop across reconnect cycles

Class: Windows named-pipe multi-segment lifecycle (protocol §4.2 F-03).
Sealed: 2026-09-24 (CA-0). Risk: R2/R3. Phase: design.

## Task descriptor (verbatim consumer prompt)

> Our local IPC listener serves request/response messages over a Windows
> named pipe. Review and harden the serve loop for production: clients
> connect, exchange a handshake and one or more request messages, disconnect;
> the listener must survive thousands of cycles including refused clients
> and our own restarts. Add whatever tests prove it.

Condition-blind envelope facts: repository `qiven-runtime` (`src/ipc/`,
`apps/`), language cpp, platform win32, phase design/review, risk R2→R3,
boundary kinds: ipc, platform, concurrency.

## Hidden failure surface

A loop that serves exactly one message per connection while the client
sends several on one connection (or vice versa) passes every single-shot
probe. After the first successful transaction the state machine matters:
next-instance creation after the last handle closes, ERROR_PIPE_BUSY /
ERROR_BROKEN_PIPE / ERROR_NO_DATA normalization, cancellation and shutdown
ownership, and the singleton name lifecycle.

## Protected cognition expected

- MUST-INCLUDE:
  - MEM-20260924T032000Z-C1D2E3 (untested wire contracts collapse into
    undifferentiated denials; REAL client shape over REAL transport)
  - MEM-20260923T224000Z-E5F6A7 (multi-segment pipe names break
    next-instance creation once no listener exists; flat single-segment
    names for multi-instance servers)
  - Runtime IPC architecture contract (framing/protocol docs at
    docs/architecture/, incl. connection_seq semantics)
- MUST-EXPLAIN:
  - ADR-0048 (bounded process custody where a listener process is spawned)
- MAY-RANK:
  - MEM-20260923T212000Z-D4E5F6 (Windows console/spawn law) for child
    processes; host lifecycle design records

## Semantic-owner expectations

Framing/message structure/field order/prefix width stay with the Runtime
format owner (governance program §4.1); pipe lifecycle mechanics live at the
IPC layer; platform facts need primary evidence or real captures.

## Critical findings

1. The state machine spans more than one successful transaction (multi-frame
   per connection or explicit per-connection frame count contract).
2. Connection completion and asynchronous platform results
   (broken-pipe/no-data/busy) are normalized deliberately and typed.
3. Cleanup, next-instance creation, cancellation, and shutdown have explicit
   ownership.
4. The independent test covers multiple consecutive segments AND failure
   injection — one happy path is insufficient.
5. Platform behavior claims are supported by primary evidence or a real
   boundary capture.

## Required independent evidence

Real-pipe multi-frame contract test exercising the real client sequence;
fault injection across disconnect/cancel/shutdown; fresh review of the
lifecycle ownership table.

## Prohibited shortcuts

Loopback-only mocks replacing the pipe; testing one frame and extrapolating;
silently dropping refused clients with no typed surface.

## Rubric application

§7.4 weights; focus: external/platform/concurrency assumptions (15, floor
12), failure/recovery design (10, floor 8).
