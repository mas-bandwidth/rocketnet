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

No harness verified, on CBMC 6.6.0 as on 5.95.1. Every harness includes
`sdk/rocketnet_fragment.cpp`, which is C++, and CBMC parses it in neither
language mode:

- As C (the `.c` harness, the mode the other proofs use): `sdk/rocketnet_memory.h:5`
  includes `<initializer_list>` (fatal: no such file in C mode),
  `sdk/rocketnet_memory.h:21` takes `uint64_t & total` by reference,
  `sdk/rocketnet_memory.h:34` is a range-based for, and
  `sdk/rocketnet_fragment.cpp:93` and `:95` name `rocketnet_reassembly_set_t`
  and `rocketnet_reassembly_slot_t` without `struct`.
- As C++ (`goto-cc -x c++ -std=c++11`, with the libstdc++ initializer_list
  header bypassed): the CBMC C++ parser rejects the range-based for at
  `sdk/rocketnet_memory.h:34` (`parse error before 'v : x )'`, reproduced on a
  three-line file), and it reads a chain of the form
  `a < 1 || a > 256 || b < 1416` as a template argument list:
  `sdk/rocketnet_fragment.cpp:89` fails with
  `parse error before '|| max_payload_bytes < 1416'`. With line 89
  parenthesized in a scratch copy (never committed), line 90 and then
  `sdk/rocketnet_fragment.cpp:170` fail the same way, so the pattern runs
  through the file.

This is a checker front-end limit, not a defect at BASE: g++ compiles the same
file. The message reader proof works because `sdk/rocketnet_message.cpp` does
not include `rocketnet_memory.h`.

Gate output on the bench (CBMC 6.6.0):

    make -C proofs/cbmc proofs-fragment_reassembly
    ../../sdk/rocketnet_memory.h:5:10: fatal error: initializer_list: No such file or directory
    PARSING ERROR
    harness_header_and_plan FAILED (exit 6)

| harness | properties passed | bound | result |
|---|---|---|---|
| harness_header_and_plan | 0 of 0 (not reached) | none run | parse error, sdk/rocketnet_memory.h:5 |
| harness_process_fragment | 0 of 0 (not reached) | none run | parse error, sdk/rocketnet_memory.h:5 |
| harness_memory_sizing | 0 of 0 (not reached) | none run | parse error, sdk/rocketnet_memory.h:5 |

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

All three harnesses. A run on the real code needs one of: the sdk readers made
parseable by CBMC (parenthesized comparison chains in
`sdk/rocketnet_fragment.cpp`, an index loop at `sdk/rocketnet_memory.h:34`, and
the C++-only parts of `rocketnet_memory.h` kept out of the reassembly
translation unit), which is an sdk change outside this card; or a checker with
a full C++ front end, which is not installed.

## Bounds used

None (no verification ran). Intended run bounds: header/plan at the full
fragmented-payload cap; process at a bounded `PROOF_TYPE_BUF`/`PROOF_MAX_PAYLOAD`
with the gap to the full bounds recorded here once it runs.
