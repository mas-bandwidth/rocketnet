# CBMC and ESBMC proof inventory

Inventory recovery base: RocketNet `b8058dc3a7bf84eb9d7899315aed1b9d62ff7f53`
(the first attempt ran at `ef0229e87240cadf6e3b2376d2f72782d5fd7c97`).
This file distinguishes a harness that compiles the current SDK body from a
copy of a body, and a retained historical result from an execution at this
exact source. A timeout, resource exhaustion, `UNKNOWN`, failed unwinding
assertion, skipped property, or smaller diagnostic bound is not a pass.

## Exact-head execution status

The first full-bound completion run is recorded in `full-bounds-results.md`.
The reviewed recovery decomposition and its second timeout are recorded in
`full-bounds-recovery-results.md`. Historical `results.md` files remain useful
design evidence, but are not exact-head execution receipts when the included
SDK translation unit changed after the recorded run.

## Harnesses and claims

| Target / harness | Checker and source binding | Property and assumptions | Bound and unwind | Reachability / negative control | Exact-head status |
| --- | --- | --- | --- | --- | --- |
| `replay-near-4096` / `harness_replay_near_nonwrapping`, `harness_replay_near_wrapping` | CBMC; includes current `sdk/rocketnet_replay.cpp` | For symbolic `sequence > highest_opened`, delta below the deployed 4096-bit history, and an arbitrary selected word, the production advance clears exactly the skipped ring bits, sets the received bit, and advances the high mark. The wrappers partition only the production `end <= 4096` / `end > 4096` branch. `highest_opened < UINT64_MAX` prevents only arithmetic wrap outside this operation's contract. | Full delta 1..4095 across the disjoint/exhaustive partitions; 64 window words; global unwind 65 covers 64-word initialization and either production range-clear path. | Each wrapper has a reviewed concrete intended-failure control after the production call and common assertions. Neither control ran because the first partition timed out. | **HOLD** in `full-bounds-recovery-results.md`: nonwrapping timed out in CaDiCaL; wrapping and both controls did not start. |
| `replay-far` / `harness_replay_far` | CBMC; includes current `sdk/rocketnet_replay.cpp` | For delta at least 4096, the production advance clears every word, sets only the received bit, and advances the high mark. The high mark is restricted only enough to make the subtraction defined. | Full far range; 64 words; unwind 65. | All 64 result words are asserted. | Historical default-target result only until rerun at this head. |
| `reconstruct` | CBMC; copied arithmetic leaves from `sdk/rocketnet_protocol.cpp`, not the current SDK body | 32-bit and 24-bit reconstruction keeps the wire low bits, selects the documented nearest candidate, and round-trips an in-window value. | One arithmetic step; unwind 1. | Direct assertions, no loop assumptions. | Not a source-specific proof; current SDK changed after the harness was written. |
| `ack-run-bound-8`, `ack-run-full-4096` / `harness_ack_apply_run` | ESBMC C++17; includes current `sdk/rocketnet_ack.cpp` and calls its static production leaf | A symbolic in-window run has matching live records and exact `sent_written`, `sent_held`, and `sent_unacked` identities; each record's prior ack state is symbolic. Records carry no packet or aggregate id and use a negative RTT sample. The production bitmap walk must count every and only newly acked record, visit it once, retain live identity, clear unacked identity, produce no unknown/batch-full result, and update `largest_acked_sequence`. | Required run 1..8, then declared retained-window run 1..4096. Run-8 unwind 65; full unwind 4097. | `ack-run-negative-control` requires a newly unacked record and negates the observed production visit count after the call. It must fail on the named assertion. | Source-binding syntax passed; all three checker executions were not started after the replay stop condition. |
| `read-write` | CBMC; current inline bodies from `sdk/rocketnet_read_write.h`; copied `rocketnet_copy_string` body from `sdk/rocketnet.cpp` | Integer and byte round trips, bounded read refusal and cursor stability, and bounded string termination. The backing read object deliberately has eight bytes of slack so the guard's pointer addition remains inside one object. | Eight-byte buffers; unwind 9. | Direct accepted/refused assertions. No retained planted failure. | Historical default-target result only; copied string body is not source-specific. |
| `message-read` | CBMC; includes current `sdk/rocketnet_message.cpp` | Reader begin/next never reads beyond a 24-byte symbolic payload; accepted record spans fit the receive buffer; malformed input and a short fixed config body refuse. | Payload 24; unwind 8. | Accepted/refused branches are asserted. | Historical default-target result only; included SDK changed after the last retained result. |
| `xdp-free-list` | CBMC; includes current `sdk/rocketnet_xdp_free_list.h` | Push/pop preserves exactly the admitted frames; full, out-of-region, and non-4096-aligned pushes refuse. | Capacity 3, four operations; unwind 5. | Refusal branches and list equality asserted. | Historical default-target result only; not the deployed frame count. |
| `fragment-bounds` | CBMC; copied former reassembly body | Small malformed-state and placement checks. | One connection/slot, fragment bytes at most 8, payload maximum at most 16; unwind 64. | Direct refusal and arena assertions. | Superseded, excluded from `all`, and not source-specific. |
| `proofs-fragment_reassembly` | ESBMC C++17; includes current `sdk/rocketnet_fragment.cpp` | Header/plan, process-fragment memory and state refusals, and overflow-safe memory sizing. | Full fragment size, but process `max_payload_bytes <= 40`; one connection/slot; unwind 16 or 33 plus library `memset` 64. | Historical planted write failures are recorded in `fragment_reassembly/results.md`. | The retained result is reduced-bound and predates current SDK. `proofs-fragment_reassembly-full` is the full target and its historical result is resource exhaustion, not pass. |
| `proofs-message_read_max` | ESBMC C++17; includes current `sdk/rocketnet_message.cpp` | Multi-record reader, full-size single record, and fixed config body parsing. | Multi-record payload only 0..24 with unwind 7; single/config payload 0..1439 with unwind 2/1. | Planted out-of-bounds failures are recorded in `esbmc_r_ok/results.md`. | Retained multi-record result is reduced-bound and all retained results predate current SDK. |
| `proofs-packet_open` | CBMC; includes current `sdk/rocketnet_protocol.cpp` | Full packet prefilter/open bounds, associated-data writer, and sequence nonce. Cryptographic primitive correctness is not claimed; nondeterministic crypto return values exercise caller memory and refusal paths. | Packet bytes 0..1473, output 0..1416; unwind 64; object bits 12. | A planted undersized allocation is retained in `esbmc_r_ok/results.md`. | Retained result predates current SDK translation-unit changes. |
| `proofs-connect_token` | ESBMC C++17; includes current `sdk/rocketnet_connect_token.cpp` | Token, signed-data and private-data readers plus caller-side verify/open consistency and identifier bounds. Cryptographic primitive correctness is not claimed. | Full 768/576/432/384-byte objects; per-harness unwind 1..577; 30-minute and 8 GiB limits. | Retained planted read/write failures are in `esbmc_r_ok/results.md`. | Retained result predates current SDK translation-unit changes. |
| `esbmc_r_ok` controls | ESBMC C/C++17; tests `esbmc_shim.h`, not product behavior | Dynamic allocation extent and pointer-offset checks reject one-byte out-of-bounds reads/writes. | Small standalone objects. | `planted_oob.c` must fail for the intended object-bound reason. | Historical checker-compatibility evidence; rerun is required after a shim or checker change. |

## Coverage gaps at inventory time

- The retained fragment-process and arbitrary multi-record message-reader
  results are explicitly reduced-bound diagnostics, not full production-bound
  proofs.
- The copied reconstruct, string-copy, and old fragment harnesses do not
  establish source correspondence. The old copied ACK leaf was removed by
  this run and replaced with the production-source harness above.
- No existing harness proves every production stale-generation transition or
  every live-slot overwrite refusal named by the completion brief. A result
  may not infer those properties from a different caller's memory-safety pass.
- The workflow files checked for tool presence but did not pin an exact tool or
  container digest. The run receipt therefore hashes the actual host binaries
  and names this limitation; it does not call an unpinned installation a
  reproducible container identity.

## Soundness rules

Every successful claim requires all of: the named source binding, explicit
object and loop bounds, unwinding assertions, satisfiable assumptions or a
reaching negative control, a successful checker verdict, and a retained raw
receipt. Cryptographic caller proofs cover memory/state behavior only. Native
tests, fuzzing, sanitizers, TLA correspondence, and production qualification
remain separate evidence.
