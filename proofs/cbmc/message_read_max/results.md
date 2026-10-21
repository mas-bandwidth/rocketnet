# rocketnet-cbmc-message-read-max

Bounded model checking of `rocketnet_message_reader_begin`,
`rocketnet_message_reader_next`, and `rocketnet_message_read_config_body` on the
real SDK implementation (`sdk/rocketnet_message.cpp`, included directly by
`proof_message_read_max.c` without copying).

The checker is ESBMC 8.5.0 (`~/sdk/bin/esbmc`), invoked in C++17 mode with
`--std c++17 --overflow-check --ub-shift-check -DROCKETNET_ESBMC -DNDEBUG -I../../sdk --unwindsetname __memset_impl:0:64`
and its default pointer, bounds, division-by-zero, and unwinding assertions
enabled. In each harness, the input pointer is either `NULL` or tail-aligned to
the end of a stack buffer (`buf + (BOUND - bytes)`), so any read 1 byte past the
symbolic length `bytes` is an immediate out-of-bounds object violation rather
than slack inside the stack array.

## Results

| Harness | Reader(s) | Symbolic length bound | Unwind | Solver | Properties passed / total | Failed / unproved | Verdict | Time |
| --- | --- | --- | ---: | --- | ---: | ---: | --- | ---: |
| `harness_message_reader` | `rocketnet_message_reader_begin`, `rocketnet_message_reader_next` | `bytes` in `[0, 24]` (`PAYLOAD_BOUND = 24`), arbitrary single-message and multi-record payloads, symbolic `rocketnet_message_limits_t` | 7 | `--bitwuzla` | 204 / 204 | 0 | `VERIFICATION SUCCESSFUL` | 37 s |
| `harness_message_reader_single_max` | `rocketnet_message_reader_begin`, `rocketnet_message_reader_next` | `bytes` in `[0, 1439]` (`ROCKETNET_RECEIVE_PAYLOAD_BYTES`), non-`RECORDS` payloads (`FORM_SINGLE`, invalid form bytes, short and `NULL` inputs), symbolic `rocketnet_message_limits_t` | 2 | `--z3` | 192 / 192 | 0 | `VERIFICATION SUCCESSFUL` | 6 s |
| `harness_message_read_config_body` | `rocketnet_message_read_config_body` | `bytes` in `[0, 1439]` (`ROCKETNET_RECEIVE_PAYLOAD_BYTES`), symbolic buffer contents and `NULL` | 1 | `--z3` | 64 / 64 | 0 | `VERIFICATION SUCCESSFUL` | 2 s |

All 460 / 460 checked properties across the three harnesses pass (`0` failed,
`0` refuted, `0` timed out at the configured bounds).

## Claims proved

1. **`rocketnet_message_reader_begin` and `rocketnet_message_reader_next` (`harness_message_reader` at `[0, 24]` and `harness_message_reader_single_max` at `[0, 1439]`)**:
   - Every refused payload (`violation != ROCKETNET_MESSAGE_VIOLATION_NONE`, including `NULL`, `bytes < 2`, unknown form byte, or any malformed record in the first pass of `rocketnet_message_check_payload` at `sdk/rocketnet_message.cpp:424`) yields `0` on `rocketnet_message_reader_next(&reader, &record)` (`sdk/rocketnet_message.cpp:456`).
   - Every accepted record has `record.data >= payload`, `record.bytes >= 1`, and `record.data + record.bytes <= payload + bytes - 1` (stopping before the trailing form byte, with zero out-of-bounds reads past `bytes`).
   - Every accepted block fragment (`record.block != 0`, `sdk/rocketnet_message.cpp:365-386`) has `base = record.ring - record.fragment * (ROCKETNET_MESSAGE_FRAGMENT_BYTES / 16) >= 0` and `base * 16 + span <= limits.receive_message_buffer_bytes`.
   - Every accepted non-block holdable record (`ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED` or `ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED`, `sdk/rocketnet_message.cpp:388-392`) has `record.ring * 16 + round16(record.bytes) <= limits.receive_message_buffer_bytes`.
   - On `FORM_SINGLE` payloads up to the full `1439`-byte bound (`sdk/rocketnet_message.cpp:463-471`), `rocketnet_message_reader_next` yields exactly one `ROCKETNET_MESSAGE_CODE_UNRELIABLE` record spanning `[payload, payload + bytes - 1)` and `0` thereafter.
2. **`rocketnet_message_read_config_body` (`harness_message_read_config_body` at `[0, 1439]`)**:
   - Any `NULL` buffer, any `bytes != ROCKETNET_MESSAGE_CONFIG_BYTES` (`28`) across `[0, 1439]`, or any buffer with `data[0] != ROCKETNET_MESSAGE_CONTROL_CONFIG` (`1`) is refused with `ROCKETNET_MESSAGE_VIOLATION_BAD_CONTROL` (`sdk/rocketnet_message.cpp:512-515`) without reading past `bytes`.
   - Any accepted 28-byte config record populates `body` from `data[1..27]` within bounds and enforces `zero == 0` (`data[3]`) and `zero_tail == 0` (`data[24..27]`) when `body.version == ROCKETNET_MESSAGE_CONFIG_VERSION` (`sdk/rocketnet_message.cpp:529-530`).

## Unproved multi-record range (`25..1439` bytes)

- For unconstrained multi-record `ROCKETNET_MESSAGE_FORM_RECORDS` payloads (`sdk/rocketnet_message.cpp:424-432` and `:474-484`), each 4-byte minimum record adds one loop iteration in `rocketnet_message_check_payload` and one in the `rocketnet_message_reader_next` drain loop (up to `360` iterations at `1439` bytes). Under ESBMC 8.5.0, symbolic multi-record unwinding completes within 40 seconds up to `PAYLOAD_BOUND = 24` (`--unwind 7`, up to 6 records) and times out above `24` bytes (`PAYLOAD_BOUND = 32, --unwind 9` exceeds the solver budget). No property failed or produced a counterexample at any tested bound.

## Gate output

```
$ make -C proofs/cbmc proofs-message_read_max
proofs-message_read_max
harness_message_reader VERIFICATION SUCCESSFUL
  unwind 7 --bitwuzla; ** 0 of 204 properties failed, 204 passed; 37 s
harness_message_reader_single_max VERIFICATION SUCCESSFUL
  unwind 2 --z3; ** 0 of 192 properties failed, 192 passed; 6 s
harness_message_read_config_body VERIFICATION SUCCESSFUL
  unwind 1 --z3; ** 0 of 64 properties failed, 64 passed; 2 s
```
