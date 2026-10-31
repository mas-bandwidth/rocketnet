# rocketnet-cbmc-message-read-max

Bounded model checking of `rocketnet_message_reader_begin`,
`rocketnet_message_reader_next`, and `rocketnet_message_read_config_body` on the
real SDK implementation (`sdk/rocketnet_message.cpp`, included directly by
`proof_message_read_max.c` without copying).

The checker is ESBMC 8.5.0, invoked in C++17 mode with
`--std c++17 --overflow-check --ub-shift-check -DROCKETNET_ESBMC -DNDEBUG -I../../sdk --unwindsetname __memset_impl:0:64`
and its default pointer, bounds, division-by-zero, and unwinding assertions
enabled. In each harness, the input pointer is either `NULL` or tail-aligned to
the end of a stack buffer (`buf + (BOUND - bytes)`), so any read 1 byte past the
symbolic length `bytes` is an immediate out-of-bounds object violation rather
than slack inside the stack array.

## Run

`make -C proofs/cbmc proofs-message_read_max` on hetzner1 (Linux x86_64),
ESBMC 8.5.0, at 49c36e8f: RocketNet 8e792181 plus the CBMC harness commits
of #754. `sdk/rocketnet_message.cpp` and every header the harness includes
are identical at main 212f6da6.

## Results

| Harness | Reader(s) | Symbolic length bound | Unwind | Solver | Properties passed / total | Failed / unproved | Verdict | Time |
| --- | --- | --- | ---: | --- | ---: | ---: | --- | ---: |
| `harness_message_reader` | `rocketnet_message_reader_begin`, `rocketnet_message_reader_next` | `bytes` in `[0, 24]` (`PAYLOAD_BOUND = 24`), arbitrary single-message and multi-record payloads, symbolic `rocketnet_message_limits_t` | 7 | `--bitwuzla` | 213 / 213 | 0 | `VERIFICATION SUCCESSFUL` | 12 s |
| `harness_message_reader_single_max` | `rocketnet_message_reader_begin`, `rocketnet_message_reader_next` | `bytes` in `[0, 1416]` (`ROCKETNET_RECEIVE_PAYLOAD_BYTES`), non-`RECORDS` payloads (`FORM_SINGLE`, invalid form bytes, short and `NULL` inputs), symbolic `rocketnet_message_limits_t` | 2 | `--z3` | 201 / 201 | 0 | `VERIFICATION SUCCESSFUL` | 4 s |
| `harness_message_read_config_body` | `rocketnet_message_read_config_body` | `bytes` in `[0, 1416]` (`ROCKETNET_RECEIVE_PAYLOAD_BYTES`), symbolic buffer contents and `NULL` | 1 | `--z3` | 71 / 71 | 0 | `VERIFICATION SUCCESSFUL` | 1 s |

All 485 / 485 checked properties across the three harnesses pass (`0`
failed, `0` refuted, `0` timed out at the configured bounds).

## Claims proved

1. **`rocketnet_message_reader_begin` and `rocketnet_message_reader_next` (`harness_message_reader` at `[0, 24]` and `harness_message_reader_single_max` at `[0, 1416]` (`ROCKETNET_RECEIVE_PAYLOAD_BYTES`))**:
   - Every refused payload (`violation != ROCKETNET_MESSAGE_VIOLATION_NONE`, including `NULL`, `bytes < 2`, unknown form byte, or any malformed record found by `rocketnet_message_check_payload`) yields `0` on `rocketnet_message_reader_next(&reader, &record)`.
   - Every accepted record has `record.data >= payload`, `record.bytes >= 1`, and `record.data + record.bytes <= payload + bytes - 1` (stopping before the trailing form byte, with zero out-of-bounds reads past `bytes`).
   - Every accepted block fragment (`record.block != 0`) has `base = record.ring - record.fragment * (ROCKETNET_MESSAGE_FRAGMENT_BYTES / 16) >= 0` and `base * 16 + span <= limits.receive_message_buffer_bytes`.
   - Every accepted non-block holdable record (`ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED` or `ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED`) has `record.ring * 16 + round16(record.bytes) <= limits.receive_message_buffer_bytes`.
   - On `FORM_SINGLE` payloads up to the full bound, `rocketnet_message_reader_next` yields exactly one `ROCKETNET_MESSAGE_CODE_UNRELIABLE` record spanning `[payload, payload + bytes - 1)` and `0` thereafter.
2. **`rocketnet_message_read_config_body` (`harness_message_read_config_body` at `[0, 1416]` (`ROCKETNET_RECEIVE_PAYLOAD_BYTES`))**:
   - Any `NULL` buffer, any `bytes != ROCKETNET_MESSAGE_CONFIG_BYTES` (`28`), or any buffer with `data[0] != ROCKETNET_MESSAGE_CONTROL_CONFIG` (`1`) is refused with `ROCKETNET_MESSAGE_VIOLATION_BAD_CONTROL` without reading past `bytes`.
   - Any accepted 28-byte config record populates `body` from `data[1..27]` within bounds and enforces `zero == 0` (`data[3]`) and `zero_tail == 0` (`data[24..27]`) when `body.version == ROCKETNET_MESSAGE_CONFIG_VERSION`.

## Unproved multi-record range (`25` up to `ROCKETNET_RECEIVE_PAYLOAD_BYTES` (1416) bytes)

- For unconstrained multi-record `ROCKETNET_MESSAGE_FORM_RECORDS` payloads, each 4-byte minimum record adds one loop iteration in `rocketnet_message_check_payload` and one in the `rocketnet_message_reader_next` drain loop (up to `ROCKETNET_RECEIVE_PAYLOAD_BYTES / 4` (354) iterations at the full bound). Under ESBMC 8.5.0, symbolic multi-record unwinding completes up to `PAYLOAD_BOUND = 24` (`--unwind 7`, up to 6 records) and times out above `24` bytes (`PAYLOAD_BOUND = 32, --unwind 9` exceeds the solver budget). No property failed or produced a counterexample at any tested bound.

## Gate output

```
$ make -C proofs/cbmc proofs-message_read_max
proofs-message_read_max
harness_message_reader VERIFICATION SUCCESSFUL
  unwind 7 --bitwuzla; ** 0 of 213 properties failed, 213 passed; 12 s
harness_message_reader_single_max VERIFICATION SUCCESSFUL
  unwind 2 --z3; ** 0 of 201 properties failed, 201 passed; 4 s
harness_message_read_config_body VERIFICATION SUCCESSFUL
  unwind 1 --z3; ** 0 of 71 properties failed, 71 passed; 1 s
```
