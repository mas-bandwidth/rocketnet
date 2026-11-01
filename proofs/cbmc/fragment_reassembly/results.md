# rocketnet-cbmc-fragment-reassembly

These proofs check the fragment readers on the real sdk code
(`sdk/rocketnet_fragment.cpp`, included, never copied). The three harnesses
are in `proof_fragment_reassembly.c`. Run them with
`make -C proofs/cbmc proofs-fragment_reassembly`, which is on the nightly
list through `proof.mk`.

## Checker

The checker is ESBMC 8.5.0, building the harness as C++17. CBMC (5.95.1 and
6.6.0) cannot parse the reassembly translation unit in either language
mode:

- `sdk/rocketnet_memory.h` uses `<initializer_list>`, a reference
  parameter, and a range-based for.
- `sdk/rocketnet_fragment.cpp:89` has a comparison chain that the CBMC C++
  parser rejects.
- `sdk/rocketnet_wire.h` has `__builtin_offsetof` static_asserts.

ESBMC's clang front end parses all of it, with no sdk change.

`esbmc_cprover.h` maps the harness's `__CPROVER_assert` and
`__CPROVER_assume` onto ESBMC, and `proof.mk` passes it with
`--include-file`. ESBMC treats every undefined `nondet_` function as
nondeterministic. `proof.mk` compiles the `.c` harness through a one-line
C++ wrapper, because ESBMC picks the language from the file extension.

ESBMC checks by default: array bounds, pointer validity and dereference,
division by zero, and the unwinding assertions. The flags add
`--overflow-check` (signed arithmetic) and `--ub-shift-check`.
`harness_memory_sizing` also adds `--unsigned-overflow-check`.

The other harnesses do not use the unsigned check. The reader's stale test
depends on unsigned wraparound (`payload_id - reference`, the rollover past
0xFFFFFFFF), and in C++ that wraparound is defined behaviour. Also,
ESBMC's unsigned check flags `rocketnet_read_uint32`'s `<< 24`
(`sdk/rocketnet_read_write.h:87`), which is defined for `uint32_t`. Every
other check in CBMC_FLAGS has an ESBMC counterpart in the list above.

## Models (outside the reader)

- **memcpy.** Every `memcpy` (in the reader and in the inline helpers it
  calls) is `proof_memcpy`. It makes a bounds-checked write of the
  destination's first and last byte from the source's first and last
  byte. Objects are contiguous, so both ends inside one object means the
  whole span is inside it. The checker's bounds checks fire when either
  end is outside its object.
  - A span that touches the slot arena must also lie wholly inside one of
    its two regions: the payload (`max_payload_bytes` from the start) or
    the staging buffer after it (asserted for destination and source). The
    object bounds alone would let a placement run from the payload into
    staging.
  - Overlap between source and destination is not checked.
  - The bytes between the ends are not modelled. No property depends on
    payload bytes, only on where they go.
  - The model is needed because the completion copy runs up to a whole
    payload (361,728 bytes at the full bound), and ESBMC's library memcpy
    is a byte loop that cannot unroll that far.
- **memset.** `memset` is ESBMC's library loop, unwound 64 times. The
  reader only clears 56-byte slot records with it.
- **Logging.** `rocketnet_error` and `rocketnet_spam` are no-ops.
- **sdk assertions.** The build leaves `NDEBUG` unset and maps
  `rocketnet_assert` onto a checked property, so every `rocketnet_assert`
  the reader reaches (the plan's four postconditions,
  `sdk/rocketnet_fragment.cpp:62-65`, and the completion's,
  `:567-572`) is proved with the rest.
- **libsodium.** The reader makes no libsodium call.
- **Allocator.** `rocketnet_malloc` is not reached: the harness lays out
  the set exactly as `rocketnet_reassembly_set_create_delivered` does
  (slot records, then the per-connection reference, in one object), and
  places the arena and the delivered mirror in their own objects.

## Results

The gate ran on a 16-core Linux bench (hetzner2, 2026-10-22), with ESBMC
8.5.0: Boolector for the process and sizing harnesses, Z3 4.13.3 for the
header harness. The whole target took 70 s, with a peak resident set of
256 MiB, inside the 16 GB limit.

| harness | properties | unwind | bound proved | time |
|---|---|---|---|---|
| harness_header_and_plan | 87 of 87 | 16 | full: every 6-byte header, every `int` payload size | 16 s |
| harness_process_fragment | 568 of 568 | 33, memset 64 | fragments at the full bound; `max_payload_bytes` 1..40 only, below every deployed maximum (at least 1416); one connection, one slot | 47 s |
| harness_memory_sizing | 56 of 56 | 16 | every argument the transports pass, not every `int` | 7 s |

