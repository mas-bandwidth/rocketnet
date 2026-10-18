# rocketnet-cbmc-fragment-reassembly

This card proves the fragment readers on the real sdk code
(`sdk/rocketnet_fragment.cpp`, included, never copied). Three harnesses live in
`proof_fragment_reassembly.c`:

- `harness_header_and_plan` — `rocketnet_read_fragment_header` and
  `rocketnet_fragment_plan`, every payload up to the fragment cap.
- `harness_process_fragment` — `rocketnet_reassembly_process_fragment` from the
  real `.cpp`, symbolic `type_data_bytes`, symbolic fragment index/count bytes,
  symbolic `max_payload_bytes`; claims no write outside the slot's payload
  region, the refusals, and a completed payload's span inside the arena.
- `harness_memory_sizing` — `rocketnet_reassembly_memory_required` / `_allowed`
  and their `_delivered` forms, no unsigned overflow on any documented argument.

## Result

No harness verified. The installed CBMC (5.95.1 on this bench, not the 6.6.0
the card expects) cannot parse `sdk/rocketnet_fragment.cpp`: it transitively
includes `sdk/rocketnet_memory.h`, which uses C++ constructs the CBMC 5.95 C++
frontend rejects:

- `#include <initializer_list>` and `std::initializer_list<uint64_t>` in
  `rocketnet_memory_allocations` (`sdk/rocketnet_memory.h:30`).
- a range-based `for (uint64_t bytes : sizes)` loop
  (`sdk/rocketnet_memory.h:34`).

Both fail to parse (`parse error before 'x : l'` for range-for; the header
include itself fails in C mode). This is a toolchain limitation, not a defect at
BASE. The message reader proof (`proof_message_read.c`) works only because
`sdk/rocketnet_message.cpp` does not include `rocketnet_memory.h`.

## Supersedes `proof_fragment_bounds.c`

The new harness supersedes `proof_fragment_bounds.c` (which copies the reassembly
function). The copy's divergences from the real function:

- stale check: the copy refuses `payload_id - slot->payload_id > MAX_FRAGMENTS`
  (a plain half-space serial comparison); the real code (lines 387-393) refuses
  `slot->payload_id - payload_id <= REASSEMBLY_BEHIND`, a signed
  newest-completed reference and a full rollover-safe comparison.
- delivery: the copy writes delivered bytes at
  `delivered + num_delivered * max_payload_bytes` (line 348); the real code
  packs at a 16-byte stride (`ROCKETNET_REASSEMBLY_DELIVERED_ALIGNED`, lines
  577-584) and enforces the byte bound.
- the copy has no `delivered_bytes` field and no batch-full byte bound.

## Not done

All three harnesses, because the real `.cpp` cannot be parsed by the installed
CBMC. A fully-working run would need CBMC 6.x (which parses these C++
constructs) or a C-compatible memory header.

## Bounds used

None (no verification ran). Intended run bounds: header/plan at the full
fragmented-payload cap; process at a bounded `PROOF_TYPE_BUF`/`PROOF_MAX_PAYLOAD`
with the gap to the full bounds recorded here once it runs.
