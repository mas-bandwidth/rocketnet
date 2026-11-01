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

The limits are symbolic over every value the configuration accepts and below:
`reliable_messages_in_flight` (W) and `receive_message_buffer_bytes` (B) from
1 and 0 up to their `1 << 20` maximum in `sdk/rocketnet_internal.h`, and
`max_message_bytes` up to `ROCKETNET_MAX_MESSAGE_BYTES` (1048576). The
multi-record harness runs in two W partitions, 1..4096 and 4097..`1 << 20`:
with all three limits at their full range in one run, Bitwuzla exhausted
memory (95 GB resident after 3 min, and again under a 24 GB limit after
59 s) and Z3 ran past a 15-minute limit, while each partition finishes in
under a minute.

## Run

`make -C proofs/cbmc proofs-message_read_max` on vision (Linux x86_64),
ESBMC 8.5.0, at af705a92.

## Results

| Harness | Reader(s) | Symbolic length bound | Unwind | Solver | Properties passed / total | Failed / unproved | Verdict | Time |
| --- | --- | --- | ---: | --- | ---: | ---: | --- | ---: |
| `harness_message_reader`, W 1..4096 | `rocketnet_message_reader_begin`, `rocketnet_message_reader_next` | `bytes` in `[0, 24]` (`PAYLOAD_BOUND = 24`), arbitrary single-message and multi-record payloads, symbolic `rocketnet_message_limits_t` | 7 | `--bitwuzla` | 215 / 215 | 0 | `VERIFICATION SUCCESSFUL` | 29 s |
| `harness_message_reader`, W 4097..`1 << 20` | as above | as above | 7 | `--bitwuzla` | 216 / 216 | 0 | `VERIFICATION SUCCESSFUL` | 44 s |
| `harness_message_reader_single_max` | `rocketnet_message_reader_begin`, `rocketnet_message_reader_next` | `bytes` in `[0, 1416]` (`ROCKETNET_RECEIVE_PAYLOAD_BYTES`), non-`RECORDS` payloads (`FORM_SINGLE`, invalid form bytes, short and `NULL` inputs), symbolic `rocketnet_message_limits_t` | 2 | `--z3` | 204 / 204 | 0 | `VERIFICATION SUCCESSFUL` | 13 s |
| `harness_message_read_config_body` | `rocketnet_message_read_config_body` | `bytes` in `[0, 1416]` (`ROCKETNET_RECEIVE_PAYLOAD_BYTES`), symbolic buffer contents and `NULL` | 1 | `--z3` | 71 / 71 | 0 | `VERIFICATION SUCCESSFUL` | 3 s |

All 706 / 706 checked properties across the four runs pass (`0`
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
  unwind 7 --bitwuzla -DW_PARTITION=1; ** 0 of 215 properties failed, 215 passed; 29 s
harness_message_reader VERIFICATION SUCCESSFUL
  unwind 7 --bitwuzla -DW_PARTITION=2; ** 0 of 216 properties failed, 216 passed; 44 s
harness_message_reader_single_max VERIFICATION SUCCESSFUL
  unwind 2 --z3; ** 0 of 204 properties failed, 204 passed; 13 s
harness_message_read_config_body VERIFICATION SUCCESSFUL
  unwind 1 --z3; ** 0 of 71 properties failed, 71 passed; 3 s
```

With check 8's block span test removed from `rocketnet_message.cpp`
(`base_units < 0` alone), the first partition fails on the span assertion:

```
harness_message_reader VERIFICATION FAILED
  unwind 7 --bitwuzla -DW_PARTITION=1; ** 1 of 211 properties failed, 133 passed, 77 not checked; 17 s
```

## 2026-11-01 continuation rerun

At `ffe88b3db18ba2b3650925980d609cf49826b526`, the real
`proofs-message_read_max` target passed on intel (Linux x86_64), serially,
with the unchanged 1800-second limit per harness and an additional 48-GiB
process address-space cap. ESBMC was 8.5.0; its executable SHA-256 was
`d314db72d213859d6cd1cf9045c227489a817cb133dc14d390a5e2663c31ca0a`.
The bounds, partitions, solvers and unwinds above are unchanged.

| Harness | Properties passed / total | Time |
| --- | ---: | ---: |
| `harness_message_reader`, W 1..4096 | 216 / 216 | 21 s |
| `harness_message_reader`, W 4097..`1 << 20` | 217 / 217 | 21 s |
| `harness_message_reader_single_max` | 205 / 205 | 3 s |
| `harness_message_read_config_body` | 71 / 71 | 1 s |

All four processes exited zero with `VERIFICATION SUCCESSFUL`: 709 checked
properties, zero failures. This does not extend the 24-byte multi-record
payload bound or prove the unproved range described above.

The same invocation also ran `proofs-esbmc_r_ok`: in C and C++17 the valid
slices verified (`0 of 9` failed), and the planted overrun alone failed
(`1 of 5`, expected). The pinned Darwin ESBMC build passed the same four
controls. A first unguarded `using std::isnan` import caused a real glibc
C++ parsing conflict; the final shim imports it only on Darwin, whose verifier
math header exposes it in `std`. The failed parsing attempt is not a proof.

## Type-9 integration refresh (2026-11-01)

The frozen source was `59752f6f4878360534f03ef7112a27e09013e9ca`, which
integrates type9 `7047d7c68d2f18caf2dc9c54a4b9ead4a16cad14`. Each of the
four cases ran serially on vision (Linux x86_64) with the exact flags and
1800-second per-case timeout in `proof.mk`; raw output was retained for
every case. The existing ESBMC 8.5.0 executable was verified live, SHA-256
`d314db72d213859d6cd1cf9045c227489a817cb133dc14d390a5e2663c31ca0a`.
Recorded native source hashes match the frozen source copy.

| Harness | Properties passed / total | Elapsed |
| --- | ---: | ---: |
| `harness_message_reader`, W 1..4096 | 216 / 216 | 19.72 s |
| `harness_message_reader`, W 4097..`1 << 20` | 217 / 217 | 19.22 s |
| `harness_message_reader_single_max` | 205 / 205 | 3.31 s |
| `harness_message_read_config_body` | 71 / 71 | 0.90 s |

All four processes exited zero with `VERIFICATION SUCCESSFUL`: 709 checked
properties, zero failures. Unwinds remain 7/7/2/1 with the named memset
limit 64. The two multi-record partitions still cover payload lengths only
0..24; the single-record and config cases retain their full 0..1416 bounds.
These receipts do not extend the unproved multi-record range or establish
a timing qualification.