Until c613a6a8 the process and sizing harnesses ran on Bitwuzla 0.9.0 (31 s
and 6 s). That commit set `ROCKETNET_MAX_RECEIVE_FRAGMENT_BYTES` to 1413
(it was 1433). It changed no proof logic or unwind, but the harness sizes
track that macro, so its effective fragment and arena bound decreased with
the constant. From that commit on, Bitwuzla runs out of the 16 GB limit on
both (the sizing harness grew past 100 GB with no limit), and Boolector
proves both.
The reversed witness below (the arena one byte short) still fails under
Boolector.

No property failed in any harness. No defect was found at BASE.

### harness_header_and_plan

**Header.** For every 6-byte header (symbolic bytes), the payload id is
the first four bytes, little-endian. The fragment index is the fifth byte,
and the count is the sixth byte plus one. No read leaves the 6-byte
object.

**Plan.** For every `int` payload size:

- A plan exists exactly when the size is above
  `ROCKETNET_MAX_PACKET_PAYLOAD_BYTES` and at most
  `ROCKETNET_MAX_FRAGMENTED_PAYLOAD_BYTES`.
- The count is in 2..256.
- The stride is in 1..`ROCKETNET_MAX_FRAGMENT_BYTES`, and every stride
  fits the receiver's staging (`ROCKETNET_MAX_RECEIVE_FRAGMENT_BYTES`).
- The final fragment is 1..stride bytes.
- No signed overflow or division by zero occurs on any input.

### harness_process_fragment

One connection and one slot. Inputs:

- The fragment is an object of exactly `type_data_bytes` symbolic bytes,
  with `type_data_bytes` symbolic in 0..1420: from empty to one byte past
  the largest the reader accepts (6 + 1413). Every payload id, index byte
  and count byte is symbolic.
- `max_payload_bytes` is symbolic in 1..40.
- The slot's arena is `max_payload_bytes + 1413` bytes, as create sizes
  it.
- The delivered mirror is a symbolic size, from one payload up to its
  ceiling, with a symbolic part already used and up to 64 completions
  already delivered.
- The slot before the call is any state `slot_ok` admits, not only an
  empty one. `slot_ok` is a superset of the reachable states: the count,
  stride, final size, staged final and received bitmap agree with each
  other and with the payload maximum (it also admits an occupied slot with
  no fragment held). The harness assumes it before the call and asserts it
  after, so the invariant is checked inductively for one call.
- The stale rule's reference is symbolic.

Proved, beyond the memory and arithmetic checks:

- No write leaves the slot's arena, the staging buffer, or the delivered
  mirror. No read leaves the fragment object.
- An illegal size, zero id, count of one, or index out of range is refused,
  and leaves the slot unchanged.
- Each other check refuses: an id beyond reach of the newest completed,
  and an id 1..256 behind the occupant (both counted stale), a count that
  differs from the pinned count, a non-final fragment off the stride, a
  final over the stride, and an index already held.
- A refusal fires exactly one refusal counter; an acceptance fires none.
- Every refusal leaves the slot unchanged or clears it. A refusal completes
  nothing and leaves the stale rule's reference unchanged.
- `accepted` counts exactly the acceptances.
- An accepted, uncompleted fragment holds the slot under its own id and
  count, and has its index bit set. Its placement,
  `stride x index + bytes` (or `stride x (count - 1) + bytes` for the
  final), is inside the payload maximum. A final that arrives first waits
  in staging. Every copy, including the staged final's placement and the
  completing fragment's, stays inside its region (the memcpy model).
- Only the last missing fragment completes a payload. Completion clears
  the slot, and moves the reference only to the completed id.
- A delivered completion's span is inside the delivered mirror, and its
  size is 1..`max_payload_bytes`. A completion that does not fit counts
  `batch_full` and packs nothing.

`PROOF_SLOT_CASE` 1..5 splits the same harness by slot state and arriving
fragment, for smaller runs. Case 0, the default, is all of them.

### harness_memory_sizing

The functions checked are `rocketnet_reassembly_memory_required`,
`_required_delivered`, `_allowed` and `_allowed_delivered`. Inputs:

- `num_connections` up to `ROCKETNET_MAX_CLIENTS`. The server passes its
  client count, and the client passes one.
- `delivered_capacity` up to `ROCKETNET_RECEIVE_PACKETS_PER_CLIENT *
  ROCKETNET_MAX_CLIENTS`, the largest server drain.
  Both delivered-capacity functions return at most their drain, and the
  client's drain is smaller.
- The delivered byte bound up to that capacity times the aligned largest
  payload, which is the most `rocketnet_server_reassembly_delivered_bytes`
  returns.
