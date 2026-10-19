# client_packets: ESBMC proofs of the client's packet readers

`make -C proofs/cbmc proofs-client_packets` runs every harness in
`proof_client_packets.c` under ESBMC. The target is on the nightly list
only, through `proof.mk`. It is not in `all`, and not in
`.github/workflows/cbmc.yml`.

Verdict: not proved. No harness finishes under ESBMC 8.5.0 with Z3. Every
harness that has been run stopped in the solver (Z3 out of memory, or "SMT
solver failed") under the 12 GiB cap, before any property verdict. No
property is refuted and no sdk defect is established.

## Checker, flags and bound

- ESBMC 8.5.0, `--std c++17 --z3 --overflow-check --unsigned-overflow-check
  --ub-shift-check -DROCKETNET_ESBMC`, with
  ESBMC's default pointer, bounds, alignment and division-by-zero checks and
  its unwinding assertions. `--unwind 65` is meant to cover every sdk loop
  on these paths (the longest seen is the 32-byte key write). The named
  bounds `proof_memcpy:0:1473`, `proof_memset:0:1473` and
  `rocketnet_esbmc_havoc_slice:0:1473` are meant to cover a copy, fill or
  havoc of up to one maximum packet. No run has reached the unwinding
  assertions that check these bounds. `rocketnet_assert` stays live (`ROCKETNET_ASSERTS 1`,
  no `-DNDEBUG`).
- The harness includes the shared `proofs/cbmc/esbmc_shim.h` and is copied
  to `proof_client_packets.cpp` so ESBMC selects its C++ front end.
- The bound is the full one. `packet_bytes` is symbolic in
  0..`ROCKETNET_MAX_PACKET_BYTES` (1472), over a `malloc` of exactly
  `packet_bytes` bytes. The dispatcher and record harnesses also make the
  output capacity symbolic in 0..1472, over an object of exactly that size.
- Each run: `nice -n 10`, `prlimit --as` 12 GiB, and a 1800-second timeout.
- The six runs below used `--overflow-check` alone; the unsigned-overflow
  and shift checks were added after them, so they only add properties to a
  run that already did not finish.

## Results (ESBMC 8.5.0, Z3 4.13.3, 2026-10-18)

| harness | result | time | detail |
|---|---|---|---|
| harness_process_cookie | solver failed | 885 s | 868 VCCs, 160 after simplification; 206 s encoding, 405 s in Z3, then "SMT solver failed" at 8 GB resident, under the 12 GiB cap |
| harness_process_challenge | out of memory | 1429 s | 17356 VCCs, 1667 after simplification; "Z3 error out of memory" at 11.2 GB resident |
| harness_process_payload | out of memory | 1234 s | 31634 VCCs, 10647 after simplification; "Z3 error out of memory" at 11.6 GB resident |
| harness_process_denied | not run | | |
| harness_open_session_packet | not run | | |
| harness_process_keep_alive | not run | | |
| harness_process_ack | not run | | |
| harness_process_fragment | out of memory | 907 s | 21347 VCCs, 4751 after simplification; "Z3 error out of memory" at 10.3 GB resident |
| harness_process_disconnect | out of memory | 656 s | 23931 VCCs, 4741 after simplification; "Z3 error out of memory" at 10.3 GB resident |
| harness_process_packet | out of memory | 431 s | ESBMC "Out of memory", then "SMT solver failed", at 11.9 GB resident |
| harness_process_record | not run | | |

Proved: 0. Refuted: 0. Out of memory: 5. Solver failed: 1. Not run: 5. The reversed-witness
pass runs only after a harness proves, so it has not run under ESBMC.

## Why the solver runs out of memory

The cost is the client object, not the packet bound. The cookie reader
accepts only a 38-byte packet and still runs Z3 out of memory.
`struct rocketnet_client_protocol_t` holds an ack endpoint with one sent
record per history packet and an acked-id batch of the same length.
`ROCKETNET_HISTORY_PACKETS` is at least 4096 (`sdk/rocketnet_history.h`), so
the client cannot be built smaller. ESBMC encodes the struct as one Z3
tuple. Every field write on a reader path makes a new tuple of the whole
client. Measured on scratch harnesses (not committed) built on this file:

- `rocketnet_client_protocol_init` alone proves in 0.02 s of encoding.
- Building the symbolic client (init, then about twenty field writes) takes
  26 s of encoding for one assertion.
- An 8-byte library `memcpy` into `client->return_cookie` runs Z3 out of
  memory at 8 GB. The library memcpy writes through `char *`, and ESBMC
  makes that an update of the whole client. With the harness's `memcpy`
  model (byte copy through `uint8_t`) the same write is cheap. The harness
  therefore models memset and memcpy, but the readers still run out of
  memory.
- Holding the client as aligned byte storage (an SMT byte array, not a
  tuple) runs Z3 out of memory on init alone.
- `--tuple-node-flattener` and `--array-flattener` crash ESBMC (signal 11).
  `--tuple-sym-flattener` stops with a Z3 sort error.

## The shim's readable check

The shared shim maps `__CPROVER_r_ok` to `__ESBMC_r_ok`. Under ESBMC 8.5.0,
`__ESBMC_r_ok(p + 6, 8)` is false on a `malloc` of symbolic size n even when
n is 38, so every model that asserts it on a packet fails spuriously. On a
`malloc` of constant size, and on a static array, it is right.
`__ESBMC_get_object_size` is unconstrained for a struct member. This harness
checks an extent by reading its first and last byte instead
(`proof_extent_ok`): ESBMC's own bounds check on those two reads holds
exactly when the range lies inside one object. A scratch test (not
committed) checked both directions: a range inside a symbolic-size malloc
passes, and a range one byte past the end, or past a static struct, fails.
A length above 1 MiB is refused, so a negative length converted to size_t
cannot pass.

## Client state

Each harness builds its client through `rocketnet_client_protocol_init`,
then makes every field a reader branches on symbolic: the state over all
twelve client states; both replay windows (arbitrary) and high-water marks;
the send sequences; the integrity failure count below its limit; the cookie
flag; the rtt and ack-debt fields; and, for the per-type readers, the I/O
verdict of the record being consumed. Sequences stay below 2^63.

## Not done

- No harness proves under ESBMC. The CBMC 6.6.0 run of the same harnesses
  (this card's previous attempt) proved all eleven at the full bound, with
  every witness reached. That run is not the checker this card names.
- The five harnesses marked not run share the same client object, so they
  are expected to stop the same way; that is not measured.
- The sdk headers are included before the memset and memcpy models are
  defined, so an inline helper in those headers that calls memcpy uses
  ESBMC's library memcpy, not the model.
- Landing this target as it stands puts a target on the nightly list that
  does not finish. The CBMC target it replaces proved all eleven harnesses.
