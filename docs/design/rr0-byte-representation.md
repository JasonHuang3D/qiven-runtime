# RR-0 — Byte Representation Remediation: inventory, admission, per-format decisions

Status: DECISION ARTIFACTS (2026-09-24, v23; the consolidation
implementation is the next bounded batch inside the still-open corrective
interval — the H1 rerun gates the interval's close, not this document).
Authority: ADR-0050 roadmap amendment §4 RR-0 + §5.1; ADR-0024 admission
law; qiven-context PR #128 (CA-0).

## 1. Inventory (bound to exact revisions)

Duplicated generic byte/scalar mechanics at qiven-runtime `f0ca5b7`
(unchanged through `6ebf6ff`):

| Site | Mechanics | Call sites |
|---|---|---:|
| `src/ipc/framing.cpp:13-44` | string-based put_u32/put_u64 + get_u32/get_u64 | ~14 |
| `src/decision.cpp:12-27` | local put/get u32/u64 | ~40 |
| `src/journal/runtime_journal.cpp:116-130` | local put/get u32/u64 | ~44 |
| `src/state_store.cpp:10-26` | local put/get u32/u64 | ~12 |
| `src/adapter/manifest.cpp:12-27` | local put/get u32/u64 | ~9 |
| `src/identity.cpp:15` | put_u64_le into a fixed array | 1 |
| `src/scope.cpp`, `src/resolver.cpp`, `src/ipc/named_pipe_server.cpp:70-74` | one-off shift loops | ~5 |

Total ≈ 120 call sites, ~11 of which are explicit length-prefix writes.
Docs already ASSUMED a shared writer (`docs/architecture/runtime-cpp-design.md`
references ByteWriter; it never landed in runtime). Foundation at `79e6789`
owns: `ByteWriter` (fixed-capacity span cursor, reserve-based),
`ByteCursor`, `endian.hpp` (encode/decode LE/BE u16/u32/u64,
bool/optional-returning), `checked_span`, `Result<void>` — and NO owning
growing builder (a deliberate, documented gap).

## 2. ADR-0024 admission decision (the RR-0 question)

**Admitted to Foundation: `ByteBuilder` — an owning, growing, bounded byte
accumulator with scalar-put composition.** Rationale against the admission
law (semantic ownership, not consumer count):

- semantics are storage + bounds + ownership only: append bytes, append
  fixed-width LE/BE scalars, extract the accumulated span, explicit
  failure channel on growth/allocation failure (optional/Result, no
  exceptions) — no field order, no framing, no versioning;
- the capability is stable and product-free; every consumer composes it
  with ITS format semantics staying local;
- evidence: SIX independent local re-implementations with ~120 sites in
  one repository (consumer count is supporting evidence, never the
  threshold — ADR-0024);
- failure signals (admission law): growth failure returns a typed
  failure; capacity is bounded by an explicit max; no hidden copies.

**NOT admitted (stays with each format owner in Runtime):** framing,
message structure, field order, length-prefix WIDTH, compatibility, and
versioning. The u32-vs-u64 prefix choice is a per-format representation
contract (§3).

**Reused as-is (no new primitive):** `endian.hpp` codecs, `ByteCursor`
(read side), `checked_span`. `ByteWriter` (fixed-capacity) remains for
stack-buffer cases; `ByteBuilder` is the owning/growing complement.

## 3. Per-format prefix-width decisions (never silently unified)

| Format | Length prefix | Decision |
|---|---|---|
| IPC frame (`ipc/framing`) | u64 body_len | KEEP u64 (wire stability; version-gated already) |
| Journal records (`journal/runtime_journal`) | as-is per row kind | KEEP existing widths; byte-compat golden test per kind |
| Decision/state store serialization | as-is | KEEP existing widths; golden tests |
| Adapter manifest | as-is | KEEP existing widths; golden tests |

Rule: consolidation REPLACES the mechanics (local put/get →
foundation codecs + ByteBuilder), NEVER the representation. Every format
gets a byte-compat golden fixture captured at the pre-consolidation head
and asserted identical at the post-consolidation head.

## 4. Implementation batch (next, bounded — the interval stays open)

1. Foundation: `include/qiven/byte_builder.hpp` + tests (ownership,
   growth failure channel, bounds, scalar-put correctness vs endian.hpp
   golden vectors, move semantics); admission record
   `docs/design/byte-builder-admission.md`; capability surface entry.
2. Runtime: golden byte fixtures for every inventoried format (captured
   at `6ebf6ff`); replace the six local mechanic definitions with
   foundation codecs + ByteBuilder; keep each format's layout untouched;
   delete the dead local helpers.
3. Exit gate mapping (roadmap RR-0): every inventoried duplicate
   consolidated or retained by explicit semantic-owner decision (this
   document §2/§3 are those decisions); byte-compat proven by goldens;
   old defective paths fail regression tests (the dangling-span and
   wrong-width classes); RR-0 evidence reported independently of Profile
   A/B/utility (no evidence contamination — roadmap §1/R1).
4. Zero-cost/performance claims, if any, get the one-time assembly
   comparison the roadmap requires; otherwise none are made.

## 5. Honesty notes

- This document is the DECISION core of RR-0 landed by the v23 session;
  the mechanical consolidation (step 4.1-4.2) is explicitly the next
  bounded batch, tracked by the program obligation — the corrective
  interval cannot close before the real H1 rerun regardless (owner
  hands), so no sequencing claim is weakened by this split.
- RR-0 evidence counts toward NEITHER Profile A/B nor Cognitive Utility
  (evidential independence, ADR-0050).