- Every `int` slot count and payload size, valid or not, and every
  negative or zero argument.

Proved:

- No unsigned or signed overflow occurs in any sizing sum or product.
- Invalid geometry, and a delivered bound below one payload, return 0 and
  are refused.
- A nonzero requirement covers the slot arenas and the delivered mirror.
- `allowed` holds exactly when the requirement is nonzero and inside
  `ROCKETNET_REASSEMBLY_MEMORY_MIB`.

### Reversed witnesses (scratch, not committed)

- With the harness's arena one byte short of create's sizing, the staging
  write fails the bounds check in `proof_memcpy`
  (`array bounds violated: heap object`).
- With a scratch copy of `sdk/rocketnet_fragment.cpp` whose final-size check
  allows one byte past the stride (line 443), the harness fails
  "the slot stays in a reachable state".

## Not proved: the full bounds

`make -C proofs/cbmc proofs-fragment_reassembly-full` runs the full bounds.
It is not on the nightly list. Every full-bound run below stopped at the
solver's memory, or at the 30-minute limit, without a verdict; the proved
bounds are the largest that finished inside a 16 GB address-space limit.

- **The bound: `max_payload_bytes` at most 40.** This is the bound proved for
  `rocketnet_reassembly_process_fragment`; the full production range is not.
  Payload bytes are copied opaquely: no branch or index depends on a payload
  byte, so bytes beyond a small window add no new case to the region, index
  and refusal arithmetic the harness checks. The fragment itself is proved at
  its full size (1413 bytes). Full-size payload bounds exhaust the solver's
  memory (below), so the largest bound that finishes is the one proved.
- **harness_process_fragment at `max_payload_bytes` up to
  `ROCKETNET_MAX_FRAGMENTED_PAYLOAD_BYTES` (361,728).** Not proved. The
  deployed range starts at `ROCKETNET_MAX_PACKET_PAYLOAD_BYTES` (1416), so no
  deployed payload maximum is inside the proved bound. What limits the run
  is the payload maximum, not the fragment size: fragments at the full
  1413 bytes prove in under a minute at `max_payload_bytes` up to 24.
  Measured on a 64-core bench:
  - Bitwuzla, no memory limit: grew to 76 GB, killed after 415 s.
  - Bitwuzla, 14-16 GB limit: out of memory at every bound from 48 up. At
    48: 163 s. At 64: 134 s. At windows 1416..1448: 271 s, and
    361696..361728: 160 s.
  - Z3, no memory limit: no verdict within the 30-minute limit.
  - Bitwuzla at 40: PROVED (146 s on that bench), the bound `proof.mk`
    uses.
  - Split by slot state at bound 64: an empty slot proves (45 s), an
    occupied slot with no stride proves (86 s), and the fixed-stride cases
    run out of memory. These runs predate the region and refusal
    assertions.
- **Other slots and connections.** One connection and one slot only: the
  slot and arena index arithmetic for other slots and connections
  (`sdk/rocketnet_fragment.cpp:343-344`) is not exercised.
- **harness_memory_sizing with every argument unconstrained**
  (`PROOF_SIZING_FULL`: any `int` connection count and capacity, any
  `uint64_t` byte bound). Not proved.
  - Bitwuzla: out of memory under a 40 GB limit in 27 s.
  - Z3: no verdict after 18 and 20 minutes on two runs, stopped.
  - A capacity of 65,536 (past every transport caller): out of memory
    under 40 GB in 27 s.
  - The proved range covers every caller in the sdk; the gap is values no
    transport passes.

## Supersedes `proof_fragment_bounds.c`

This harness supersedes `proof_fragment_bounds.c`, which copies the
reassembly function. The copy is still in `all` (per-commit CI) and is
unchanged. Its divergences from the real function:

- **Stale check.** The copy refuses when unsigned
  `payload_id - slot->payload_id > ROCKETNET_MAX_FRAGMENTS`
  (`proofs/cbmc/proof_fragment_bounds.c:159-160`). The real code keeps a
  `uint32_t` newest-completed reference per connection
  (`sdk/rocketnet_fragment.cpp:378`), applies a reach test against it
  (line 380), and a behind test against `ROCKETNET_REASSEMBLY_BEHIND`
  (line 388).
- **Delivery.** The copy writes delivered bytes at
  `delivered + num_delivered * max_payload_bytes`
  (`proof_fragment_bounds.c:348-349`). The real code packs at a 16-byte stride
  (`ROCKETNET_REASSEMBLY_DELIVERED_ALIGNED`, lines 577-582) and enforces
  the byte bound `delivered_bytes`.
- **Fields.** The copy has no `delivered_bytes` field and no
  `newest_completed` reference.
