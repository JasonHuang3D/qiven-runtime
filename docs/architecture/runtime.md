# Qiven Runtime Architecture

`qiven-runtime` owns shared native execution-runtime semantics above Qiven Foundation and below native products.

## 1. Dependency law

```text
OS / C++ standard library
    -> qiven-foundation
    -> qiven-runtime
    -> native products
```

Runtime depends on Foundation deliberately. It uses Foundation types, platform/compiler boundaries, contracts, and checked arithmetic/conversions rather than recreating those primitives locally.

Runtime must not depend on DCR, CAD, robotics, rendering, editor, or other product/domain semantics.

A new lower primitive is placed in Foundation only when it is intrinsically foundational. A Windows backend helper that exists only to implement Runtime internals remains private Runtime code rather than becoming public Foundation API by default.

## 2. Initial scope

The first accepted subsystem is process execution. It owns process-generation identity, child-tree lifetime, byte-stream drainage, explicit process results, and bounded termination.

The first backend is Windows. Linux/macOS support is not claimed until implemented and continuously tested.
## 3. Process-generation ownership

A process generation is one root launch plus every descendant owned by the same generation boundary.

Generation identity is never inferred from executable names or reused PIDs. On Windows the backend uses a Job Object so teardown addresses only the owned generation. The root is created suspended, assigned to ownership, then resumed so normal execution cannot race ahead of containment.

Owned native resources use deterministic RAII in the private Windows backend. Raw `HANDLE` lifetime bookkeeping must not be spread across error branches.

Runtime does not enumerate and kill generic `node.exe`, `cmd.exe`, PowerShell, Terminal, or other processes by name.

## 4. Output contract

Stdout and stderr are opaque byte streams. Runtime does not assume text encoding and does not retain complete histories.

Both pipes are drained concurrently. Each successful read increments an exact/safely-checked byte count and may deliver the transient chunk to an optional caller-provided sink.

The sink callback executes on a drain thread, receives a non-owning span valid only for the callback, must be non-blocking, and must not retain the span. Runtime must document this execution context explicitly.

Runtime itself does not own UI history, disk rotation, or product log retention. Products that need those facilities enqueue/copy into their own explicitly bounded storage.
## 5. Launch and encoding contract

The public process API uses UTF-8 for executable/argument text. The Windows backend validates UTF-8 and converts it to UTF-16 before calling wide Win32 APIs. Invalid UTF-8 is a recoverable launch error rather than replacement-character coercion.

Windows command-line quoting follows the `CommandLineToArgvW`/MSVC-compatible backslash-and-quote rules required to preserve the caller's argument vector. Quoting is backend machinery, not a product responsibility.

Environment customization is a generic launch mechanism but is deferred until the DCR integration batch requires it. When added, it must distinguish inherited environment from explicit overrides and preserve deterministic encoding/case behavior on Windows.

## 6. Shutdown is mechanism, not product policy

Runtime exposes mechanisms such as closing child stdin, waiting with a deadline, and terminating the owned generation.

Runtime must not define `stdin EOF` as universal graceful shutdown. The caller decides which protocol action constitutes an orderly request. Runtime then provides bounded wait and exact owned-generation termination when required.

No API may wait forever during required teardown unless the caller explicitly asks for an unbounded wait.

## 7. Result and failure semantics

Launch failure, invalid input/state, wait failure, pipe-drain failure, orderly exit, timeout, and forced termination are distinct observable outcomes.
Recoverable OS/environment failures are not assertions. Programmer invariants may use Foundation contracts; Win32 failures carry a stable Runtime error category plus the native error code needed for diagnosis.

Process exit is data. Non-zero child exit is not converted into a Runtime infrastructure failure merely because it is non-zero.

## 8. Performance law

Process supervision is designed for long-running, high-output workloads. Important costs remain visible: reader threads, pipe reads, callback execution, allocations, copies, synchronization, polling/waits, and termination syscalls.

The steady-state drain path should avoid per-chunk heap allocation. Reader buffers are reused. Runtime does not serialize stdout and stderr through one lock merely for convenience when independent stream processing can avoid contention.

Counters and size conversions use Foundation checked primitives where overflow/narrowing could change semantics.

## 9. Public/private boundary

Public headers live under `include/qiven/runtime/`. Windows headers and Win32 native types stay out of portable public headers unless a deliberately Windows-specific public API is later accepted.

Private backend code lives under `src/windows/` and may include Win32 headers directly.

## 10. Initial test law

The process subsystem uses a purpose-built fixture for exact exit codes, high-rate dual-stream output, giant no-newline output, cooperative stdin EOF, hangs, and descendant spawning.

Tests must prove generation isolation, descendant containment, truthful exit reporting, concurrent drainage without pipe deadlock, bounded forced termination, repeated-generation freshness, and that unrelated processes remain untouched.
