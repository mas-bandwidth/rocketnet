# Integer narrowing sweep, 2026-10-24

A sweep of `mas-bandwidth/rocketnet` at `9775266a705148969cc00fcadc1463b06879cfbd` (`main`) for one bug
class: a `size_t`, `ptrdiff_t`, `uint64_t` or `int64_t` value narrowed to `int`, `uint32_t`, `uint16_t` or
`uint8_t` (a return, an explicit cast, an assignment to a struct field or a parameter) with no range check on
the path, so a large value truncates or turns negative and a caller reads the wrong answer. Scope read: every
`.cpp` and `.h` under `sdk/` (the public header read was `sdk/rocketnet.h`); all of `tests/functional/`; and the
`tests/*.h` helpers the functional and deterministic-simulation tiers share (`counters_expect.h`,
`dst_checks.h`, `dst_harness.h`, `live_verify.h`, `native_shutdown.h`, `pcapng.h`, `portable_crt.h`,
`resource_file.h`, `suite_runner.h`, `time_scale.h`). Out of scope: `sdk/rocketnet_tests.cpp`, which builds
only into the separate test-support library and defines no helper that production code calls (102 hits of
pattern 1, not judged), and the unit-tier-only headers `tests/serialize.h`, `tests/test_unit_standard.h`,
`tests/wycheproof.h` and `tests/client_io_rig.h` (200 hits of pattern 1 and 18 of pattern 2, not judged).
Method: the build compiles every target with `-Wconversion -Wsign-conversion -Werror` on gcc and clang
(`CMakeLists.txt:186`, `:189`), so no implicit integer narrowing (to a return, a field or a parameter)
compiles; every narrowing in the tree is an explicit cast, and the cast patterns below are therefore the
complete candidate set. Patterns and in-scope hit counts:
pattern 1, the cast form `\(\s*(int|uint32_t|uint16_t|uint8_t|int32_t)\s*\)|\b(int|uint32_t|uint16_t|uint8_t)\s*\(`,
1228 hits (717 in `sdk/`, 368 in `tests/functional/`, 143 in `tests/`);
pattern 2, widened after reading showed narrowings to other narrow spellings,
`\(\s*(unsigned( int)?|int16_t|int8_t)\s*\)|\b(int32_t|int16_t|int8_t|unsigned)\s*\(`, 22 hits (15, 5, 2);
pattern 3, functions returning a computed size as `int`, `return int\(|return \(int\)`, 39 hits (34, 1, 4),
every one also a pattern 1 hit; pattern 4, `static_cast<` to any of these types, 0 hits. The union is 1250
distinct lines. Each hit was read in its function, the source type of the cast value was identified, and
every real narrowing was followed back to its origin (a caller size, a config field, the wire, a platform
return, a computed total) through every caller reachable from `sdk/rocketnet.h`, the wire or a config value,
to find the check that keeps it in range; an assert alone was not counted as a check, since asserts compile
out of a release build. No code was changed. Findings are ordered most severe first; each gives severity
(`bug` / `risk` / `quality`), `file:line` at 9775266a, a one-sentence defect, a one-sentence concrete failure
or cost, and the smallest fix. Every other hit is listed under Judged not a defect with the reason it is safe.

## Findings

### 1. bug — `sdk/rocketnet_frame_sync.cpp:222`

- Defect: The macOS frame sync builds its real-time thread policy by casting the `uint64_t` frame period in mach time units to the `uint32_t` fields `period` (line 222), `computation` (line 223) and `constraint` (line 224) with no range check, while `rocketnet_frame_sync_create_ex` (line 372) bounds the frame rate from above only.
- Failure or cost: `rocketnet_frame_sync_create( ctx, 0.2 )` with thread priority on, on a Mac whose timebase is 1/1, asks the kernel for a period of 705032704 units (0.705 s, the 5 s period truncated) against a constraint of 2.5 s, and on a 125/3 timebase any rate below about 0.0056 Hz wraps the same way, so the policy the kernel judges is not the frame's.
- Smallest fix: When `period_mach` exceeds `UINT32_MAX`, skip the time constraint policy and keep plain absolute sleep, as the refusal path at line 233 already does.

### 2. risk — `sdk/rocketnet_frame_sync.cpp:337`

- Defect: `rocketnet_frame_sync_advance` returns the `uint64_t` count of missed periods, `error / period_ns`, cast to `int` with no bound, and the waiter at line 498 adds it to `dropped_frames` only when the result is positive.
- Failure or cost: At the maximum 1000 Hz, one wait that wakes 2^31 or more periods late (about 24.8 days, such as a process stopped under a debugger or `SIGSTOP` while the monotonic clock runs) yields a negative or wrapped count, so `rocketnet_frame_sync_stats` reports too few dropped frames (the deadline itself still advances correctly).
- Smallest fix: Return the count as `uint64_t` (the field it feeds is `uint64_t`), or clamp the quotient to `INT_MAX` before the cast.

### 3. risk — `tests/dst_harness.h:1114`

- Defect: The deterministic-simulation harness narrows its `uint64_t` step counter `k` to `int` to compare and schedule client steps (`(int) k` at lines 1114, 1115, 1120, 1134, 1135, 1145 and 1220), and narrows the `uint64_t` fault window to `int` at lines 925 and 1220, while `--steps` (`tests/dst.cpp:162`) refuses only zero.
- Failure or cost: A run with `--steps 3000000000` turns `(int) k` negative past step 2^31, so disconnected clients stop reconnecting and paused clients never resume, and `--steps 4294967396` truncates the quiet window at line 1220 to step 1100, so a client left terminal by an injected fault is reported as a failure the transport did not cause (`(int) k + 50` at lines 1134 and 1135 is also signed overflow near `INT_MAX`).
- Smallest fix: Refuse `--steps` above `INT_MAX - 1000` in `tests/dst.cpp`, or keep `next_connect_step`, `churn_step` and `pause_until` as `uint64_t` (with a separate "unset" flag) and compare without casts.

### 4. risk — `tests/dst_checks.h:525`

- Defect: The whole-message check compares the recorded sent size, clamped to 65535 at line 296, with the delivered `int bytes` narrowed by `(uint16_t) bytes`, which wraps modulo 65536 instead of clamping, so the two sides disagree for any message above 65535 bytes.
- Failure or cost: A harness sending a legal 70000-byte message (`max_message_bytes` reaches 1 << 20) records 65535 and then compares 4464 for the correct delivery, a false `whole_message` failure, while a delivery 65536 bytes off the sent size passes; today both callers cap messages at 16384 bytes (`tests/dst_harness.h:772`, `tests/fuzz/rocketnet_fuzz_pair.h:275`), so nothing reaches it yet.
- Smallest fix: Compare with the same clamp, `(uint16_t) ( bytes > 65535 ? 65535 : bytes )`, or widen `sent_size` to `uint32_t`.

## Counts

- bug: 1 (finding 1)
- risk: 3 (findings 2–4)
- quality: 0
- total: 4

## Judged not a defect

The 1235 in-scope hits not named by a finding or under Already filed, one row each, in file order. Lines 223
and 224 of `sdk/rocketnet_frame_sync.cpp` belong to finding 1; lines 925, 1115, 1120, 1134, 1135, 1145 and
1220 of `tests/dst_harness.h` belong to finding 3.

| file:line | why it is safe |
| --- | --- |
| `tests/functional/channel_config.cpp:52` | source is int index and loop int i widened to uint32_t for a hash, not narrowing |
| `tests/functional/channel_config.cpp:56` | intentional top byte of a uint32_t hash, x >> 24 is at most 0xFF |
| `tests/functional/channel_config.cpp:103` | double timeout times 60, not integer narrowing |
| `tests/functional/channel_config.cpp:130` | double timeout times 60, not integer narrowing |
| `tests/functional/channel_config.cpp:177` | double timeout times 60, not integer narrowing |
| `tests/functional/channel_config.cpp:196` | double timeout times 60, not integer narrowing |
| `tests/functional/channel_config.cpp:213` | double timeout times 60, not integer narrowing |
| `tests/functional/channel_control_packets.cpp:105` | source is int i under 3000, intentional byte pattern wrap, not wide narrowing |
| `tests/functional/channel_harness.h:194` | uint8_t ipv4 bytes widened to uint32_t, not narrowing |
| `tests/functional/channel_harness.h:195` | uint8_t ipv4 bytes widened to uint32_t, not narrowing |
| `tests/functional/channel_harness.h:207` | vector size_t is datagram bytes plus fixed IPv4/UDP header, bytes bounded by ROCKETNET_RECEIVE_CEILING in the harness |
| `tests/functional/channel_harness.h:216` | source is int kind, a ROCKETNET_PCAPNG_KIND constant of 1 to 3 |
| `tests/functional/channel_harness.h:219` | record size_t is 1 plus opened bytes, bounded by ROCKETNET_RECEIVE_CEILING per tests/functional/channel_harness.h:237 |
| `tests/functional/channel_harness.h:247` | source is int packet_type read from one uint8_t byte packet[0] |
| `tests/functional/channel_harness.h:313` | declaration of a pointer to fixed-size array, not a cast |
| `tests/functional/channel_harness.h:366` | declaration of a pointer to fixed-size array, not a cast |
| `tests/functional/channel_harness.h:858` | pointer cast of malloc result, not integer narrowing |
| `tests/functional/channel_harness.h:860` | pointer cast of malloc result, not integer narrowing |
| `tests/functional/channel_harness.h:862` | pointer cast of malloc result, not integer narrowing |
| `tests/functional/channel_ordered.cpp:59` | source is int message and offset, same width sign cast into a uint32_t hash |
| `tests/functional/channel_ordered.cpp:63` | intentional top byte of a uint32_t hash, x >> 24 is always below 256 |
| `tests/functional/channel_receipts.cpp:64` | source is int step, below messages=48, intentional byte value |
| `tests/functional/channel_receipts.cpp:65` | source is int step, below messages=48, intentional byte value |
| `tests/functional/channel_receipts.cpp:98` | source is enum rocketnet_receipt_status_t, checked ACKED or LOSS on the line above |
| `tests/functional/channel_receipts.cpp:108` | source is enum rocketnet_receipt_status_t, checked ACKED or LOSS on the line above |
| `tests/functional/channel_receipts.cpp:148` | source is int arithmetic, intentional byte pattern wrap |
| `tests/functional/channel_receipts.cpp:149` | source is int arithmetic, intentional byte pattern wrap |
| `tests/functional/channel_receipts_reliable.cpp:58` | source is int arithmetic, intentional low byte of a test fill pattern |
| `tests/functional/channel_receipts_reliable.cpp:59` | source is int arithmetic, intentional low byte of a test fill pattern |
| `tests/functional/channel_reliable.cpp:30` | source is an int test-pattern expression deliberately reduced to a byte, not wide-integer narrowing |
| `tests/functional/channel_reliable.cpp:31` | source is an int test-pattern expression deliberately reduced to a byte, not wide-integer narrowing |
| `tests/functional/channel_reliable.cpp:100` | source is an int test-pattern expression deliberately reduced to a byte, not wide-integer narrowing |
| `tests/functional/channel_reliable.cpp:211` | source is an int test-pattern expression deliberately reduced to a byte, not wide-integer narrowing |
| `tests/functional/channel_reliable.cpp:377` | sizeof of a 1 MiB static array (ROCKETNET_MAX_MESSAGE_BYTES = 1<<20), a compile-time constant that fits int |
| `tests/functional/channel_reliable.cpp:380` | source is an int test-pattern expression deliberately reduced to a byte, not wide-integer narrowing |
| `tests/functional/channel_reliable_sequenced.cpp:24` | intentional byte extraction of a uint32_t state, little-endian encode |
| `tests/functional/channel_reliable_sequenced.cpp:31` | widening uint8_t to uint32_t, not narrowing |
| `tests/functional/channel_reliable_sequenced.cpp:84` | uint64_t counter on a fresh per-test server, bounded by about 1575 states sent in the test at line 97 |
| `tests/functional/channel_resend_timing.cpp:109` | Source is double from ceil, not integer narrowing; index is clamped to 0 .. count-1 at lines 110-113 |
| `tests/functional/channel_resend_timing.cpp:190` | Source is int (sent * 13 + i); intentional byte pattern truncation of a test payload |
| `tests/functional/channel_resend_timing.cpp:196` | sizeof of a 32-byte local array, compile-time constant 32 |
| `tests/functional/channel_resend_timing.cpp:437` | Source is int (i * 31 + 7); intentional byte pattern truncation of a test block |
| `tests/functional/channel_resend_timing.cpp:467` | Source is int (small_sent * 13 + i); intentional byte pattern truncation of a test payload |
| `tests/functional/channel_resend_timing.cpp:472` | sizeof of a 32-byte local array, compile-time constant 32 |
| `tests/functional/channel_smoke.cpp:132` | source is int client_sent below 600 sends, deliberate low and high byte split of the message index |
| `tests/functional/channel_smoke.cpp:138` | source is int server_sent below 600 sends, deliberate low and high byte split of the message index |
| `tests/functional/channel_smoke.cpp:295` | compile-time sizeof of a ROCKETNET_MAX_PACKET_PAYLOAD_BYTES + 1 array |
| `tests/functional/channel_smoke.cpp:296` | source is int loop index, deliberate low-byte fill pattern |
| `tests/functional/channel_smoke.cpp:307` | compile-time sizeof of a 3-byte array |
| `tests/functional/channel_smoke.cpp:310` | compile-time sizeof of a 3-byte array |
| `tests/functional/channel_smoke.cpp:313` | compile-time sizeof of a ROCKETNET_MAX_PACKET_PAYLOAD_BYTES + 1 array |
| `tests/functional/channel_unreliable_budget.cpp:21` | source is int id, intentional low byte of a test pattern, id below 12 |
| `tests/functional/channel_unreliable_budget.cpp:22` | source is int id, intentional low byte of a test pattern |
| `tests/functional/channel_unreliable_budget.cpp:23` | source is int id, intentional low byte of a test pattern |
| `tests/functional/channel_unreliable_budget.cpp:24` | source is int id, intentional low byte of a test pattern |
| `tests/functional/channel_unreliable_budget.cpp:31` | constant sizeof of a 4 byte array |
| `tests/functional/channel_unreliable_budget.cpp:32` | constant sizeof of a 4 byte array |
| `tests/functional/channel_unreliable_duplicates.cpp:48` | intentional low byte of a uint32_t pattern hash |
| `tests/functional/channel_unreliable_duplicates.cpp:53` | source is int kind, checked below DUP_KINDS 3 |
| `tests/functional/channel_unreliable_duplicates.cpp:54` | intentional byte extraction of uint32_t id, id below DUP_MESSAGES 400 |
| `tests/functional/channel_unreliable_duplicates.cpp:55` | intentional byte extraction of uint32_t id |
| `tests/functional/channel_unreliable_duplicates.cpp:56` | intentional byte extraction of uint32_t id |
| `tests/functional/channel_unreliable_duplicates.cpp:72` | widening uint8_t bytes to uint32_t, not narrowing |
| `tests/functional/channel_unreliable_duplicates.cpp:139` | constant DUP_MESSAGES 400 cast to uint32_t |
| `tests/functional/channel_unreliable_duplicates.cpp:143` | constant DUP_SEND_TICKS 100 cast to uint32_t |
| `tests/functional/channel_unreliable_send_drops.cpp:52` | source is int arithmetic of uint8_t tag and int, intentional byte pattern wrap |
| `tests/functional/channel_unreliable_send_drops.cpp:60` | source is int arithmetic of uint8_t tag and int, intentional byte pattern wrap |
| `tests/functional/channel_unreliable_send_drops.cpp:98` | source is int, 0x10 plus i below 4 |
| `tests/functional/channel_unreliable_send_drops.cpp:130` | source is int constant 0xA0 or 0xB0 |
| `tests/functional/channel_unreliable_send_drops.cpp:164` | source is int, 0x20 plus i below sent=12 |
| `tests/functional/channel_unreliable_send_drops.cpp:177` | source is int, 0x20 plus arrived checked below sent=12 in the same condition |
| `tests/functional/channel_unreliable_sequenced.cpp:24` | intentional byte extraction of uint32_t state by shift, not wide narrowing |
| `tests/functional/channel_unreliable_sequenced.cpp:31` | uint8_t widened to uint32_t, not narrowing |
| `tests/functional/channel_unreliable_sequenced.cpp:94` | source is int i under 2000, intentional test byte pattern wrap |
| `tests/functional/channel_unreliable_sequenced.cpp:104` | source is int max_message_bytes, same width cast |
| `tests/functional/channel_unreliable_sequenced.cpp:117` | source is int i under 2000, intentional test byte pattern wrap |
| `tests/functional/cpp_transports.cpp:121` | sizeof of a ROCKETNET_RECEIVE_CEILING stack array, a compile-time constant |
| `tests/functional/harness.inc:260` | source is uint16_t port, widening to int for printf |
| `tests/functional/harness.inc:282` | source is uint16_t returned by rocketnet_test_free_port, widening to int |
| `tests/functional/live_matrix.cpp:264` | same width sign cast of uint32_t length bounded by max_message_bytes, at most ROCKETNET_MAX_MESSAGE_BYTES (sdk/rocketnet_internal.h:580) |
| `tests/functional/live_matrix.cpp:265` | same width sign cast of uint32_t length bounded by max_message_bytes, at most ROCKETNET_MAX_MESSAGE_BYTES (sdk/rocketnet_internal.h:580) |
| `tests/functional/live_matrix.cpp:315` | same width sign cast of uint32_t entry length, at most max_message_bytes (1 MiB) |
| `tests/functional/live_matrix.cpp:329` | same width sign cast of uint32_t entry length, at most max_message_bytes (1 MiB) |
| `tests/functional/live_matrix.cpp:333` | same width sign cast of uint32_t entry length, at most max_message_bytes (1 MiB) |
| `tests/functional/live_matrix.cpp:373` | source is int bytes, rocketnet_live_check passed so bytes is at least 1 |
| `tests/functional/live_matrix.cpp:412` | int64_t difference below 256: q < LIVE_MATRIX_MAX_SEQUENCES checked at tests/functional/live_matrix.cpp:390 and q > last checked at :405 |
| `tests/functional/live_matrix.cpp:448` | same width sign cast of uint32_t length from rocketnet_live_length, at most max_message_bytes (1 MiB) |
| `tests/functional/live_matrix.cpp:449` | same width sign cast of uint32_t length from rocketnet_live_length, at most max_message_bytes (1 MiB) |
| `tests/functional/live_matrix.cpp:452` | same width sign cast of uint32_t length, at most max_message_bytes (1 MiB) |
| `tests/functional/live_matrix.cpp:453` | same width sign cast of uint32_t length, at most max_message_bytes (1 MiB) |
| `tests/functional/live_matrix.cpp:506` | int64_t q is a uint32_t next or closing, negative return checked at tests/functional/live_matrix.cpp:500 |
| `tests/functional/live_matrix.cpp:523` | same width sign cast of uint32_t length, at most max_message_bytes (1 MiB) |
| `tests/functional/live_matrix.cpp:542` | source is int mode in 0 to 7 |
| `tests/functional/live_matrix.cpp:594` | same width sign cast of uint32_t length, at most max_message_bytes (1 MiB) |
| `tests/functional/live_matrix.cpp:600` | source is int length in 1 to LIVE_MATRIX_TINY, widening |
| `tests/functional/live_matrix.cpp:614` | same width sign cast of uint32_t receipt count difference, bounded by LIVE_MATRIX_RECEIPT_TABLE sends |
| `tests/functional/live_matrix.cpp:690` | same width sign cast of uint32_t over, at most max_message_bytes plus 1 |
| `tests/functional/live_matrix.cpp:691` | same width sign cast of uint32_t over, at most max_message_bytes plus 1 |
| `tests/functional/live_matrix.cpp:696` | same width sign cast of uint32_t over, at most max_message_bytes plus 1 |
| `tests/functional/live_matrix.cpp:709` | same width sign cast of uint32_t over, at most max_message_bytes plus 1 |
| `tests/functional/live_matrix.cpp:710` | same width sign cast of uint32_t over, at most max_message_bytes plus 1 |
| `tests/functional/live_matrix.cpp:717` | same width sign cast of uint32_t over, at most max_message_bytes plus 1 |
| `tests/functional/live_matrix.cpp:743` | source is int channel in 0 to 31, widening sign cast |
| `tests/functional/live_matrix.cpp:756` | same width sign cast of uint32_t length, at most max_message_bytes (1 MiB) |
| `tests/functional/live_matrix.cpp:765` | source is int mode in 0 to 7 |
| `tests/functional/live_matrix.cpp:840` | same width sign cast of uint32_t probe length, clipped to the mode maximum |
| `tests/functional/live_matrix.cpp:866` | same width sign cast of uint32_t receipt count difference, bounded by probes sent |
| `tests/functional/live_matrix.cpp:919` | array size from a sizeof quotient, compile-time constant, no cast |
| `tests/functional/live_matrix.cpp:920` | size_t loop bound from a sizeof quotient, compile-time constant, no narrowing |
| `tests/functional/live_matrix.cpp:1032` | size_t loop bound from a sizeof quotient, compile-time constant, no narrowing |
| `tests/functional/live_matrix.cpp:1131` | same width sign cast of int max_message_bytes, range checked at sdk/rocketnet_internal.h:580 by message config validation |
| `tests/functional/platform_primitives.cpp:228` | uint64 difference is always 16: the loop above records exactly 16 sends between first and sequence |
| `tests/functional/posix_hard_killed_client.cpp:381` | compile-time sizeof of a 32-byte array |
| `tests/functional/posix_hard_killed_client.cpp:398` | compile-time sizeof of a 32-byte array |
| `tests/functional/posix_hard_killed_client.cpp:421` | source is int slot, loop below ROCKETNET_MAX_CLIENTS, intentional low byte as a payload tag |
| `tests/functional/posix_hard_killed_client.cpp:422` | compile-time sizeof of a 32-byte array |
| `tests/functional/posix_hard_killed_client.cpp:444` | size_t index bounded by live vector of 3 created at posix_hard_killed_client.cpp:671 and :892 |
| `tests/functional/posix_hard_killed_client.cpp:445` | compile-time sizeof of a 32-byte array |
| `tests/functional/posix_hard_killed_client.cpp:606` | compile-time sizeof of a 32-byte array |
| `tests/functional/posix_hard_killed_client.cpp:728` | size_t index below 3 by the loop bound, printf argument |
| `tests/functional/posix_hard_killed_client.cpp:786` | 20000 + arc4random_uniform(20000) is below 40000, fits uint16_t |
| `tests/functional/posix_hard_killed_client.cpp:790` | 20000 + draw % 12000 is below 32000, fits uint16_t |
| `tests/functional/posix_hard_killed_client.cpp:912` | source is int returned from a uint16_t address.port at posix_hard_killed_client.cpp:805 |
| `tests/functional/posix_hard_killed_client.cpp:923` | compile-time sizeof of a 64-byte array |
| `tests/functional/posix_hard_killed_client.cpp:946` | size_t index below 3 by the loop bound, printf argument |
| `tests/functional/posix_hard_killed_client.cpp:1083` | compile-time sizeof of a POSIX_SEAM_BYTES (64) array |
| `tests/functional/posix_hard_killed_client.cpp:1145` | source is int i below total, which equals POSIX_SEAM_PACKETS 10, intentional byte tag |
| `tests/functional/posix_hard_killed_client.cpp:1188` | compile-time sizeof of a POSIX_SEAM_BYTES (64) array |
| `tests/functional/posix_hard_killed_client.cpp:1230` | compile-time element count of a small static array |
| `tests/functional/posix_hard_killed_client.cpp:1244` | compile-time element count of a 3-entry static array |
| `tests/functional/posix_hard_killed_client.cpp:1245` | compile-time element count of a 1-entry static array |
| `tests/functional/posix_hard_killed_client.cpp:1371` | declaration with an unnamed int parameter, not a cast |
| `tests/functional/posix_hard_killed_client.cpp:1376` | declaration with an unnamed int parameter, not a cast |
| `tests/functional/posix_lifecycle_stress.cpp:135` | compile-time sizeof of a 4096-entry proc_fdinfo array, well below INT_MAX |
| `tests/functional/posix_lifecycle_stress.cpp:139` | compile-time sizeof of struct proc_fdinfo |
| `tests/functional/posix_lifecycle_stress.cpp:161` | compile-time sizeof of struct proc_taskinfo |
| `tests/functional/posix_lifecycle_stress.cpp:162` | compile-time sizeof of struct proc_taskinfo |
| `tests/functional/posix_lifecycle_stress.cpp:164` | source is int32_t pti_threadnum, same width |
| `tests/functional/posix_lifecycle_stress.cpp:319` | source is int from POSIX socket(), same type |
| `tests/functional/posix_lifecycle_stress.cpp:346` | compile-time element count of the static cell table |
| `tests/functional/posix_lifecycle_stress.cpp:659` | source is uint16_t from rocketnet_server_transport_port, widening |
| `tests/functional/posix_lifecycle_stress.cpp:954` | same width sign cast of a positive compile-time int count |
| `tests/functional/posix_lifecycle_stress.cpp:959` | same width sign cast of a non-negative int loop index |
| `tests/functional/posix_lifecycle_stress.cpp:965` | same width sign cast of a positive pid_t |
| `tests/functional/posix_lifecycle_stress.cpp:970` | source is int from POSIX socket(), same type |
| `tests/functional/posix_lifecycle_stress.cpp:978` | port is at most last_port 29999 checked at posix_lifecycle_stress.cpp:966, fits uint16_t |
| `tests/functional/posix_lifecycle_stress.cpp:1010` | source is uint16_t from rocketnet_server_transport_port, widening |
| `tests/functional/posix_lifecycle_stress.cpp:1099` | compile-time sizeof of a 64-byte array |
| `tests/functional/posix_socket_errors.cpp:380` | enum client transport state to int for printf, not integer narrowing |
| `tests/functional/posix_socket_errors.cpp:432` | ssize_t from sendto is at most the int bytes requested or -1, fits int |
| `tests/functional/posix_socket_errors.cpp:475` | source is int index, intentional low byte of a test payload tag |
| `tests/functional/posix_socket_errors.cpp:476` | source is int index, intentional low byte of a test payload tag |
| `tests/functional/posix_socket_errors.cpp:478` | source is int arithmetic, intentional low byte of a test fill pattern |
| `tests/functional/posix_socket_errors.cpp:518` | source already uint16_t, rocketnet_server_transport_port returns uint16_t at sdk/rocketnet_server_transport.cpp:1942 |
| `tests/functional/posix_socket_errors.cpp:565` | source is int arithmetic, intentional low byte of a test fill pattern |
| `tests/functional/posix_socket_errors.cpp:585` | uint64_t random modulo 1500 is below 1500, fits int |
| `tests/functional/posix_socket_errors.cpp:587` | intentional low byte of a uint64_t random value used as a fill byte |
| `tests/functional/posix_socket_errors.cpp:815` | sizeof a 100-byte array, constant |
| `tests/functional/posix_socket_errors.cpp:919` | sizeof a string literal minus one, constant |
| `tests/functional/posix_socket_errors.cpp:949` | source is int arithmetic, intentional low byte of a test fill pattern |
| `tests/functional/posix_socket_errors.cpp:1098` | source is int tag plus i, intentional low byte tag, compared the same way at 1115 |
| `tests/functional/posix_socket_errors.cpp:1115` | source is int tag plus arrived, intentional low byte matching the sender at 1098 |
| `tests/functional/posix_socket_errors.cpp:1280` | sizeof of two uint32_t, constant 8 |
| `tests/functional/posix_socket_errors.cpp:1281` | uint32_t sum truncated to a fill byte on purpose, same pattern on both ends |
| `tests/functional/posix_socket_errors.cpp:1348` | source is int tag plus i, intentional low byte tag |
| `tests/functional/posix_socket_errors.cpp:1366` | source is int tag plus arrived, intentional low byte matching the sender |
| `tests/functional/posix_socket_errors.cpp:1387` | source is int tag plus i, intentional low byte tag |
| `tests/functional/posix_socket_errors.cpp:1403` | source is int tag plus arrived, intentional low byte matching the sender |
| `tests/functional/posix_socket_errors.cpp:2182` | source is int arithmetic, intentional low byte of a test fill pattern |
| `tests/functional/posix_socket_errors.cpp:2195` | source is int arithmetic, intentional low byte of a test fill pattern |
| `tests/functional/posix_socket_errors.cpp:2217` | uint64_t random modulo int max_whole (tests/functional/posix_socket_errors.cpp:2140, under MTU) fits int |
| `tests/functional/posix_socket_errors.cpp:2219` | intentional low byte of a uint64_t random value used as a fill byte |
| `tests/functional/posix_socket_errors.cpp:2249` | ssize_t from recv is at most sizeof other_received, 4001 bytes at tests/functional/posix_socket_errors.cpp:2160 |
| `tests/functional/rocketnet_functional_tests.cpp:117` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:118` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:120` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:143` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:144` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:146` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:462` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:471` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:472` | source is int tag below 65536 (test packet counts are at most a few thousand); high byte of a two-byte tag, not wider-integer narrowing |
| `tests/functional/rocketnet_functional_tests.cpp:474` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:493` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:501` | size_t sizeof of a receive buffer sized ROCKETNET_RECEIVE_CEILING or ROCKETNET_RECEIVE_PAYLOAD_BYTES (under 1500), compile-time bound |
| `tests/functional/rocketnet_functional_tests.cpp:528` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:536` | size_t sizeof of a receive buffer sized ROCKETNET_RECEIVE_CEILING or ROCKETNET_RECEIVE_PAYLOAD_BYTES (under 1500), compile-time bound |
| `tests/functional/rocketnet_functional_tests.cpp:631` | source is int count bounded by loop guard count < 20000 at line 625; mod-256 marker byte, not wider-integer narrowing |
| `tests/functional/rocketnet_functional_tests.cpp:770` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:771` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:951` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:1162` | size_t sizeof of a receive buffer sized ROCKETNET_RECEIVE_CEILING or ROCKETNET_RECEIVE_PAYLOAD_BYTES (under 1500), compile-time bound |
| `tests/functional/rocketnet_functional_tests.cpp:1394` | size_t sizeof of a receive buffer sized ROCKETNET_RECEIVE_CEILING or ROCKETNET_RECEIVE_PAYLOAD_BYTES (under 1500), compile-time bound |
| `tests/functional/rocketnet_functional_tests.cpp:1528` | size_t sizeof of a receive buffer sized ROCKETNET_RECEIVE_CEILING or ROCKETNET_RECEIVE_PAYLOAD_BYTES (under 1500), compile-time bound |
| `tests/functional/rocketnet_functional_tests.cpp:1651` | size_t sizeof of a receive buffer sized ROCKETNET_RECEIVE_CEILING or ROCKETNET_RECEIVE_PAYLOAD_BYTES (under 1500), compile-time bound |
| `tests/functional/rocketnet_functional_tests.cpp:1677` | uint64_t sum of per-frame send deltas over at most 120 frames of one payload each way; bounded to hundreds, fits int |
| `tests/functional/rocketnet_functional_tests.cpp:1811` | size_t sizeof of a receive buffer sized ROCKETNET_RECEIVE_CEILING or ROCKETNET_RECEIVE_PAYLOAD_BYTES (under 1500), compile-time bound |
| `tests/functional/rocketnet_functional_tests.cpp:2172` | size_t sizeof(send_data) is the constant 8192 from the local array declaration; fits int |
| `tests/functional/rocketnet_functional_tests.cpp:2173` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:2174` | size_t sizeof(send_data) is the constant 8192 from the local array declaration; fits int |
| `tests/functional/rocketnet_functional_tests.cpp:2313` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:2315` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:2358` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:2450` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:2452` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:2500` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:2503` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:2539` | size_t sizeof of a fixed local test array; compile-time constant far below INT_MAX |
| `tests/functional/rocketnet_functional_tests.cpp:2582` | size_t sizeof(send_data[i]) is the constant 64 from static uint8_t send_data[8][64]; fits int |
| `tests/functional/rocketnet_functional_tests.cpp:2605` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:2924` | size_t sizeof(pattern) is ROCKETNET_TRANSPORT_MAX_PAYLOAD_BYTES 16384 (sdk/rocketnet_layers.h:40); fits int |
| `tests/functional/rocketnet_functional_tests.cpp:2925` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:3074` | size_t sizeof(cursor_buffer) is ROCKETNET_TRANSPORT_MAX_PAYLOAD_BYTES 16384 (sdk/rocketnet_layers.h:40); fits int |
| `tests/functional/rocketnet_functional_tests.cpp:3099` | size_t sizeof of a receive buffer sized ROCKETNET_RECEIVE_CEILING or ROCKETNET_RECEIVE_PAYLOAD_BYTES (under 1500), compile-time bound |
| `tests/functional/rocketnet_functional_tests.cpp:3256` | size_t sizeof of a receive buffer sized ROCKETNET_RECEIVE_CEILING or ROCKETNET_RECEIVE_PAYLOAD_BYTES (under 1500), compile-time bound |
| `tests/functional/rocketnet_functional_tests.cpp:3355` | size_t sizeof of a receive buffer sized ROCKETNET_RECEIVE_CEILING or ROCKETNET_RECEIVE_PAYLOAD_BYTES (under 1500), compile-time bound |
| `tests/functional/rocketnet_functional_tests.cpp:3487` | size_t sizeof of a receive buffer sized ROCKETNET_RECEIVE_CEILING or ROCKETNET_RECEIVE_PAYLOAD_BYTES (under 1500), compile-time bound |
| `tests/functional/rocketnet_functional_tests.cpp:3493` | size_t sizeof of a receive buffer sized ROCKETNET_RECEIVE_CEILING or ROCKETNET_RECEIVE_PAYLOAD_BYTES (under 1500), compile-time bound |
| `tests/functional/rocketnet_functional_tests.cpp:3655` | size_t sizeof(payload) is the constant 16 from const uint8_t payload[16]; fits int |
| `tests/functional/rocketnet_functional_tests.cpp:3658` | size_t sizeof(payload) is the constant 16 from const uint8_t payload[16]; fits int |
| `tests/functional/rocketnet_functional_tests.cpp:3659` | int client index widened to uint64_t and size_t constant 16 to int; no wider-to-narrower loss, index valid inside CONNECTED branch |
| `tests/functional/rocketnet_functional_tests.cpp:3662` | size_t sizeof(payload) is the constant 16 from const uint8_t payload[16]; fits int |
| `tests/functional/rocketnet_functional_tests.cpp:3752` | source is int (rocketnet_client_index returns int, sdk/rocketnet.h:1607); same-type cast, not narrowing |
| `tests/functional/rocketnet_functional_tests.cpp:3892` | size_t sizeof of a receive buffer sized ROCKETNET_RECEIVE_CEILING or ROCKETNET_RECEIVE_PAYLOAD_BYTES (under 1500), compile-time bound |
| `tests/functional/rocketnet_functional_tests.cpp:4006` | size_t sizeof of a receive buffer sized ROCKETNET_RECEIVE_CEILING or ROCKETNET_RECEIVE_PAYLOAD_BYTES (under 1500), compile-time bound |
| `tests/functional/rocketnet_functional_tests.cpp:4282` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:4283` | source is int tag below 65536 (test packet counts are at most a few thousand); high byte of a two-byte tag, not wider-integer narrowing |
| `tests/functional/rocketnet_functional_tests.cpp:4285` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:4304` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:4334` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:4335` | source is int tag below 65536 (test packet counts are at most a few thousand); high byte of a two-byte tag, not wider-integer narrowing |
| `tests/functional/rocketnet_functional_tests.cpp:4337` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:4348` | size_t sizeof of a receive buffer sized ROCKETNET_RECEIVE_CEILING or ROCKETNET_RECEIVE_PAYLOAD_BYTES (under 1500), compile-time bound |
| `tests/functional/rocketnet_functional_tests.cpp:4360` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:4414` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:4443` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:4458` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:4608` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:4610` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:4630` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:4647` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:4649` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:4659` | size_t sizeof of a receive buffer sized ROCKETNET_RECEIVE_CEILING or ROCKETNET_RECEIVE_PAYLOAD_BYTES (under 1500), compile-time bound |
| `tests/functional/rocketnet_functional_tests.cpp:4670` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:5299` | not a narrowing cast: sizeof inside a CMSG macro or array declaration, compile-time constant |
| `tests/functional/rocketnet_functional_tests.cpp:5312` | not a narrowing cast: sizeof inside a CMSG macro or array declaration, compile-time constant |
| `tests/functional/rocketnet_functional_tests.cpp:5313` | source is const int probe_segment_bytes = 100; constant fits uint16_t |
| `tests/functional/rocketnet_functional_tests.cpp:5325` | size_t sizeof(super) is the constant 800 (8 segments of 100 bytes); fits int |
| `tests/functional/rocketnet_functional_tests.cpp:5329` | not a narrowing cast: sizeof inside a CMSG macro or array declaration, compile-time constant |
| `tests/functional/rocketnet_functional_tests.cpp:5342` | ssize_t recvmsg result, positive here and bounded by the 800-byte iov buffer at line 5328; fits int |
| `tests/functional/rocketnet_functional_tests.cpp:5348` | not a narrowing cast: sizeof inside a CMSG macro or array declaration, compile-time constant |
| `tests/functional/rocketnet_functional_tests.cpp:5349` | ssize_t recvmsg result bounded by the 800-byte iov buffer at line 5328, positive at this point; fits int |
| `tests/functional/rocketnet_functional_tests.cpp:5463` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:5465` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:5486` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:5515` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:5517` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:5530` | size_t sizeof of a receive buffer sized ROCKETNET_RECEIVE_CEILING or ROCKETNET_RECEIVE_PAYLOAD_BYTES (under 1500), compile-time bound |
| `tests/functional/rocketnet_functional_tests.cpp:5543` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:5569` | size_t sizeof(runt) is ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES - 1, a small compile-time constant; fits int |
| `tests/functional/rocketnet_functional_tests.cpp:5572` | size_t sizeof(oversize) is ROCKETNET_RECEIVE_CEILING + 74, under 1600; fits int |
| `tests/functional/rocketnet_functional_tests.cpp:5607` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:5609` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:5616` | not a narrowing cast: sizeof inside a CMSG macro or array declaration, compile-time constant |
| `tests/functional/rocketnet_functional_tests.cpp:5629` | not a narrowing cast: sizeof inside a CMSG macro or array declaration, compile-time constant |
| `tests/functional/rocketnet_functional_tests.cpp:5630` | source is const int gro_segment_bytes = 100; constant fits uint16_t |
| `tests/functional/rocketnet_functional_tests.cpp:5654` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:5799` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:5800` | source is int tag below 65536 (test packet counts are at most a few thousand); high byte of a two-byte tag, not wider-integer narrowing |
| `tests/functional/rocketnet_functional_tests.cpp:5802` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:5814` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:5815` | source is int tag below 65536 (test packet counts are at most a few thousand); high byte of a two-byte tag, not wider-integer narrowing |
| `tests/functional/rocketnet_functional_tests.cpp:5817` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:5837` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:5845` | size_t sizeof of a receive buffer sized ROCKETNET_RECEIVE_CEILING or ROCKETNET_RECEIVE_PAYLOAD_BYTES (under 1500), compile-time bound |
| `tests/functional/rocketnet_functional_tests.cpp:5851` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:5932` | source is int loop index below ROCKETNET_GSO_SEND_QUEUE_SIZE 4096; memset fill byte, intended mod-256 wrap |
| `tests/functional/rocketnet_functional_tests.cpp:5959` | size_t sizeof(tag) is the constant 64 from uint8_t tag[64]; fits int |
| `tests/functional/rocketnet_functional_tests.cpp:5968` | size_t sizeof of a receive buffer sized ROCKETNET_RECEIVE_CEILING or ROCKETNET_RECEIVE_PAYLOAD_BYTES (under 1500), compile-time bound |
| `tests/functional/rocketnet_functional_tests.cpp:5974` | size_t sizeof(tag) is the constant 64 from uint8_t tag[64]; fits int |
| `tests/functional/rocketnet_functional_tests.cpp:6041` | size_t sizeof(first) of a fixed local test array, small compile-time constant; fits int |
| `tests/functional/rocketnet_functional_tests.cpp:6062` | size_t sizeof(second) of a fixed local test array, small compile-time constant; fits int |
| `tests/functional/rocketnet_functional_tests.cpp:6210` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:6212` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:6233` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:6259` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:6261` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:6295` | size_t sizeof of a receive buffer sized ROCKETNET_RECEIVE_CEILING or ROCKETNET_RECEIVE_PAYLOAD_BYTES (under 1500), compile-time bound |
| `tests/functional/rocketnet_functional_tests.cpp:6308` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:6331` | size_t sizeof(runt) is ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES - 1, a small compile-time constant; fits int |
| `tests/functional/rocketnet_functional_tests.cpp:6334` | size_t sizeof(oversize) is ROCKETNET_RECEIVE_CEILING + 74, under 1600; fits int |
| `tests/functional/rocketnet_functional_tests.cpp:6449` | source is int tag = num_expected, at most 65 segments over the three bursts; fits uint8_t |
| `tests/functional/rocketnet_functional_tests.cpp:6451` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:6455` | size_t sizeof(super) is the constant 65000 from static uint8_t super[65000]; fits int |
| `tests/functional/rocketnet_functional_tests.cpp:6458` | not a narrowing cast: sizeof inside a CMSG macro or array declaration, compile-time constant |
| `tests/functional/rocketnet_functional_tests.cpp:6471` | not a narrowing cast: sizeof inside a CMSG macro or array declaration, compile-time constant |
| `tests/functional/rocketnet_functional_tests.cpp:6472` | source is int burst stride from a constant table (100, 300, 1200) at line 6438; fits uint16_t |
| `tests/functional/rocketnet_functional_tests.cpp:6492` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:6571` | source is int loop index below ROCKETNET_URING_SEND_QUEUE_SIZE 2048; memset fill byte, intended mod-256 wrap |
| `tests/functional/rocketnet_functional_tests.cpp:6597` | size_t sizeof(tag) is the constant 64 from uint8_t tag[64]; fits int |
| `tests/functional/rocketnet_functional_tests.cpp:6606` | size_t sizeof of a receive buffer sized ROCKETNET_RECEIVE_CEILING or ROCKETNET_RECEIVE_PAYLOAD_BYTES (under 1500), compile-time bound |
| `tests/functional/rocketnet_functional_tests.cpp:6612` | size_t sizeof(tag) is the constant 64 from uint8_t tag[64]; fits int |
| `tests/functional/rocketnet_functional_tests.cpp:6652` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:6653` | size_t sizeof(packet) is the constant 64 from uint8_t packet[64]; fits int |
| `tests/functional/rocketnet_functional_tests.cpp:6654` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:6655` | size_t sizeof(packet) is the constant 64 from uint8_t packet[64]; fits int |
| `tests/functional/rocketnet_functional_tests.cpp:6686` | size_t sizeof(tag) is the constant 64 from uint8_t tag[64]; fits int |
| `tests/functional/rocketnet_functional_tests.cpp:6696` | size_t sizeof(tag) is the constant 64 from uint8_t tag[64]; fits int |
| `tests/functional/rocketnet_functional_tests.cpp:6815` | size_t sizeof(int) is the constant 4; fits int |
| `tests/functional/rocketnet_functional_tests.cpp:6829` | size_t sizeof(int) is the constant 4; fits int |
| `tests/functional/rocketnet_functional_tests.cpp:6971` | size_t sizeof(int) is the constant 4; fits int |
| `tests/functional/rocketnet_functional_tests.cpp:7014` | size_t sizeof(int) is the constant 4; fits int |
| `tests/functional/rocketnet_functional_tests.cpp:7127` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:7136` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:7172` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:7188` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:7369` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:7393` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:7426` | size_t sizeof(truncated_data) is the constant 16; fits int |
| `tests/functional/rocketnet_functional_tests.cpp:7427` | size_t sizeof(compact_receive_data[0]) is the constant 8; fits int |
| `tests/functional/rocketnet_functional_tests.cpp:7527` | size_t sizeof(packet) is the constant 8 from uint8_t packet[8] at line 7502; fits int |
| `tests/functional/rocketnet_functional_tests.cpp:7590` | size_t sizeof(packet) is the constant 8 from uint8_t packet[8] at line 7579; fits int |
| `tests/functional/rocketnet_functional_tests.cpp:7607` | size_t sizeof(packet) is the constant 8 from uint8_t packet[8] at line 7579; fits int |
| `tests/functional/rocketnet_functional_tests.cpp:7702` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:7703` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:7739` | size_t sizeof of a receive buffer sized ROCKETNET_RECEIVE_CEILING or ROCKETNET_RECEIVE_PAYLOAD_BYTES (under 1500), compile-time bound |
| `tests/functional/rocketnet_functional_tests.cpp:8101` | source is unsigned int octet, not wider; range checked octet <= 0xff at line 8100 |
| `tests/functional/rocketnet_functional_tests.cpp:8156` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:8157` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:8213` | size_t sizeof of a receive buffer sized ROCKETNET_RECEIVE_CEILING or ROCKETNET_RECEIVE_PAYLOAD_BYTES (under 1500), compile-time bound |
| `tests/functional/rocketnet_functional_tests.cpp:8325` | size_t built from int atoi of a tcpdump length field, cast back to int for a log line; round-trips the original int |
| `tests/functional/rocketnet_functional_tests.cpp:8381` | size_t sizeof(first[0]) is the constant 16 from first[2][16] at line 8369; fits int |
| `tests/functional/rocketnet_functional_tests.cpp:8382` | size_t sizeof(first[1]) is the constant 16 from first[2][16] at line 8369; fits int |
| `tests/functional/rocketnet_functional_tests.cpp:8389` | size_t sizeof of a receive buffer sized ROCKETNET_RECEIVE_CEILING or ROCKETNET_RECEIVE_PAYLOAD_BYTES (under 1500), compile-time bound |
| `tests/functional/rocketnet_functional_tests.cpp:8391` | size_t sizeof(first[0]) is the constant 16 from first[2][16] at line 8369; fits int |
| `tests/functional/rocketnet_functional_tests.cpp:8601` | size_t sizeof(payload) is the constant 32 from uint8_t payload[32]; fits int |
| `tests/functional/rocketnet_functional_tests.cpp:8602` | size_t sizeof(payload) is the constant 32 from uint8_t payload[32]; fits int |
| `tests/functional/rocketnet_functional_tests.cpp:8616` | size_t sizeof(received) is the constant 64 from uint8_t received[64]; fits int |
| `tests/functional/rocketnet_functional_tests.cpp:9229` | source is uint16_t port (sdk/rocketnet.h:1803); widening to int, not narrowing |
| `tests/functional/rocketnet_functional_tests.cpp:9230` | source is uint16_t port (sdk/rocketnet.h:1803); widening to int, not narrowing |
| `tests/functional/rocketnet_functional_tests.cpp:9265` | source is an enum client state, not a wider integer; printf argument |
| `tests/functional/rocketnet_functional_tests.cpp:9393` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:9395` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:9402` | source is int (index, count, tag or thread id); deliberate mod-256 test pattern byte, sender and checker wrap identically |
| `tests/functional/rocketnet_functional_tests.cpp:9442` | size_t sizeof of a receive buffer sized ROCKETNET_RECEIVE_CEILING or ROCKETNET_RECEIVE_PAYLOAD_BYTES (under 1500), compile-time bound |
| `tests/functional/rocketnet_functional_tests.cpp:9470` | size_t sizeof of a receive buffer sized ROCKETNET_RECEIVE_CEILING or ROCKETNET_RECEIVE_PAYLOAD_BYTES (under 1500), compile-time bound |
| `tests/functional/rocketnet_functional_tests.cpp:9949` | size_t element count of the static test table, a compile-time constant of a few hundred entries; fits int |
| `tests/functional/server_receive_backlog.cpp:88` | clients.size() size_t bounded by max_clients 48 at tests/functional/server_receive_backlog.cpp:99 |
| `tests/functional/server_receive_backlog.cpp:155` | source is uint16_t port, widened to int |
| `tests/functional/server_receive_backlog.cpp:230` | size_t i bounded by clients.size() at most 48 per tests/functional/server_receive_backlog.cpp:99 |
| `tests/functional/server_receive_backlog.cpp:231` | source is int frame under 300, byte extraction by shift and mask |
| `tests/functional/server_receive_backlog.cpp:258` | uint8_t widened to uint32_t, not narrowing |
| `tests/functional/server_receive_backlog.cpp:259` | uint8_t widened to uint32_t, not narrowing |
| `tests/functional/server_receive_backlog.cpp:261` | source is int constant 48 and int frame, sign cast only |
| `tests/functional/windows_connreset.cpp:161` | sizeof of a ROCKETNET_MAX_PACKET_PAYLOAD_BYTES array, a compile-time constant |
| `tests/functional/windows_connreset.cpp:174` | uint64 *sent < WINDOWS_CONNRESET_MEASURED_PACKETS (100) is required at tests/functional/windows_connreset.cpp:173 before the cast |
| `tests/functional/windows_connreset.cpp:446` | uint64 server_sent counts test sends, bounded by fixed loop counts far below 2^32; sequence <= 100 required at :447 when used |
| `tests/functional/windows_connreset.cpp:450` | sizeof of a WINDOWS_CONNRESET_PAYLOAD_BYTES (32) array, a compile-time constant |
| `tests/functional/windows_connreset.cpp:456` | sizeof of a WINDOWS_CONNRESET_PAYLOAD_BYTES (32) array, a compile-time constant |
| `tests/functional/windows_connreset.cpp:769` | sizeof of a WINDOWS_CONNRESET_PAYLOAD_BYTES (32) array, a compile-time constant |
| `tests/functional/windows_socket_errors.cpp:105` | source is int arithmetic, intentional low byte of a test fill pattern |
| `tests/functional/windows_socket_errors.cpp:116` | intentional low byte of a uint64_t generator value used as a fill byte |
| `tests/functional/windows_socket_errors.cpp:123` | intentional low byte of a uint64_t generator value used as a fill byte |
| `tests/functional/windows_socket_errors.cpp:151` | sizeof sockaddr_in6, constant |
| `tests/functional/windows_socket_errors.cpp:159` | sizeof sockaddr_in, constant |
| `tests/functional/windows_socket_errors.cpp:167` | size_t vector size at most 65507, largest size built at tests/functional/windows_socket_errors.cpp:99 |
| `tests/functional/windows_socket_errors.cpp:168` | size_t vector size at most 65507, largest size built at tests/functional/windows_socket_errors.cpp:99 |
| `tests/functional/windows_socket_errors.cpp:205` | sizeof a ROCKETNET_MAX_PACKET_PAYLOAD_BYTES array, constant |
| `tests/functional/windows_socket_errors.cpp:442` | size_t vector size at most 65507, sizes bounded at tests/functional/windows_socket_errors.cpp:99 and 118 |
| `tests/functional/windows_socket_errors.cpp:496` | uint16_t port widened to int for printf, not narrowing |
| `tests/functional/windows_socket_errors.cpp:515` | size_t vector size at most 65507, sizes bounded at tests/functional/windows_socket_errors.cpp:99 and 118 |
| `tests/functional/windows_socket_errors.cpp:611` | sizeof a ROCKETNET_RECEIVE_PACKET_BYTES array, constant |
| `tests/functional/windows_socket_errors.cpp:613` | sizeof a ROCKETNET_RECEIVE_PACKET_BYTES array, constant |
| `sdk/rocketnet.cpp:125` | long long parsed checked INT32_MIN .. INT32_MAX at sdk/rocketnet.cpp:123 before the cast |
| `sdk/rocketnet.cpp:195` | Source is an enum with int size, not wider-integer narrowing |
| `sdk/rocketnet.cpp:257` | size_t length clamped to at most 1023 at sdk/rocketnet.cpp:296-297, the only caller of this static sink |
| `sdk/rocketnet.cpp:261` | size_t length clamped to at most 1023 at sdk/rocketnet.cpp:296-297, the only caller of this static sink |
| `sdk/rocketnet.h:832` | static_assert comparing sizeof values, not a narrowing |
| `sdk/rocketnet.h:833` | static_assert comparing sizeof values, not a narrowing |
| `sdk/rocketnet.h:834` | static_assert comparing sizeof values, not a narrowing |
| `sdk/rocketnet.h:845` | _Static_assert comparing sizeof values, not a narrowing |
| `sdk/rocketnet.h:846` | _Static_assert comparing sizeof values, not a narrowing |
| `sdk/rocketnet.h:847` | _Static_assert comparing sizeof values, not a narrowing |
| `sdk/rocketnet_abi.cpp:26` | Compile-time element count of a small static size table, a handful of entries |
| `sdk/rocketnet_ack.cpp:36` | intentional low 32 bits of uint64_t largest by explicit mask, wire field is 32 bits |
| `sdk/rocketnet_ack.cpp:37` | source is int num_ranges at most 255, capped by max_ranges at sdk/rocketnet_ack.cpp:85 or read from u8 |
| `sdk/rocketnet_ack.cpp:38` | uint32_t first_range_length capped below 0xFFFF in build at sdk/rocketnet_ack.cpp:97, read from u16 at :160 |
| `sdk/rocketnet_ack.cpp:45` | ptrdiff_t written bytes at most 7 plus 4 times 255 ranges |
| `sdk/rocketnet_ack.cpp:54` | uint64_t modulo ROCKETNET_REPLAY_WINDOW_BITS 4096, result under 4096 |
| `sdk/rocketnet_ack.cpp:138` | uint32_t gap capped at 0xFFFF by loop guard at sdk/rocketnet_ack.cpp:111 |
| `sdk/rocketnet_ack.cpp:139` | uint32_t run capped at 0xFFFF by loop guard at sdk/rocketnet_ack.cpp:123 |
| `sdk/rocketnet_ack.cpp:221` | uint32_t gap read from u16 wire field at sdk/rocketnet_ack.cpp:217 |
| `sdk/rocketnet_ack.cpp:222` | uint32_t length read from u16 wire field at sdk/rocketnet_ack.cpp:218 |
| `sdk/rocketnet_ack.cpp:383` | MSVC unsigned long bit index, 32 bits on Windows, value 0 to 63 |
| `sdk/rocketnet_ack.cpp:596` | uint64_t modulo ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE 2048 fits uint16_t |
| `sdk/rocketnet_ack.cpp:612` | uint16_t stats_prev widened to int, not narrowing |
| `sdk/rocketnet_ack.cpp:623` | int after is a ring slot in 0 to 2047, from tail or stats_prev, fits uint16_t |
| `sdk/rocketnet_ack.cpp:641` | uint64_t modulo ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE 2048 fits uint16_t |
| `sdk/rocketnet_ack.cpp:825` | source is int wire_bytes parameter, sign cast only |
| `sdk/rocketnet_ack.cpp:827` | bool comparison result, not integer narrowing |
| `sdk/rocketnet_ack.cpp:912` | source is int fragment_count, at most ROCKETNET_MAX_FRAGMENTS 256 |
| `sdk/rocketnet_ack.cpp:941` | source is int wire_bytes parameter, sign cast only |
| `sdk/rocketnet_ack.cpp:959` | MSVC unsigned long bit index, 32 bits on Windows, value 0 to 63 |
| `sdk/rocketnet_ack.cpp:974` | popcount byte sum shifted by 56, value 0 to 64 |
| `sdk/rocketnet_ack.cpp:982` | __popcnt64 result 0 to 64 |
| `sdk/rocketnet_ack.cpp:1044` | source is int bit 0 to 63, sign cast to uint32_t only |
| `sdk/rocketnet_ack.cpp:1098` | uint64_t modulo ROCKETNET_SENT_PACKET_BUFFER_SIZE 4096, result under 4096 |
| `sdk/rocketnet_ack.cpp:1099` | uint64_t modulo ROCKETNET_SENT_PACKET_BUFFER_SIZE 4096, result under 4096 |
| `sdk/rocketnet_ack.cpp:1175` | uint64_t modulo ROCKETNET_RTT_HISTORY_SIZE 2048, plus double to float, not integer narrowing |
| `sdk/rocketnet_ack.cpp:1371` | uint64_t modulo ROCKETNET_SENT_PACKET_BUFFER_SIZE 4096, result under 4096 |
| `sdk/rocketnet_ack.cpp:1448` | uint64_t modulo ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE 2048, result under 2048 |
| `sdk/rocketnet_ack.cpp:1509` | no cast here, uint64_t size arithmetic, not narrowing |
| `sdk/rocketnet_ack.cpp:1533` | int widened to size_t, not narrowing |
| `sdk/rocketnet_address.cpp:31` | Source is a char digit difference 0..9, not wider-integer narrowing |
| `sdk/rocketnet_address.cpp:35` | unsigned int value checked value > 65535 refused at sdk/rocketnet_address.cpp:32 |
| `sdk/rocketnet_address.cpp:275` | Source is uint8_t ipv4 byte, same width |
| `sdk/rocketnet_address.cpp:276` | Source is uint8_t ipv4 byte, same width |
| `sdk/rocketnet_address.cpp:277` | Source is uint8_t ipv4 byte, same width |
| `sdk/rocketnet_address.cpp:278` | Source is uint8_t ipv4 byte, same width |
| `sdk/rocketnet_address.cpp:286` | Two uint8_t bytes combined into 16 bits after int promotion, always fits uint16_t |
| `sdk/rocketnet_address.cpp:287` | Two uint8_t bytes combined into 16 bits after int promotion, always fits uint16_t |
| `sdk/rocketnet_address.cpp:295` | uint16_t masked with 0xFF00 and shifted 8, always fits uint8_t |
| `sdk/rocketnet_address.cpp:296` | uint16_t masked with 0x00FF, always fits uint8_t |
| `sdk/rocketnet_address.cpp:297` | uint16_t masked with 0xFF00 and shifted 8, always fits uint8_t |
| `sdk/rocketnet_address.cpp:298` | uint16_t masked with 0x00FF, always fits uint8_t |
| `sdk/rocketnet_allocator.cpp:128` | uint64_t count checked below 0x3FFFFFFF at sdk/rocketnet_allocator.cpp:126 |
| `sdk/rocketnet_allocator.cpp:129` | no narrowing cast, uint64_t arithmetic |
| `sdk/rocketnet_allocator.cpp:154` | no narrowing cast, uint64_t arithmetic |
| `sdk/rocketnet_allocator.cpp:200` | quotient bounded by send_message_buffer_bytes at most 1 shl 28 or store sized from counts under 0x3FFFFFFF at allocator.cpp:126 |
| `sdk/rocketnet_allocator.cpp:218` | source is int bytes, ceil divide by 1152 is under 2 to the 21 |
| `sdk/rocketnet_allocator.cpp:258` | no narrowing cast, uint64_t arithmetic on widened uint32_t |
| `sdk/rocketnet_allocator.cpp:319` | source is int class index 0 to 2, shifted into uint32_t handle |
| `sdk/rocketnet_allocator.cpp:399` | source is int class index 0 to 2, shifted into uint32_t handle |
| `sdk/rocketnet_allocator.cpp:425` | source is int num_channels validated by rocketnet_message_validate_config at sdk/rocketnet_allocator.cpp:105 |
| `sdk/rocketnet_allocator.cpp:426` | source is int reliable_messages_in_flight validated 64 to 1 shl 20 at allocator.cpp:105 |
| `sdk/rocketnet_allocator.cpp:427` | source is int receive_message_buffer_bytes validated 4096 to 1 shl 20 at allocator.cpp:105 |
| `sdk/rocketnet_allocator.cpp:428` | descriptor bytes divided by size equals Q, checked below UINT32_MAX at sdk/rocketnet_allocator.cpp:134 |
| `sdk/rocketnet_allocator.cpp:429` | source is int packets_in_flight validated 64 to 1 shl 20 at allocator.cpp:105 |
| `sdk/rocketnet_allocator.cpp:433` | source is int unreliable_messages_per_update validated 1 to 1 shl 16 at allocator.cpp:105 |
| `sdk/rocketnet_allocator.cpp:434` | source is int sequenced_messages_held validated 0 to 4096 at allocator.cpp:105 |
| `sdk/rocketnet_allocator.cpp:435` | source is int max_message_receipts validated 16 to 1 shl 16 at allocator.cpp:105 |
| `sdk/rocketnet_allocator.cpp:436` | source is int max_message_bytes validated at most 1 shl 20 at allocator.cpp:105 |
| `sdk/rocketnet_allocator.cpp:459` | uint64_t SR checked at most UINT32_MAX at sdk/rocketnet_allocator.cpp:134 |
| `sdk/rocketnet_allocator.cpp:523` | no cast, uint32_t times size_t memset length, not narrowing |
| `sdk/rocketnet_allocator.cpp:622` | source is int bytes, checked positive on the same line |
| `sdk/rocketnet_allocator.cpp:688` | source is int bytes, refused above max_message_bytes at sdk/rocketnet_allocator.cpp:622 |
| `sdk/rocketnet_allocator.cpp:690` | bytes at most max_message_bytes 1 shl 20 per allocator.cpp:622, at most 911 chunks fits uint16_t |
| `sdk/rocketnet_allocator.cpp:737` | uint64_t tail is span_tail uint32_t below span_bytes, fits uint32_t |
| `sdk/rocketnet_allocator.cpp:751` | uint64_t tail is span_tail uint32_t below span_bytes, fits uint32_t |
| `sdk/rocketnet_allocator.cpp:775` | end at most span_bytes, which is SR at most UINT32_MAX checked at sdk/rocketnet_allocator.cpp:134 |
| `sdk/rocketnet_allocator.cpp:807` | constant flag complement to uint8_t, not integer narrowing |
| `sdk/rocketnet_allocator.cpp:818` | constant RN_SPAN_MIN_BYTES and int bytes checked at least that, sign cast only |
| `sdk/rocketnet_allocator.cpp:873` | source is int bytes, refused above max_message_bytes at sdk/rocketnet_allocator.cpp:818 |
| `sdk/rocketnet_allocator.cpp:875` | bytes at most max_message_bytes 1 shl 20 per allocator.cpp:818, at most 911 chunks fits uint16_t |
| `sdk/rocketnet_allocator.cpp:997` | source is int class index 0 to 2, shifted into uint32_t handle |
| `sdk/rocketnet_allocator.cpp:1010` | descriptor bytes at most max_message_bytes 1 shl 20 per allocator.cpp:622 and :818, chunks fit uint16_t |
| `sdk/rocketnet_allocator.cpp:1032` | used never exceeds total at every caller, so permille is at most 1000 |
| `sdk/rocketnet_allocator.cpp:1080` | source is int bytes, checked at least 1 on the same line |
| `sdk/rocketnet_allocator.cpp:1444` | sizeof of a 4096 byte local buffer, constant |
| `sdk/rocketnet_allocator.h:324` | uint32_t handle shifted right 30, value 0 to 3 |
| `sdk/rocketnet_allocator.h:529` | constant RN_SPAN_MIN_BYTES 1153 |
| `sdk/rocketnet_api.cpp:80` | constants sizeof int and INT_MIN in a static table |
| `sdk/rocketnet_api.cpp:85` | constants sizeof int and INT_MIN in a static table |
| `sdk/rocketnet_api.cpp:102` | source is int kbps, checked positive on the same line |
| `sdk/rocketnet_api.cpp:133` | size_t length only in an error log line, nothing returned to the caller depends on it |
| `sdk/rocketnet_api.cpp:182` | long parsed range-checked against int min and max at sdk/rocketnet_api.cpp:177 |
| `sdk/rocketnet_api.cpp:524` | size_t bytes below sizeof size_t on this branch, at most 7 |
| `sdk/rocketnet_api.cpp:1007` | enum to int for switch, not integer narrowing |
| `sdk/rocketnet_api.cpp:1034` | enum to int for switch, not integer narrowing |
| `sdk/rocketnet_api.cpp:1057` | source is int client index, same width cast |
| `sdk/rocketnet_api.cpp:1088` | enum to int, not integer narrowing |
| `sdk/rocketnet_api.cpp:1121` | enum to int, not integer narrowing |
| `sdk/rocketnet_api.cpp:1155` | source is int, resolved int to int32_t, same width |
| `sdk/rocketnet_api.cpp:1936` | size_t values only in an error log line; length bounded by ROCKETNET_MAX_ADDRESS_STRING_LENGTH buffer |
| `sdk/rocketnet_api.cpp:2064` | uint64_t destination_id checked below max_clients at sdk/rocketnet_api.cpp:2053 |
| `sdk/rocketnet_api.cpp:2102` | enum to int, not integer narrowing |
| `sdk/rocketnet_base64.cpp:135` | source is int value checked nonnegative at line 133, 0 to 63 |
| `sdk/rocketnet_base64.cpp:140` | value masked with 0xFF before the uint8_t cast |
| `sdk/rocketnet_channel.cpp:83` | uint64_t hash masked by entries - 1, a uint32_t, so it fits |
| `sdk/rocketnet_channel.cpp:157` | uint64_t number reduced mod 2^24 by rn_u24 on purpose, compared as a u24 wire number |
| `sdk/rocketnet_channel.cpp:166` | source is int record bytes, same width, 1 to 65535 from the u16 wire field |
| `sdk/rocketnet_channel.cpp:167` | record channel read as u16 or u8 at sdk/rocketnet_message.cpp:350 |
| `sdk/rocketnet_channel.cpp:168` | record code is an int 0 to 4, checked by the parser |
| `sdk/rocketnet_channel.cpp:432` | channel below num_channels checked at sdk/rocketnet_channel.cpp:423, at most ROCKETNET_MAX_CHANNELS 65536 |
| `sdk/rocketnet_channel.cpp:496` | same width uint32_t to int32_t, bytes at most max_message_bytes 1 << 20 (sdk/rocketnet_internal.h:580) |
| `sdk/rocketnet_channel.cpp:497` | channel below num_channels checked at sdk/rocketnet_channel.cpp:472 |
| `sdk/rocketnet_channel.cpp:573` | record channel from the u16 wire field, sdk/rocketnet_message.cpp:350 |
| `sdk/rocketnet_channel.cpp:610` | source is int record bytes, same width, 1 to 65535 from the wire |
| `sdk/rocketnet_channel.cpp:646` | count at most max_count 911, checked at sdk/rocketnet_channel.cpp:718 and sdk/rocketnet_channel_reliable.cpp:41 |
| `sdk/rocketnet_channel.cpp:656` | uint64_t number reduced mod 2^24 by rn_u24 on purpose |
| `sdk/rocketnet_channel.cpp:684` | count at most max_count 911, checked at sdk/rocketnet_channel.cpp:718 and sdk/rocketnet_channel_reliable.cpp:41 |
| `sdk/rocketnet_channel.cpp:685` | count at most max_count 911, checked at sdk/rocketnet_channel.cpp:718 and sdk/rocketnet_channel_reliable.cpp:41 |
| `sdk/rocketnet_channel.cpp:726` | source is int record bytes, same width sign cast, 0 to 65535 from the wire |
| `sdk/rocketnet_channel.cpp:769` | source is int record bytes checked 1 to F at sdk/rocketnet_channel.cpp:726 |
| `sdk/rocketnet_channel.cpp:771` | uint64_t number reduced mod 2^24 by rn_u24 on purpose |
| `sdk/rocketnet_channel.cpp:772` | uint64_t number reduced mod 2^24 by rn_u24 on purpose |
| `sdk/rocketnet_channel.cpp:778` | base_units checked nonnegative and span_end within ring_bytes at sdk/rocketnet_channel.cpp:737 |
| `sdk/rocketnet_channel.cpp:793` | total checked at most max_message_bytes at sdk/rocketnet_channel.cpp:772 |
| `sdk/rocketnet_channel.cpp:794` | record channel from the u16 wire field, sdk/rocketnet_message.cpp:350 |
| `sdk/rocketnet_channel.cpp:935` | bytes checked at most max_message_bytes 1 << 20 at sdk/rocketnet_channel.cpp:931 |
| `sdk/rocketnet_channel.cpp:944` | int receipt_base or type, a small channel type enum |
| `sdk/rocketnet_channel.cpp:986` | header bytes is an int 3 to 20, positive, sign cast only |
| `sdk/rocketnet_channel.cpp:988` | bytes at most pending_bytes at most max_message_bytes, so fragments at most 911 |
| `sdk/rocketnet_channel.cpp:989` | channel below num_channels via send_check at sdk/rocketnet_channel.cpp:925, at most 65536 |
| `sdk/rocketnet_channel.cpp:1051` | source is int config field validated 1 to ROCKETNET_MAX_CHANNELS, same width |
| `sdk/rocketnet_channel.cpp:1052` | source is int config field validated 64 to 1 << 20, same width |
| `sdk/rocketnet_channel.cpp:1053` | source is int config field validated 4096 to 1 << 20, same width |
| `sdk/rocketnet_channel.cpp:1054` | source is int config field validated at most 1 << 20, same width |
| `sdk/rocketnet_channel.cpp:1151` | source is int payload_capacity checked at least 1 at sdk/rocketnet_channel.cpp:1147 |
| `sdk/rocketnet_channel.cpp:1168` | uint32_t from int config, at most ROCKETNET_MAX_CHANNELS, same width |
| `sdk/rocketnet_channel.cpp:1170` | uint32_t from int config, at most 1 << 20, same width |
| `sdk/rocketnet_channel.cpp:1171` | uint32_t from int config, at most 1 << 20, same width |
| `sdk/rocketnet_channel.cpp:1172` | uint32_t from int config, at most 1 << 20, same width |
| `sdk/rocketnet_channel.cpp:1209` | int send_class is a small enum constant from the two callers |
| `sdk/rocketnet_channel.cpp:1211` | uint64_t ring_start mod ring_bytes, at most 1 << 20, divided by 16 fits |
| `sdk/rocketnet_channel.cpp:1220` | widening uint16_t fragments to uint32_t, not narrowing |
| `sdk/rocketnet_channel.cpp:1382` | uint64_t first masked by window - 1, below 1 << 20 |
| `sdk/rocketnet_channel.cpp:1385` | uint64_t first + i masked by window - 1, below 1 << 20 |
| `sdk/rocketnet_channel_reliable.cpp:42` | same-width sign cast of int record->bytes; last branch checks bytes >= 1 first, else a negative never equals F |
| `sdk/rocketnet_channel_reliable.cpp:50` | uint64 descriptor number masked with ROCKETNET_MESSAGE_U24_MASK before the cast, so it fits 24 bits |
| `sdk/rocketnet_channel_reliable.cpp:74` | uint64 descriptor number masked with ROCKETNET_MESSAGE_U24_MASK before the cast, so it fits 24 bits |
| `sdk/rocketnet_channel_reliable.cpp:75` | uint64 descriptor number masked with ROCKETNET_MESSAGE_U24_MASK before the cast, so it fits 24 bits |
| `sdk/rocketnet_channel_reliable.cpp:83` | uint64 masked with region->window - 1; window is uint32 at most 1<<20 (sdk/rocketnet_layers.h:1495) |
| `sdk/rocketnet_channel_reliable.cpp:102` | int64 base_units checked >= 0 and base_units*16 + span <= uint32 ring_bytes at sdk/rocketnet_channel_reliable.cpp:60, so it fits uint32 |
| `sdk/rocketnet_channel_reliable.cpp:106` | uint32 total <= region->max_message_bytes checked at sdk/rocketnet_channel_reliable.cpp:90; that field comes from an int config (sdk/rocketnet_allocator.cpp:436) |
| `sdk/rocketnet_channel_reliable.cpp:130` | uint64 descriptor number masked with ROCKETNET_MESSAGE_U24_MASK before the cast, so it fits 24 bits |
| `sdk/rocketnet_channel_reliable.cpp:141` | uint64 descriptor number masked with ROCKETNET_MESSAGE_U24_MASK before the cast, so it fits 24 bits |
| `sdk/rocketnet_channel_sequenced.cpp:100` | uint32_t channel read as u8 or u16 from the wire at sdk/rocketnet_message.cpp:350, below num_channels checked at :368 |
| `sdk/rocketnet_channel_sequenced.cpp:101` | source is int code, the channel type constant RELIABLE_SEQUENCED or UNRELIABLE_SEQUENCED |
| `sdk/rocketnet_channel_sequenced.cpp:128` | same width sign cast of int record bytes, wire range 1 to 65535 |
| `sdk/rocketnet_channel_sequenced.cpp:143` | count at most max_count, ceil of 1 MiB over 1152 is 911, checked at sdk/rocketnet_channel_sequenced.cpp:127 |
| `sdk/rocketnet_channel_sequenced.cpp:145` | int64_t base checked nonnegative with end within ring_bytes at sdk/rocketnet_channel_sequenced.cpp:129 |
| `sdk/rocketnet_channel_sequenced.cpp:156` | int64_t base checked nonnegative with end within ring_bytes at sdk/rocketnet_channel_sequenced.cpp:129 |
| `sdk/rocketnet_channel_sequenced.cpp:185` | count at most max_count, 911, checked at sdk/rocketnet_channel_sequenced.cpp:127 |
| `sdk/rocketnet_channel_sequenced.cpp:186` | count at most max_count, 911, checked at sdk/rocketnet_channel_sequenced.cpp:127 |
| `sdk/rocketnet_channel_sequenced.cpp:187` | int64_t base checked nonnegative with end within ring_bytes at sdk/rocketnet_channel_sequenced.cpp:129 |
| `sdk/rocketnet_channel_sequenced.cpp:196` | same width sign cast of uint32_t total, at most max_message_bytes checked at sdk/rocketnet_channel_sequenced.cpp:178 |
| `sdk/rocketnet_channel_sequenced.cpp:354` | uint64_t number kept as low 32 bits, every reader masks it to window bits or U24 (sdk/rocketnet_channel_sequenced.cpp:37-38) |
| `sdk/rocketnet_channel_sequenced.cpp:373` | same width sign cast of int record bytes, at most ROCKETNET_MAX_PACKET_PAYLOAD_BYTES checked at sdk/rocketnet_channel_sequenced.cpp:307 |
| `sdk/rocketnet_channel_sequenced.cpp:376` | uint32_t channel read as u8 or u16 from the wire at sdk/rocketnet_message.cpp:350, below num_channels checked at :368 |
| `sdk/rocketnet_channel_sequenced.cpp:444` | same width sign cast of uint32_t descriptor bytes, at most max_message_bytes (1 MiB) |
| `sdk/rocketnet_channel_sequenced.cpp:448` | same width sign cast of uint32_t hold bytes, at most ROCKETNET_MAX_PACKET_PAYLOAD_BYTES |
| `sdk/rocketnet_client_gso.h:77` | declaration of a function pointer, not a cast |
| `sdk/rocketnet_client_gso.h:78` | declaration of a function pointer, not a cast |
| `sdk/rocketnet_client_gso.h:79` | declaration of a function pointer, not a cast |
| `sdk/rocketnet_client_gso.h:80` | declaration of a function pointer, not a cast |
| `sdk/rocketnet_client_gso.h:93` | declaration with sizeof in an array bound, not a cast |
| `sdk/rocketnet_client_gso.h:99` | declaration with sizeof in an array bound, not a cast |
| `sdk/rocketnet_client_gso_linux.cpp:140` | CMSG_SPACE of sizeof uint16_t, constant, no cast |
| `sdk/rocketnet_client_gso_linux.cpp:144` | CMSG_LEN of sizeof uint16_t, constant, no cast |
| `sdk/rocketnet_client_gso_linux.cpp:145` | int stride, run over 1 keeps run bytes at most 65000 (sdk/rocketnet_coalesce.h:83, ROCKETNET_CLIENT_GSO_MAX_SUPER_BYTES), fits uint16_t |
| `sdk/rocketnet_client_gso_linux.cpp:174` | source is int, positive message count at most the send batch, sign cast only |
| `sdk/rocketnet_client_gso_linux.cpp:269` | comparison against CMSG_LEN of sizeof int, no cast |
| `sdk/rocketnet_client_gso_linux.cpp:339` | same width cast of unsigned msg_len, no MSG_TRUNC so at most iov_len 65536 at sdk/rocketnet_client_gso_linux.cpp:314 |
| `sdk/rocketnet_client_io.h:320` | sizeof a ROCKETNET_RECEIVE_CEILING array, constant |
| `sdk/rocketnet_client_io.h:321` | source is int packet type from the prefilter, a known small type when floor is nonzero |
| `sdk/rocketnet_client_io.h:614` | size_t holding an int already checked at most ROCKETNET_TRANSPORT_MAX_DATAGRAM_BYTES at sdk/rocketnet_client_io.h:597 |
| `sdk/rocketnet_client_io.h:645` | size_t holding an int already checked at most ROCKETNET_TRANSPORT_MAX_DATAGRAM_BYTES at sdk/rocketnet_client_io.h:632 |
| `sdk/rocketnet_client_protocol.cpp:98` | Compile-time element count of a two-entry local span array |
| `sdk/rocketnet_client_protocol.cpp:396` | Same width sign cast of int client_index, validated 0 .. max_clients-1 at sdk/rocketnet_client_protocol.cpp:1073 |
| `sdk/rocketnet_client_protocol.cpp:397` | Same width sign cast of int max_clients, validated at most ROCKETNET_MAX_CLIENTS at sdk/rocketnet_client_protocol.cpp:1073 |
| `sdk/rocketnet_client_protocol.cpp:707` | Function pointer typedef declaration, not a narrowing |
| `sdk/rocketnet_client_protocol.cpp:1070` | Same width sign cast of wire uint32_t; negative or out-of-range refused at sdk/rocketnet_client_protocol.cpp:1073 |
| `sdk/rocketnet_client_protocol.cpp:1071` | Same width sign cast of wire uint32_t; negative or above ROCKETNET_MAX_CLIENTS refused at sdk/rocketnet_client_protocol.cpp:1073 |
| `sdk/rocketnet_client_protocol.cpp:1115` | Same width sign cast of the bytes the validator accepted at sdk/rocketnet_client_protocol.cpp:1073 via the open at :880 |
| `sdk/rocketnet_client_protocol.cpp:1116` | Same width sign cast of the bytes the validator accepted at sdk/rocketnet_client_protocol.cpp:1073 via the open at :880 |
| `sdk/rocketnet_client_protocol.cpp:1544` | uint64_t modulo 0xFFFFFFFF plus 1 is at most 0xFFFFFFFF, always fits uint32_t |
| `sdk/rocketnet_client_slots.cpp:56` | uint64_t size arithmetic from sizeof, no narrowing |
| `sdk/rocketnet_client_slots.cpp:57` | uint64_t size arithmetic from sizeof, no narrowing |
| `sdk/rocketnet_client_slots.cpp:82` | int widened to size_t, num_buckets positive since max_clients 1 .. INT_MAX/2 checked at sdk/rocketnet_client_slots.cpp:70 |
| `sdk/rocketnet_client_slots.cpp:83` | int widened to size_t, max_clients positive checked at sdk/rocketnet_client_slots.cpp:70 |
| `sdk/rocketnet_client_slots.cpp:85` | int widened to size_t, max_clients positive checked at sdk/rocketnet_client_slots.cpp:70 |
| `sdk/rocketnet_client_slots.cpp:86` | int widened to size_t, max_clients positive checked at sdk/rocketnet_client_slots.cpp:70 |
| `sdk/rocketnet_client_slots.cpp:87` | int widened to size_t, max_clients positive checked at sdk/rocketnet_client_slots.cpp:70 |
| `sdk/rocketnet_client_slots.cpp:88` | int widened to size_t, max_clients positive checked at sdk/rocketnet_client_slots.cpp:70 |
| `sdk/rocketnet_client_slots.cpp:100` | int widened to size_t, max_clients positive checked at sdk/rocketnet_client_slots.cpp:70 |
| `sdk/rocketnet_client_slots.cpp:102` | int widened to size_t, max_clients positive checked at sdk/rocketnet_client_slots.cpp:70 |
| `sdk/rocketnet_client_slots.cpp:103` | int widened to size_t, max_clients positive checked at sdk/rocketnet_client_slots.cpp:70 |
| `sdk/rocketnet_client_slots.cpp:165` | uint64_t modulo num_buckets, which is below INT_MAX by the check at sdk/rocketnet_client_slots.cpp:70 |
| `sdk/rocketnet_client_transport.cpp:82` | sizeof in an array dimension, compile-time constant, no cast |
| `sdk/rocketnet_client_transport.cpp:797` | source is int wire_bytes, same type |
| `sdk/rocketnet_client_transport.cpp:1380` | source is int bind family, an address type enum value |
| `sdk/rocketnet_client_transport.cpp:1403` | source is uint16_t preferred port returned through an int (sdk/rocketnet_platform.h:222) |
| `sdk/rocketnet_client_transport.cpp:1577` | source is int address family from rocketnet_platform_client_bind_family |
| `sdk/rocketnet_client_transport.cpp:2327` | uint64_t packet count over a one-second window of 256 buckets (sdk/rocketnet_client_transport.cpp:2316), far below INT_MAX |
| `sdk/rocketnet_client_transport.cpp:2328` | uint64_t packet count over a one-second window of 256 buckets (sdk/rocketnet_client_transport.cpp:2316), far below INT_MAX |
| `sdk/rocketnet_client_transport.cpp:2330` | double kbps over a one-second window, INT_MAX kbps is 2 Tbps, unreachable |
| `sdk/rocketnet_client_transport.cpp:2331` | double kbps over a one-second window, INT_MAX kbps is 2 Tbps, unreachable |
| `sdk/rocketnet_client_transport.cpp:2458` | enum constant ROCKETNET_CLIENT_IO_VERDICT_RAW |
| `sdk/rocketnet_client_transport.cpp:2460` | size_t stored from an int at most ROCKETNET_TRANSPORT_MAX_DATAGRAM_BYTES, checked at sdk/rocketnet_client_io.h:597 and :632 |
| `sdk/rocketnet_config.cpp:79` | compile-time offsetof and sizeof of config structs, constants |
| `sdk/rocketnet_config.cpp:80` | compile-time offsetof and sizeof of config structs, constants |
| `sdk/rocketnet_config.cpp:81` | compile-time offsetof of config struct, constant |
| `sdk/rocketnet_config.cpp:82` | compile-time offsetof of config struct, constant |
| `sdk/rocketnet_config.cpp:89` | compile-time offsetof and sizeof of config struct, constants |
| `sdk/rocketnet_config.cpp:179` | compile-time table element count, constant |
| `sdk/rocketnet_config.cpp:269` | source is int fragments, positive, sign cast only |
| `sdk/rocketnet_config.cpp:313` | compile-time table element count, constant |
| `sdk/rocketnet_config.cpp:320` | compile-time table element count, constant |
| `sdk/rocketnet_config.cpp:343` | strlen of the constant ROCKETNET_ prefix |
| `sdk/rocketnet_config.cpp:344` | strlen of a compile-time config table field name, short constant string |
| `sdk/rocketnet_config.cpp:371` | sizeof of a 64 byte local buffer, constant |
| `sdk/rocketnet_config.cpp:376` | strlen of a prefix held in a 64 byte buffer checked at sdk/rocketnet_config.cpp:371 |
| `sdk/rocketnet_config.cpp:377` | strlen of a compile-time config table field name, short constant string |
| `sdk/rocketnet_config.cpp:422` | long long parsed range-checked to int32 at sdk/rocketnet_config.cpp:420 |
| `sdk/rocketnet_config.cpp:507` | sizeof of a ROCKETNET_PLATFORM_GETENV_MAX_BYTES buffer, constant |
| `sdk/rocketnet_config.cpp:580` | sizeof of a 256 byte local buffer, constant |
| `sdk/rocketnet_config.cpp:585` | sizeof of a 256 byte local buffer, constant |
| `sdk/rocketnet_config.cpp:607` | strlen of value held in a 4096 byte getenv buffer, too long refused at sdk/rocketnet_config.cpp:655 |
| `sdk/rocketnet_config.cpp:657` | sizeof of a ROCKETNET_PLATFORM_GETENV_MAX_BYTES buffer, constant |
| `sdk/rocketnet_config.cpp:834` | compile-time array element count, constant |
| `sdk/rocketnet_config.cpp:865` | strlen of a compile-time transport name, short constant string |
| `sdk/rocketnet_config.cpp:920` | pointer difference within the transport config string, ROCKETNET_TRANSPORT_STRING_LENGTH 64 bytes |
| `sdk/rocketnet_connect_token.cpp:26` | ptrdiff_t of a fixed write sequence, compile-time bounded below ROCKETNET_PRIVATE_BLOCK_BYTES |
| `sdk/rocketnet_connect_token.cpp:47` | same width sign cast of int32_t timeout_seconds |
| `sdk/rocketnet_connect_token.cpp:53` | ptrdiff_t of a fixed write sequence, compile-time bounded below ROCKETNET_SIGNED_CONNECT_DATA_BYTES |
| `sdk/rocketnet_connect_token.cpp:63` | same width sign cast of uint32_t wire field to int32_t, not wider narrowing |
| `sdk/rocketnet_connect_token.cpp:156` | ptrdiff_t of a fixed write sequence, compile-time bounded below ROCKETNET_CONNECT_TOKEN_BYTES |
| `sdk/rocketnet_connect_token.cpp:337` | sizeof a ROCKETNET_PRIVATE_BLOCK_BYTES array, constant |
| `sdk/rocketnet_connect_token.cpp:347` | sizeof a ROCKETNET_PRIVATE_BLOCK_BYTES array, constant |
| `sdk/rocketnet_connect_token.cpp:352` | sizeof a ROCKETNET_PRIVATE_BLOCK_BYTES array, constant |
| `sdk/rocketnet_connect_token.cpp:420` | size_t is below ROCKETNET_CONNECT_TOKEN_BYTES on this branch, checked at sdk/rocketnet_connect_token.cpp:418 |
| `sdk/rocketnet_connection_reset.cpp:126` | uint32_t num_channels widened to size_t, not narrowing |
| `sdk/rocketnet_driver.h:99` | function pointer declaration, no cast |
| `sdk/rocketnet_driver.h:141` | function pointer declaration, no cast |
| `sdk/rocketnet_driver.h:148` | function pointer declaration, no cast |
| `sdk/rocketnet_driver_builtin.cpp:37` | no cast, sizeof arithmetic, not narrowing |
| `sdk/rocketnet_driver_builtin.cpp:52` | declaration of a pointer to fixed-size array, not a cast |
| `sdk/rocketnet_driver_builtin.cpp:113` | pointer cast into storage, not integer narrowing |
| `sdk/rocketnet_driver_builtin.cpp:124` | int capacity widened to size_t, not narrowing |
| `sdk/rocketnet_driver_builtin.cpp:713` | test-only fill with small int first plus count, fits uint16_t port |
| `sdk/rocketnet_driver_loopback.cpp:209` | max_clients range 1..1000 enforced by config table at sdk/rocketnet_internal.h:526, so packets at most 16384 |
| `sdk/rocketnet_driver_loopback.cpp:245` | enum constant 49152 and uint16_t port + 1 only when port is not 65535 |
| `sdk/rocketnet_driver_loopback.cpp:246` | uint16_t port + 1 taken only when port is not ROCKETNET_LOOPBACK_LAST_PORT 65535 |
| `sdk/rocketnet_driver_loopback.cpp:249` | enum constant 49152 and uint16_t port + 1 only when port is not 65535 |
| `sdk/rocketnet_driver_loopback.cpp:250` | uint16_t port + 1 taken only when port is not ROCKETNET_LOOPBACK_LAST_PORT 65535 |
| `sdk/rocketnet_driver_loopback.cpp:265` | compile-time constant ROCKETNET_MAX_CLIENTS + 1 |
| `sdk/rocketnet_driver_loopback.cpp:281` | same width sign cast of positive int queue_packets, at most 16384 by sdk/rocketnet_driver_loopback.cpp:209 |
| `sdk/rocketnet_driver_loopback.cpp:283` | int slot below ROCKETNET_LOOPBACK_MAX_ENDPOINTS 1001, fits uint16_t |
| `sdk/rocketnet_driver_loopback.cpp:304` | pointer difference within the 1001-entry endpoints array, plus one fits uint16_t |
| `sdk/rocketnet_encryption_map.cpp:71` | sizeof expressions with uint64_t n, no cast |
| `sdk/rocketnet_encryption_map.cpp:72` | sizeof expressions with uint64_t n, no cast |
| `sdk/rocketnet_encryption_map.cpp:100` | widening int num_buckets to size_t, positive by capacity check at sdk/rocketnet_encryption_map.cpp:84 |
| `sdk/rocketnet_encryption_map.cpp:101` | widening int capacity to size_t, positive by check at sdk/rocketnet_encryption_map.cpp:84 |
| `sdk/rocketnet_encryption_map.cpp:103` | widening int capacity to size_t, positive by check at sdk/rocketnet_encryption_map.cpp:84 |
| `sdk/rocketnet_encryption_map.cpp:130` | widening int capacity to size_t, positive by check at sdk/rocketnet_encryption_map.cpp:84 |
| `sdk/rocketnet_encryption_map.cpp:176` | modulo result below num_buckets, at most INT_MAX by capacity check at sdk/rocketnet_encryption_map.cpp:84 |
| `sdk/rocketnet_endpoint.cpp:48` | int to uint32_t then widening to uint64_t, not narrowing |
| `sdk/rocketnet_endpoint.cpp:86` | source is int default_kbps checked positive on the line before |
| `sdk/rocketnet_endpoint.cpp:184` | source is int channel checked nonnegative at sdk/rocketnet_endpoint.cpp:177 |
| `sdk/rocketnet_endpoint.cpp:188` | source is int bytes checked at least 1 at sdk/rocketnet_endpoint.cpp:177, same width |
| `sdk/rocketnet_endpoint.cpp:228` | source is int channel checked nonnegative in the route at sdk/rocketnet_endpoint.cpp:177 |
| `sdk/rocketnet_endpoint.cpp:251` | source is int channel and bytes checked nonnegative in the route at sdk/rocketnet_endpoint.cpp:177 |
| `sdk/rocketnet_endpoint.cpp:278` | int channel below num_channels checked at sdk/rocketnet_endpoint.cpp:177, at most 65536 |
| `sdk/rocketnet_endpoint.cpp:279` | int reliability is a small type enum |
| `sdk/rocketnet_endpoint.cpp:288` | source is int bytes checked at least 1, same width |
| `sdk/rocketnet_endpoint.cpp:304` | source is int channel checked nonnegative in the route at sdk/rocketnet_endpoint.cpp:177 |
| `sdk/rocketnet_endpoint.cpp:324` | source is int bytes checked at least 1 on the same line |
| `sdk/rocketnet_endpoint.cpp:342` | uint32_t channel below num_channels, same width |
| `sdk/rocketnet_endpoint.cpp:350` | source is int bytes checked at least 1 at sdk/rocketnet_endpoint.cpp:324 |
| `sdk/rocketnet_endpoint.cpp:379` | uint32_t pending channel below num_channels, at most 65536 |
| `sdk/rocketnet_endpoint.cpp:380` | int pending_type is a small type enum |
| `sdk/rocketnet_endpoint.cpp:384` | source is int bytes checked at least 1 at sdk/rocketnet_endpoint.cpp:324 |
| `sdk/rocketnet_endpoint.cpp:468` | source is int bytes checked at least 1 at sdk/rocketnet_endpoint.cpp:448 |
| `sdk/rocketnet_endpoint.cpp:503` | source is int bytes checked at least 1 by the in-place header check |
| `sdk/rocketnet_endpoint.cpp:515` | source is int bytes checked at least 1 on the same line |
| `sdk/rocketnet_endpoint.cpp:524` | uint32_t pending_bytes set from a positive int at sdk/rocketnet_endpoint.cpp:503 |
| `sdk/rocketnet_endpoint.cpp:541` | source is int pending_channel checked nonnegative, same width |
| `sdk/rocketnet_endpoint.cpp:543` | source is int pending_channel checked nonnegative, same width |
| `sdk/rocketnet_endpoint.cpp:679` | uint32_t delivery count bounded by the region delivery entries |
| `sdk/rocketnet_endpoint.cpp:801` | uint32_t bytes at most max_message_bytes 1 << 20, same width |
| `sdk/rocketnet_endpoint.cpp:1016` | uint32_t payload_bytes at most the builder payload capacity ROCKETNET_MESSAGE_SINGLE_BYTES |
| `sdk/rocketnet_endpoint.cpp:1067` | uint32_t payload_bytes at most the builder payload capacity ROCKETNET_MESSAGE_SINGLE_BYTES |
| `sdk/rocketnet_endpoint.cpp:1081` | uint32_t bytes at most max_message_bytes 1 << 20, same width |
| `sdk/rocketnet_endpoint.cpp:1136` | source is int kbps checked positive at sdk/rocketnet_endpoint.cpp:1122 |
| `sdk/rocketnet_endpoint.cpp:1145` | source is int kbps checked positive at sdk/rocketnet_endpoint.cpp:1122 |
| `sdk/rocketnet_fragment.cpp:30` | source is int fragment_index, below fragment_count at most 256 by static_assert at sdk/rocketnet_fragment.cpp:17 |
| `sdk/rocketnet_fragment.cpp:31` | source is int fragment_count at most 256, minus one fits uint8_t |
| `sdk/rocketnet_fragment.cpp:102` | int widened to uint64_t, not narrowing |
| `sdk/rocketnet_fragment.cpp:146` | int64_t bounded by max_clients 1000 and slots 256 checked at sdk/rocketnet_fragment.cpp:142 |
| `sdk/rocketnet_fragment.cpp:161` | int64_t bounded by constant drain 1024 and slots 256 checked at sdk/rocketnet_fragment.cpp:157 |
| `sdk/rocketnet_fragment.cpp:220` | int widened to size_t, not narrowing |
| `sdk/rocketnet_fragment.cpp:245` | int widened to size_t, not narrowing |
| `sdk/rocketnet_fragment.cpp:294` | source is int index, a bit mask of 1 to 128 fits uint8_t |
| `sdk/rocketnet_fragment.cpp:350` | uint32_t modulo num_slots at most 256, result fits int |
| `sdk/rocketnet_fragment.cpp:351` | uint32_t modulo num_slots at most 256, widened to size_t |
| `sdk/rocketnet_fragment.h:86` | constant ROCKETNET_MAX_FRAGMENTS |
| `sdk/rocketnet_frame_sync.cpp:235` | source is kern_return_t, an int |
| `sdk/rocketnet_gateway_netlink_linux.cpp:52` | source is unsigned short rta_type masked with NLA_TYPE_MASK, widening not narrowing |
| `sdk/rocketnet_gateway_netlink_linux.cpp:65` | not a cast: a sizeof comparison or memcpy length |
| `sdk/rocketnet_gateway_netlink_linux.cpp:67` | not a cast: a sizeof comparison or memcpy length |
| `sdk/rocketnet_gateway_netlink_linux.cpp:79` | declaration: a function-pointer typedef, no narrowing |
| `sdk/rocketnet_gateway_netlink_linux.cpp:132` | not a cast: a sizeof comparison or memcpy length |
| `sdk/rocketnet_gateway_netlink_linux.cpp:223` | same-width sign cast of uint32 RTA_OIF; result is only compared with a positive interface_index (asserted > 0), negative never matches |
| `sdk/rocketnet_gateway_netlink_linux.cpp:330` | declaration: a function-pointer typedef, no narrowing |
| `sdk/rocketnet_gateway_netlink_linux.cpp:370` | NLMSG_LENGTH of sizeof(rtmsg) or sizeof(ndmsg), a compile-time constant |
| `sdk/rocketnet_internal.h:504` | Compile-time size macro using sizeof(int), not a narrowing |
| `sdk/rocketnet_internal.h:506` | Compile-time size macro using sizeof(int), not a narrowing |
| `sdk/rocketnet_internal.h:548` | Compile-time size macro using sizeof(int), not a narrowing |
| `sdk/rocketnet_internal.h:552` | Compile-time size macro using sizeof(int), not a narrowing |
| `sdk/rocketnet_internal.h:554` | Compile-time size macro using sizeof(int), not a narrowing |
| `sdk/rocketnet_internal.h:595` | Compile-time size macro using sizeof(int), not a narrowing |
| `sdk/rocketnet_internal.h:596` | Compile-time size macro using sizeof(int), not a narrowing |
| `sdk/rocketnet_internal.h:612` | Function pointer member declaration, not a narrowing |
| `sdk/rocketnet_keys.cpp:32` | sizeof of a 128-byte local buffer, a compile-time constant |
| `sdk/rocketnet_layers.h:26` | offsetof plus sizeof in a compile-time size macro, no cast |
| `sdk/rocketnet_layers.h:27` | offsetof plus sizeof in a compile-time size macro, no cast |
| `sdk/rocketnet_message.cpp:94` | Source is int code masked by ROCKETNET_MESSAGE_FLAG_CODE_MASK, fits uint8_t |
| `sdk/rocketnet_message.cpp:150` | uint32_t masked with 0xFF, always fits uint8_t |
| `sdk/rocketnet_message.cpp:151` | uint32_t shifted and masked with 0xFF, always fits uint8_t |
| `sdk/rocketnet_message.cpp:152` | uint32_t shifted and masked with 0xFF, always fits uint8_t |
| `sdk/rocketnet_message.cpp:158` | Widening uint8_t to uint32_t, not a narrowing |
| `sdk/rocketnet_message.cpp:159` | Widening uint8_t to uint32_t, not a narrowing |
| `sdk/rocketnet_message.cpp:160` | Widening uint8_t to uint32_t, not a narrowing |
| `sdk/rocketnet_message.cpp:214` | int bytes checked 1 .. 65535 at sdk/rocketnet_message.cpp:118 via the non-assert valid check at :201 |
| `sdk/rocketnet_message.cpp:218` | uint32_t ring checked at most 0xFFFF at sdk/rocketnet_message.cpp:128 via the valid check at :201 |
| `sdk/rocketnet_message.cpp:225` | uint32_t channel checked at most 0xFFFF at sdk/rocketnet_message.cpp:134 via the valid check at :201 |
| `sdk/rocketnet_message.cpp:227` | uint32_t channel below 256 on this branch since WIDE is set exactly when channel >= 256 at sdk/rocketnet_message.cpp:97 |
| `sdk/rocketnet_message.cpp:231` | uint32_t fragment at most fragments, at most 0xFFFF, checked at sdk/rocketnet_message.cpp:136 via :201 |
| `sdk/rocketnet_message.cpp:232` | uint32_t fragments checked at most 0xFFFF at sdk/rocketnet_message.cpp:136 via the valid check at :201 |
| `sdk/rocketnet_message.cpp:368` | uint32_t channel read from a wire u8 or u16, at most 65535, fits int |
| `sdk/rocketnet_message.cpp:387` | uint32_t fragments read from a wire u16, at most 65535, fits int |
| `sdk/rocketnet_message.cpp:406` | uint32_t ring and fragment read from wire u16 fields, at most 65535 each, fit int |
| `sdk/rocketnet_message.cpp:408` | uint32_t fragments from a wire u16 and k * F bounded by max_message_bytes at sdk/rocketnet_message.cpp:391 |
| `sdk/rocketnet_message.cpp:414` | uint32_t ring read from a wire u16, at most 65535, times 16 fits int |
| `sdk/rocketnet_message.cpp:535` | uint32_t version is the constant ROCKETNET_MESSAGE_CONFIG_VERSION set at sdk/rocketnet_channel.cpp:1049 |
| `sdk/rocketnet_message.cpp:536` | uint32_t enabled masked with 0xFF at sdk/rocketnet_channel.cpp:1050 |
| `sdk/rocketnet_message_client.cpp:220` | uint32 send_kbps <= ROCKETNET_SEND_BUDGET_MAX_KBPS checked at sdk/rocketnet_message_client.cpp:214 before the cast |
| `sdk/rocketnet_message_client.cpp:234` | same-width sign cast of int32 kbps, guarded by kbps <= 0 short-circuit on the same line |
| `sdk/rocketnet_message_object.h:56` | declaration of a function pointer, not a cast |
| `sdk/rocketnet_message_object.h:63` | declaration of a function pointer, not a cast |
| `sdk/rocketnet_message_object.h:77` | declaration of a function pointer, not a cast |
| `sdk/rocketnet_message_object.h:79` | declaration of a function pointer, not a cast |
| `sdk/rocketnet_message_object.h:87` | declaration of a function pointer, not a cast |
| `sdk/rocketnet_message_object.h:89` | declaration of a function pointer, not a cast |
| `sdk/rocketnet_message_object.h:102` | declaration of a function pointer, not a cast |
| `sdk/rocketnet_message_object.h:107` | declaration of a function pointer, not a cast |
| `sdk/rocketnet_message_server.cpp:102` | widening int num_channels to uint64, not narrowing |
| `sdk/rocketnet_message_server.cpp:230` | same-width sign cast of int32 default_kbps, guarded by default_kbps <= 0 short-circuit on the same line |
| `sdk/rocketnet_message_server.cpp:238` | default_kbps is already int32_t, an identity cast |
| `sdk/rocketnet_message_server.cpp:312` | pointer difference within connections[], bounded by int num_connections |
| `sdk/rocketnet_message_server.cpp:483` | not a cast: a memset length |
| `sdk/rocketnet_message_server.cpp:616` | not a cast: a memset length |
| `sdk/rocketnet_message_server.cpp:987` | same-width sign cast of int bytes, checked bytes >= 1 first on the same line |
| `sdk/rocketnet_message_server.cpp:998` | same-width sign cast of int pending_channel, validated 0 .. num_channels-1 at sdk/rocketnet_endpoint.cpp:177 |
| `sdk/rocketnet_message_server.cpp:1252` | int channel checked < num_channels at sdk/rocketnet_message_server.cpp:1241; num_channels is 1..32 (sdk/rocketnet_internal.h:577) |
| `sdk/rocketnet_message_server.cpp:1586` | uint32 per_client_send_kbps <= ROCKETNET_SEND_BUDGET_MAX_KBPS checked at sdk/rocketnet_message_server.cpp:1580 before the cast |
| `sdk/rocketnet_message_server.cpp:1601` | same-width sign cast of int32 kbps, guarded by kbps <= 0 short-circuit on the same line |
| `sdk/rocketnet_network_simulator.cpp:211` | deliberate byte fold of the address fields for an FNV hash, sources are bytes, port and type |
| `sdk/rocketnet_network_simulator.cpp:238` | uint32 hash masked with shard_mask; shard_count <= INT_MAX checked at sdk/rocketnet_network_simulator.cpp:278 |
| `sdk/rocketnet_network_simulator.cpp:281` | uint64 shard_count_rounded > INT_MAX refused at sdk/rocketnet_network_simulator.cpp:278 before the cast |
| `sdk/rocketnet_network_simulator.cpp:297` | pointer cast of malloc result; the int count widens to size_t, not narrowing |
| `sdk/rocketnet_network_simulator.cpp:301` | pointer cast of malloc result; the int count widens to size_t, not narrowing |
| `sdk/rocketnet_network_simulator.cpp:303` | pointer cast of malloc result; the int count widens to size_t, not narrowing |
| `sdk/rocketnet_network_simulator.cpp:304` | pointer cast of malloc result; the int count widens to size_t, not narrowing |
| `sdk/rocketnet_network_simulator.cpp:306` | pointer cast of malloc result; the int count widens to size_t, not narrowing |
| `sdk/rocketnet_network_simulator.cpp:307` | pointer cast of malloc result; the int count widens to size_t, not narrowing |
| `sdk/rocketnet_network_simulator.cpp:318` | int capacity widens to size_t for a memset length, not narrowing |
| `sdk/rocketnet_network_simulator.cpp:390` | not a cast: uint64 size products in an allocation list, no narrowing |
| `sdk/rocketnet_network_simulator.cpp:394` | not a cast: uint64 size products in an allocation list, no narrowing |
| `sdk/rocketnet_network_simulator.cpp:396` | not a cast: uint64 size products in an allocation list, no narrowing |
| `sdk/rocketnet_network_simulator.cpp:397` | not a cast: uint64 size products in an allocation list, no narrowing |
| `sdk/rocketnet_network_simulator.cpp:399` | not a cast: uint64 size products in an allocation list, no narrowing |
| `sdk/rocketnet_network_simulator.cpp:400` | not a cast: uint64 size products in an allocation list, no narrowing |
| `sdk/rocketnet_packet_builder.cpp:71` | same width sign cast of uint32_t item bytes, documented 1 to 65535 at sdk/rocketnet_packet_builder.h:50 |
| `sdk/rocketnet_packet_builder.cpp:84` | uint64_t masked to 24 bits by ROCKETNET_MESSAGE_U24_MASK before the cast |
| `sdk/rocketnet_packet_builder.cpp:96` | uint64_t masked to 24 bits by ROCKETNET_MESSAGE_U24_MASK before the cast |
| `sdk/rocketnet_packet_builder.cpp:109` | uint64_t masked to 24 bits by ROCKETNET_MESSAGE_U24_MASK before the cast |
| `sdk/rocketnet_packet_builder.cpp:190` | uint16_t channel widened to uint32_t, not narrowing |
| `sdk/rocketnet_packet_builder.cpp:210` | source is int loop index over num_items, same width |
| `sdk/rocketnet_packet_builder.cpp:475` | same width sign cast of int payload_bytes, checked positive at sdk/rocketnet_packet_builder.cpp:429 |
| `sdk/rocketnet_packet_builder.cpp:492` | same width cast of uint32_t packets, at most ROCKETNET_PACKET_BUILDER_MAX_PACKETS |
| `sdk/rocketnet_packet_builder.cpp:497` | same width cast of uint32_t packets, at most ROCKETNET_PACKET_BUILDER_MAX_PACKETS |
| `sdk/rocketnet_packet_builder.cpp:510` | same width cast of uint32_t packets, at most ROCKETNET_PACKET_BUILDER_MAX_PACKETS |
| `sdk/rocketnet_packet_builder.cpp:522` | same width cast of uint32_t packets, at most ROCKETNET_PACKET_BUILDER_MAX_PACKETS |
| `sdk/rocketnet_packet_builder.cpp:537` | same width cast of uint32_t packets, at most ROCKETNET_PACKET_BUILDER_MAX_PACKETS |
| `sdk/rocketnet_packet_loss_tracker.h:69` | uint64_t modulo ROCKETNET_PACKET_LOSS_TRACKER_HISTORY, bounded by the history array size |
| `sdk/rocketnet_packet_loss_tracker.h:99` | uint64_t modulo ROCKETNET_PACKET_LOSS_TRACKER_HISTORY, bounded by the history array size |
| `sdk/rocketnet_pending_frames.h:29` | count bounded by template Capacity via append check at line 18, ROCKETNET_XDP_RECV_QUEUE_SIZE 16384 |
| `sdk/rocketnet_pending_frames.h:30` | i below count, bounded by Capacity 16384 via append check at line 18 |
| `sdk/rocketnet_pending_frames.h:31` | count bounded by template Capacity via append check at line 18, ROCKETNET_XDP_RECV_QUEUE_SIZE 16384 |
| `sdk/rocketnet_platform.cpp:66` | double time to uint32_t, not integer narrowing |
| `sdk/rocketnet_platform.cpp:68` | double fraction times 1e6, not integer narrowing |
| `sdk/rocketnet_platform.cpp:109` | double milliseconds clamped to 0xFFFFFFFE at sdk/rocketnet_platform.cpp:107, not integer narrowing |
| `sdk/rocketnet_platform.cpp:144` | function declaration, no cast |
| `sdk/rocketnet_platform.cpp:626` | uint16_t byte swap masked to 16 bits, no narrowing |
| `sdk/rocketnet_platform.cpp:635` | uint16_t byte swap masked to 16 bits, no narrowing |
| `sdk/rocketnet_platform.cpp:747` | pointer cast to char star and sizeof int, not integer narrowing |
| `sdk/rocketnet_platform.cpp:752` | pointer cast to char star and sizeof int, not integer narrowing |
| `sdk/rocketnet_platform.cpp:1083` | ssize_t from send or sendto is at most the int packet_bytes or -1 |
| `sdk/rocketnet_platform.cpp:1098` | ssize_t from send or sendto is at most the int packet_bytes or -1 |
| `sdk/rocketnet_platform.cpp:1166` | ssize_t recvfrom with MSG_TRUNC is a UDP datagram size, at most 65535, or -1 |
| `sdk/rocketnet_platform.cpp:1288` | double timeout ceil to int, not integer narrowing |
| `sdk/rocketnet_platform.cpp:1380` | double timeout ceil to int, not integer narrowing |
| `sdk/rocketnet_platform.cpp:1533` | source is int, positive count at most ROCKETNET_MSS_BATCH 64, sign cast only |
| `sdk/rocketnet_platform.cpp:1611` | source is int want, clamped to ROCKETNET_MSS_BATCH at sdk/rocketnet_platform.cpp:1591 |
| `sdk/rocketnet_platform.cpp:1643` | same width cast of unsigned msg_len, a UDP datagram size at most 65535 |
| `sdk/rocketnet_platform.cpp:2170` | same width sign cast of int ifr_ifindex from the kernel |
| `sdk/rocketnet_platform.cpp:2488` | uint16_t byte swap masked to 16 bits, no narrowing |
| `sdk/rocketnet_platform.cpp:2497` | uint16_t byte swap masked to 16 bits, no narrowing |
| `sdk/rocketnet_platform.cpp:2689` | pointer cast to char star and sizeof int, not integer narrowing |
| `sdk/rocketnet_platform.cpp:2696` | pointer cast to char star and sizeof int, not integer narrowing |
| `sdk/rocketnet_platform.cpp:3012` | ssize_t from send or sendto is at most the int packet_bytes or -1 |
| `sdk/rocketnet_platform.cpp:3029` | ssize_t from send or sendto is at most the int packet_bytes or -1 |
| `sdk/rocketnet_platform.cpp:3140` | ssize_t recvmsg of one UDP datagram, at most 65535, or -1 |
| `sdk/rocketnet_platform.cpp:3462` | ssize_t sendmsg_x message count at most count - offset, ROCKETNET_MSGX_BATCH 32 |
| `sdk/rocketnet_platform.cpp:3613` | size_t msg_datalen is a UDP datagram size, at most 65535 |
| `sdk/rocketnet_platform.cpp:3617` | ssize_t i below result, at most ROCKETNET_MSGX_BATCH 32 |
| `sdk/rocketnet_platform.cpp:3759` | double time clamped to 86400 at sdk/rocketnet_platform.cpp:3757, not integer narrowing |
| `sdk/rocketnet_platform.cpp:4019` | same width cast of MMRESULT (UINT) to unsigned int |
| `sdk/rocketnet_platform.cpp:4024` | same width cast of MMRESULT (UINT) to unsigned int |
| `sdk/rocketnet_platform.cpp:4396` | uint16_t byte swap masked to 16 bits, no narrowing |
| `sdk/rocketnet_platform.cpp:4405` | uint16_t byte swap masked to 16 bits, no narrowing |
| `sdk/rocketnet_platform.cpp:4621` | pointer cast to char star and sizeof int, not integer narrowing |
| `sdk/rocketnet_platform.cpp:4628` | pointer cast to char star and sizeof int, not integer narrowing |
| `sdk/rocketnet_platform.cpp:4806` | sizeof sockaddr_in6, constant |
| `sdk/rocketnet_platform.cpp:4813` | sizeof sockaddr_in, constant |
| `sdk/rocketnet_platform.cpp:4855` | sizeof sockaddr_in6, constant |
| `sdk/rocketnet_platform.cpp:4865` | sizeof sockaddr_in, constant |
| `sdk/rocketnet_platform.cpp:4951` | DWORD bytes_received to int is not wider, a UDP datagram size at most 65535 |
| `sdk/rocketnet_platform.cpp:5209` | POSIX socket returns int, same type cast |
| `sdk/rocketnet_platform.h:528` | typedef declaration, no cast |
| `sdk/rocketnet_platform.h:774` | function pointer declaration, no cast |
| `sdk/rocketnet_platform.h:776` | function pointer declaration, no cast |
| `sdk/rocketnet_platform.h:778` | function pointer declaration, no cast |
| `sdk/rocketnet_platform.h:780` | function pointer declaration, no cast |
| `sdk/rocketnet_platform.h:784` | function pointer declaration, no cast |
| `sdk/rocketnet_platform.h:786` | function pointer declaration, no cast |
| `sdk/rocketnet_protocol.cpp:198` | uint64_t sequence explicitly masked to 32 bits for the wire field |
| `sdk/rocketnet_protocol.cpp:223` | header pointer delta is constant and sealed_bytes bounded by ROCKETNET_SEND_BUDGET check at sdk/rocketnet_protocol.cpp:192 |
| `sdk/rocketnet_protocol.cpp:260` | uint64_t sequence explicitly masked to 32 bits for the wire field |
| `sdk/rocketnet_protocol.cpp:280` | data_bytes is int, callers cap it near ROCKETNET_MAX_PACKET_PAYLOAD_BYTES, e.g. sdk/rocketnet_client_protocol.cpp:1496 |
| `sdk/rocketnet_protocol.cpp:342` | opened_bytes is at most packet_bytes - header_bytes, an int checked at sdk/rocketnet_protocol.cpp:316 |
| `sdk/rocketnet_protocol.cpp:458` | pointer delta of a fixed address encoding, at most 19 bytes |
| `sdk/rocketnet_protocol.cpp:477` | pointer delta of fixed address plus salt plus 8 bytes, small constant bound |
| `sdk/rocketnet_read_write.h:28` | intended byte serialization, each value masked with 0xFF before the cast |
| `sdk/rocketnet_read_write.h:29` | intended byte serialization, each value masked with 0xFF before the cast |
| `sdk/rocketnet_read_write.h:35` | intended byte serialization, each value masked with 0xFF before the cast |
| `sdk/rocketnet_read_write.h:36` | intended byte serialization, each value masked with 0xFF before the cast |
| `sdk/rocketnet_read_write.h:37` | intended byte serialization, each value masked with 0xFF before the cast |
| `sdk/rocketnet_read_write.h:38` | intended byte serialization, each value masked with 0xFF before the cast |
| `sdk/rocketnet_read_write.h:44` | intended byte serialization, each value masked with 0xFF before the cast |
| `sdk/rocketnet_read_write.h:45` | intended byte serialization, each value masked with 0xFF before the cast |
| `sdk/rocketnet_read_write.h:46` | intended byte serialization, each value masked with 0xFF before the cast |
| `sdk/rocketnet_read_write.h:47` | intended byte serialization, each value masked with 0xFF before the cast |
| `sdk/rocketnet_read_write.h:48` | intended byte serialization, each value masked with 0xFF before the cast |
| `sdk/rocketnet_read_write.h:49` | intended byte serialization, each value masked with 0xFF before the cast |
| `sdk/rocketnet_read_write.h:50` | intended byte serialization, each value masked with 0xFF before the cast |
| `sdk/rocketnet_read_write.h:51` | intended byte serialization, each value masked with 0xFF before the cast |
| `sdk/rocketnet_read_write.h:76` | int built from two bytes, at most 0xFFFF, fits uint16 |
| `sdk/rocketnet_read_write.h:83` | widening a byte to uint32, not narrowing |
| `sdk/rocketnet_read_write.h:84` | widening a byte to uint32, not narrowing |
| `sdk/rocketnet_read_write.h:85` | widening a byte to uint32, not narrowing |
| `sdk/rocketnet_read_write.h:86` | widening a byte to uint32, not narrowing |
| `sdk/rocketnet_read_write.h:130` | int built from two bytes, at most 0xFFFF, fits uint16 |
| `sdk/rocketnet_read_write.h:139` | widening a byte to uint32, not narrowing |
| `sdk/rocketnet_read_write.h:140` | widening a byte to uint32, not narrowing |
| `sdk/rocketnet_read_write.h:141` | widening a byte to uint32, not narrowing |
| `sdk/rocketnet_read_write.h:142` | widening a byte to uint32, not narrowing |
| `sdk/rocketnet_replay.cpp:13` | uint64_t modulo ROCKETNET_REPLAY_WINDOW_BITS 4096, result under 4096 |
| `sdk/rocketnet_replay.cpp:19` | uint64_t modulo ROCKETNET_REPLAY_WINDOW_BITS 4096, result under 4096 |
| `sdk/rocketnet_replay.cpp:25` | uint64_t modulo ROCKETNET_REPLAY_WINDOW_BITS 4096, result under 4096 |
| `sdk/rocketnet_replay.h:29` | Compile-time word count of the replay window inside a static_assert |
| `sdk/rocketnet_rio_layout.h:154` | source is int kind or phase, not a wider integer |
| `sdk/rocketnet_rio_layout.h:155` | source is int kind or phase, not a wider integer |
| `sdk/rocketnet_rio_layout.h:161` | uint64 context masked with 0xFF before the cast |
| `sdk/rocketnet_rio_layout.h:166` | uint64 context masked with 0xFF before the cast |
| `sdk/rocketnet_rio_layout.h:171` | masked to 32 bits; the index was packed from a uint32 slot and callers range-check it (sdk/rocketnet_server_rio_windows.cpp:393, :836) |
| `sdk/rocketnet_route.h:39` | widening uint8_t to uint32_t, function-style cast |
| `sdk/rocketnet_route.h:83` | RTA_ALIGN of a u16 rta_len, at most 0xFFFF plus 3 |
| `sdk/rocketnet_route.h:118` | u32 nlmsg_len minus header, at least zero by the check at sdk/rocketnet_route.h:104, a value past INT_MAX goes negative and RTA_OK refuses |
| `sdk/rocketnet_server_gso_linux.cpp:78` | declaration with sizeof in an array bound, not a cast |
| `sdk/rocketnet_server_gso_linux.cpp:158` | declaration with sizeof in an array bound, not a cast |
| `sdk/rocketnet_server_gso_linux.cpp:206` | declaration with sizeof in an array bound, not a cast |
| `sdk/rocketnet_server_gso_linux.cpp:508` | sizeof argument to setsockopt, not a cast |
| `sdk/rocketnet_server_gso_linux.cpp:515` | sizeof argument to setsockopt, not a cast |
| `sdk/rocketnet_server_gso_linux.cpp:524` | sizeof assigned to socklen_t, constant |
| `sdk/rocketnet_server_gso_linux.cpp:526` | sizeof assigned to socklen_t, constant |
| `sdk/rocketnet_server_gso_linux.cpp:643` | sizeof in a comparison, not a cast |
| `sdk/rocketnet_server_gso_linux.cpp:732` | same width unsigned msg_len, at most iov_len ROCKETNET_GSO_RECV_BUFFER_BYTES 65536 set at line 679 |
| `sdk/rocketnet_server_gso_linux.cpp:1209` | offset checked below sizeof the bank data at sdk/rocketnet_server_gso_linux.cpp:1206, then slot range checked |
| `sdk/rocketnet_server_gso_linux.cpp:1231` | offset checked below sizeof the bank data at sdk/rocketnet_server_gso_linux.cpp:1228, then slot range checked |
| `sdk/rocketnet_server_gso_linux.cpp:1354` | constant CMSG_SPACE of sizeof, not narrowing |
| `sdk/rocketnet_server_gso_linux.cpp:1358` | constant CMSG_LEN of sizeof, not narrowing |
| `sdk/rocketnet_server_gso_linux.cpp:1359` | int segment_bytes, run_count above 1 needs run total within 65000 at sdk/rocketnet_server_gso_linux.cpp:1301 |
| `sdk/rocketnet_server_gso_linux.cpp:1396` | int difference positive by the loop condition sent below num_messages, at most 128 |
| `sdk/rocketnet_server_iocp_windows.cpp:65` | not a cast: sizeof terms in a size macro |
| `sdk/rocketnet_server_iocp_windows.cpp:84` | not a cast: sizeof terms in a size macro |
| `sdk/rocketnet_server_iocp_windows.cpp:212` | intended byte split of a uint16 IPv6 word |
| `sdk/rocketnet_server_iocp_windows.cpp:213` | intended byte split of a uint16 IPv6 word |
| `sdk/rocketnet_server_iocp_windows.cpp:234` | sizeof of a sockaddr type, a compile-time constant |
| `sdk/rocketnet_server_iocp_windows.cpp:242` | sizeof of a sockaddr type, a compile-time constant |
| `sdk/rocketnet_server_iocp_windows.cpp:248` | int built from two bytes, at most 0xFFFF, fits uint16 |
| `sdk/rocketnet_server_iocp_windows.cpp:359` | same-width DWORD to int; bytes is bounded by the posted ROCKETNET_IOCP_URO_BUFFER_BYTES receive length (sdk/rocketnet_server_iocp_windows.cpp:277) |
| `sdk/rocketnet_server_iocp_windows.cpp:360` | same-width DWORD stride, used only when stride < bytes on the same line |
| `sdk/rocketnet_server_iocp_windows.cpp:509` | DWORD bytes <= ROCKETNET_RECEIVE_CEILING checked at sdk/rocketnet_server_iocp_windows.cpp:503 |
| `sdk/rocketnet_server_iocp_windows.cpp:546` | uint64 count clamped to ROCKETNET_IOCP_SEND_QUEUE_SIZE at sdk/rocketnet_server_iocp_windows.cpp:545 |
| `sdk/rocketnet_server_iocp_windows.cpp:958` | count comes from int bank->count, at most ROCKETNET_IOCP_SEND_QUEUE_SIZE, so count - i fits int |
| `sdk/rocketnet_server_iocp_windows.cpp:987` | uint64 count clamped to ROCKETNET_IOCP_SEND_QUEUE_SIZE at sdk/rocketnet_server_iocp_windows.cpp:979 |
| `sdk/rocketnet_server_protocol.cpp:390` | same width sign cast of uint32_t wire field, negative result refused at sdk/rocketnet_server_protocol.cpp:391 |
| `sdk/rocketnet_server_protocol.cpp:1457` | int packet_type in 0..FRAGMENT checked at sdk/rocketnet_server_protocol.cpp:1423 and :1433 |
| `sdk/rocketnet_server_protocol.cpp:1729` | same width sign cast of non-negative int client_index of a connected mapping |
| `sdk/rocketnet_server_protocol.cpp:1730` | same width sign cast of int max_clients, at most 1000 |
| `sdk/rocketnet_server_protocol.cpp:1801` | source is uint8_t keep_alive_acked, widening |
| `sdk/rocketnet_server_protocol.cpp:2071` | 1 + (raw - 1) modulo 0xFFFFFFFF is at most 0xFFFFFFFF, fold is deliberate |
| `sdk/rocketnet_server_replay_marks.cpp:135` | intentional low 32 bits matching the u32 wire sequence, XDP compares serially at server_xdp/server_xdp.c:391 |
| `sdk/rocketnet_server_rio_windows.cpp:49` | macro of compile-time sizeof terms, not a narrowing cast |
| `sdk/rocketnet_server_rio_windows.cpp:250` | uint16_t ipv6 word shifted right 8, deliberate byte split |
| `sdk/rocketnet_server_rio_windows.cpp:251` | uint16_t ipv6 word low byte, deliberate byte split |
| `sdk/rocketnet_server_rio_windows.cpp:283` | two uint8_t bytes combined, at most 0xFFFF |
| `sdk/rocketnet_server_rio_windows.cpp:338` | offset / stride below ROCKETNET_RIO_SEND_FRAMES checked at sdk/rocketnet_server_rio_windows.cpp:334 |
| `sdk/rocketnet_server_rio_windows.cpp:366` | same width sign cast of int slice, a bank index below ROCKETNET_RIO_RECEIVES |
| `sdk/rocketnet_server_rio_windows.cpp:372` | same width sign cast of int slice, a bank index below ROCKETNET_RIO_RECEIVES |
| `sdk/rocketnet_server_rio_windows.cpp:425` | ULONG is 32 bits on Windows, same width, count at most ROCKETNET_RIO_DEQUEUE_BATCH after RIO_CORRUPT_CQ check |
| `sdk/rocketnet_server_rio_windows.cpp:483` | ULONG is 32 bits on Windows, count at most the batch after RIO_CORRUPT_CQ check at line 482 |
| `sdk/rocketnet_server_rio_windows.cpp:544` | compile-time sizeof of SOCKADDR_INET |
| `sdk/rocketnet_server_rio_windows.cpp:547` | widening of a compile-time constant to uint64_t |
| `sdk/rocketnet_server_rio_windows.cpp:557` | compile-time sizeof of SOCKADDR_INET |
| `sdk/rocketnet_server_rio_windows.cpp:606` | pointer cast of malloc result, compile-time size, not integer narrowing |
| `sdk/rocketnet_server_rio_windows.cpp:608` | compile-time size expression, not a narrowing cast |
| `sdk/rocketnet_server_rio_windows.cpp:627` | compile-time sizeof of a sockaddr structure |
| `sdk/rocketnet_server_rio_windows.cpp:649` | compile-time sizeof of SOCKADDR_INET |
| `sdk/rocketnet_server_rio_windows.cpp:813` | same width sign cast of int slot below ROCKETNET_RIO_SEND_QUEUE_SIZE |
| `sdk/rocketnet_server_rio_windows.cpp:849` | LONG is 32 bits on Windows, same width as int |
| `sdk/rocketnet_server_rio_windows.cpp:857` | ULONG is 32 bits on Windows, range checked against ROCKETNET_RECEIVE_CEILING at sdk/rocketnet_server_rio_windows.cpp:860 |
| `sdk/rocketnet_server_rio_windows.cpp:929` | ULONG count at most batch, which is at most room, an int |
| `sdk/rocketnet_server_rio_windows.cpp:930` | ULONG count at most batch, which is at most room, an int |
| `sdk/rocketnet_server_rio_windows.cpp:1107` | ULONG index modulo ROCKETNET_RIO_RECEIVES, same width and bounded |
| `sdk/rocketnet_server_rio_windows.cpp:1130` | compile-time sizeof of SOCKADDR_INET |
| `sdk/rocketnet_server_rio_windows.cpp:1133` | pointer cast of calloc result, not integer narrowing |
| `sdk/rocketnet_server_rio_windows.cpp:1250` | ULONG index modulo ROCKETNET_RIO_RECEIVES, same width and bounded |
| `sdk/rocketnet_server_socket_linux.cpp:341` | long from strtol over at most two hex digits between forced NULs from a kernel sysfs mac, at most 0xFF |
| `sdk/rocketnet_server_socket_linux.cpp:342` | long from strtol over at most two hex digits between forced NULs from a kernel sysfs mac, at most 0xFF |
| `sdk/rocketnet_server_socket_linux.cpp:343` | long from strtol over at most two hex digits between forced NULs from a kernel sysfs mac, at most 0xFF |
| `sdk/rocketnet_server_socket_linux.cpp:344` | long from strtol over at most two hex digits between forced NULs from a kernel sysfs mac, at most 0xFF |
| `sdk/rocketnet_server_socket_linux.cpp:345` | long from strtol over at most two hex digits between forced NULs from a kernel sysfs mac, at most 0xFF |
| `sdk/rocketnet_server_socket_linux.cpp:346` | long from strtol over at most two hex digits between forced NULs from a kernel sysfs mac, at most 0xFF |
| `sdk/rocketnet_server_socket_linux.cpp:451` | long from strtol of two hex digits validated at sdk/rocketnet_server_socket_linux.cpp:424-431, at most 0xFF |
| `sdk/rocketnet_server_socket_linux.cpp:452` | long from strtol of two hex digits validated at sdk/rocketnet_server_socket_linux.cpp:424-431, at most 0xFF |
| `sdk/rocketnet_server_socket_linux.cpp:453` | long from strtol of two hex digits validated at sdk/rocketnet_server_socket_linux.cpp:424-431, at most 0xFF |
| `sdk/rocketnet_server_socket_linux.cpp:454` | long from strtol of two hex digits validated at sdk/rocketnet_server_socket_linux.cpp:424-431, at most 0xFF |
| `sdk/rocketnet_server_socket_linux.cpp:455` | long from strtol of two hex digits validated at sdk/rocketnet_server_socket_linux.cpp:424-431, at most 0xFF |
| `sdk/rocketnet_server_socket_linux.cpp:456` | long from strtol of two hex digits validated at sdk/rocketnet_server_socket_linux.cpp:424-431, at most 0xFF |
| `sdk/rocketnet_server_socket_linux.cpp:524` | uint16_t shifted right 8, always fits uint8_t |
| `sdk/rocketnet_server_socket_linux.cpp:525` | uint16_t masked with 0xFF, always fits uint8_t |
| `sdk/rocketnet_server_socket_linux.cpp:606` | size_t value_size is sizeof a local buffer of at most 1024 bytes at every caller (lines 897-1472) |
| `sdk/rocketnet_server_socket_linux.cpp:647` | long from strtol of kernel cpu ids in a sysfs cpu list, bounded by the kernel cpu count |
| `sdk/rocketnet_server_socket_linux.cpp:655` | long from strtol of kernel cpu ids in a sysfs cpu list, bounded by the kernel cpu count |
| `sdk/rocketnet_server_socket_linux.cpp:677` | long from strtol of kernel cpu ids in a sysfs cpu list, bounded by the kernel cpu count |
| `sdk/rocketnet_server_socket_linux.cpp:685` | long from strtol of kernel cpu ids in a sysfs cpu list, bounded by the kernel cpu count |
| `sdk/rocketnet_server_socket_linux.cpp:873` | Same width sign cast of unsigned int interface index, a kernel ifindex that is an int |
| `sdk/rocketnet_server_socket_linux.cpp:1018` | sizeof of a 1024-byte local array, compile-time constant |
| `sdk/rocketnet_server_socket_linux.cpp:1126` | Same width sign cast of ethtool __u32 channel counts compared to num_queues |
| `sdk/rocketnet_server_socket_linux.cpp:1137` | Same width sign cast of ethtool __u32 combined count compared to num_queues |
| `sdk/rocketnet_server_socket_linux.cpp:1311` | Same width sign cast of unsigned int interface index, a kernel ifindex that is an int |
| `sdk/rocketnet_server_socket_linux.cpp:1462` | sizeof of a 64-byte local array, compile-time constant |
| `sdk/rocketnet_server_socket_linux.cpp:1487` | pid_t is int on Linux, same width |
| `sdk/rocketnet_server_socket_linux.cpp:1835` | __u32 reserve result is at most the requested ROCKETNET_XDP_FILL_FRAMES (16384), fits int |
| `sdk/rocketnet_server_socket_linux.cpp:1852` | Loop int i in 0 .. ROCKETNET_XDP_FILL_FRAMES-1, nonnegative, widened to uint32_t |
| `sdk/rocketnet_server_socket_linux.cpp:2061` | ptrdiff_t slot checked 0 .. ROCKETNET_XDP_SEND_QUEUE_SIZE-1 at sdk/rocketnet_server_socket_linux.cpp:2056-2060 before the cast |
| `sdk/rocketnet_server_socket_linux.cpp:2081` | uint16_t port masked with 0xFF, fits uint8_t |
| `sdk/rocketnet_server_socket_linux.cpp:2083` | uint16_t port shifted right 8, fits uint8_t |
| `sdk/rocketnet_server_socket_linux.cpp:2085` | uint64_t modulo num_queues, which is 1 .. 1024 by sdk/rocketnet_server_transport.cpp:933 and sdk/rocketnet_driver_builtin.cpp:1049 |
| `sdk/rocketnet_server_socket_linux.cpp:2169` | uint64_t modulo num_queues, which is 1 .. 1024 by sdk/rocketnet_server_transport.cpp:933 and sdk/rocketnet_driver_builtin.cpp:1049 |
| `sdk/rocketnet_server_socket_linux.cpp:2222` | uint64_t modulo num_queues, which is 1 .. 1024 by sdk/rocketnet_server_transport.cpp:933 and sdk/rocketnet_driver_builtin.cpp:1049 |
| `sdk/rocketnet_server_socket_linux.cpp:2688` | long from sysconf online cpu count, small, and read only by an assert |
| `sdk/rocketnet_server_socket_linux.cpp:2842` | size_t packet_bytes stored from a positive int payload_bytes at sdk/rocketnet_server_socket_linux.cpp:2997, bounded by the 4096-byte frame |
| `sdk/rocketnet_server_socket_linux.cpp:2922` | size_t available() is at most ROCKETNET_XDP_RECV_QUEUE_SIZE capacity (sdk/rocketnet_pending_frames.h:14), fits uint32_t |
| `sdk/rocketnet_server_socket_linux.cpp:2923` | size_t available() is at most ROCKETNET_XDP_RECV_QUEUE_SIZE capacity (sdk/rocketnet_pending_frames.h:14), fits uint32_t |
| `sdk/rocketnet_server_socket_linux.cpp:2989` | uint32_t num_packets below ROCKETNET_XDP_RECV_QUEUE_SIZE (16384) by the peek limit at sdk/rocketnet_server_socket_linux.cpp:2921-2924 |
| `sdk/rocketnet_server_socket_linux.cpp:3039` | unsigned num_completed at most XSK_RING_CONS__DEFAULT_NUM_DESCS requested at sdk/rocketnet_server_socket_linux.cpp:3035, fits int |
| `sdk/rocketnet_server_socket_linux.cpp:3041` | Loop int i nonnegative and below num_completed, widened to uint32_t |
| `sdk/rocketnet_server_socket_linux.cpp:3167` | __u32 reserve result is at most num_batch_packets, at most ROCKETNET_XDP_SEND_BATCH_SIZE (32), fits int |
| `sdk/rocketnet_server_socket_linux.cpp:3176` | Loop int i nonnegative and below send_packets, widened to uint32_t |
| `sdk/rocketnet_server_socket_linux.cpp:3183` | int send_packets equals num_batch_packets, 1 .. 32, on this branch |
| `sdk/rocketnet_server_transport.cpp:129` | compile-time array size expression, not a cast |
| `sdk/rocketnet_server_transport.cpp:363` | compile-time element count of a small static array |
| `sdk/rocketnet_server_transport.cpp:575` | result is below int send_capacity because used is less than it, checked at sdk/rocketnet_server_transport.cpp:574 |
| `sdk/rocketnet_server_transport.cpp:2408` | seals added in one drain are at most max_seals 64 by the loop bound at sdk/rocketnet_server_transport.cpp:3198 |
| `sdk/rocketnet_server_uring.h:214` | declaration of a function pointer, not a cast |
| `sdk/rocketnet_server_uring.h:216` | declaration of a function pointer, not a cast |
| `sdk/rocketnet_server_uring_linux.cpp:266` | int expression, test_sqes_queued capped at ROCKETNET_URING_ENTRIES at sdk/rocketnet_server_uring_linux.cpp:252, nonnegative |
| `sdk/rocketnet_server_uring_linux.cpp:275` | Same width sign cast of int test_sqes_queued, 0 .. ROCKETNET_URING_ENTRIES |
| `sdk/rocketnet_server_uring_linux.cpp:302` | unsigned max capped at ROCKETNET_URING_CQE_BATCH (256) at sdk/rocketnet_server_uring_linux.cpp:300-301 |
| `sdk/rocketnet_server_uring_linux.cpp:305` | unsigned max capped at ROCKETNET_URING_CQE_BATCH (256) at sdk/rocketnet_server_uring_linux.cpp:300-301 |
| `sdk/rocketnet_server_uring_linux.cpp:306` | unsigned max capped at ROCKETNET_URING_CQE_BATCH (256) at sdk/rocketnet_server_uring_linux.cpp:300-301 |
| `sdk/rocketnet_server_uring_linux.cpp:309` | int count clamped to 0 .. max at sdk/rocketnet_server_uring_linux.cpp:303-306, same width |
| `sdk/rocketnet_server_uring_linux.cpp:320` | Same width sign cast of unsigned count, at most ROCKETNET_URING_CQE_BATCH from the peek |
| `sdk/rocketnet_server_uring_linux.cpp:333` | Same width sign cast of int ready, guarded ready > 0 |
| `sdk/rocketnet_server_uring_linux.cpp:426` | sizeof(int) constant 4 as socklen_t, not a narrowing |
| `sdk/rocketnet_server_uring_linux.cpp:433` | sizeof(int) constant 4 as socklen_t, not a narrowing |
| `sdk/rocketnet_server_uring_linux.cpp:442` | sizeof(int) constant 4 as socklen_t, not a narrowing |
| `sdk/rocketnet_server_uring_linux.cpp:444` | sizeof(int) constant 4 as socklen_t, not a narrowing |
| `sdk/rocketnet_server_uring_linux.cpp:494` | socklen_t bound_length at most sizeof(struct sockaddr_storage) (128), fits int |
| `sdk/rocketnet_server_uring_linux.cpp:551` | Source is int bgid, a queue index below ROCKETNET_URING_MAX_QUEUES (32) |
| `sdk/rocketnet_server_uring_linux.cpp:576` | Source is int bgid, a queue index below ROCKETNET_URING_MAX_QUEUES (32) |
| `sdk/rocketnet_server_uring_linux.cpp:674` | Source is int queue_index below ROCKETNET_URING_MAX_QUEUES (32), checked at sdk/rocketnet_driver_builtin.cpp:1015 |
| `sdk/rocketnet_server_uring_linux.cpp:708` | msg_namelen and msg_controllen set to constants at sdk/rocketnet_server_uring_linux.cpp:1334-1335 (sockaddr_storage size, GRO control bytes) |
| `sdk/rocketnet_server_uring_linux.cpp:910` | Source is int budget - total, positive inside the loop |
| `sdk/rocketnet_server_uring_linux.cpp:919` | unsigned count at most ROCKETNET_URING_CQE_BATCH (256) from the capped peek |
| `sdk/rocketnet_server_uring_linux.cpp:1148` | Source is int b, an intentional test-hook fill byte pattern, not wider-integer narrowing |
| `sdk/rocketnet_server_uring_linux.cpp:1512` | ptrdiff_t quotient may truncate, but the slot root identity check at sdk/rocketnet_server_uring_linux.cpp:1513 refuses any wrong slot |
| `sdk/rocketnet_server_uring_linux.cpp:1600` | Source is int sends_pending, not wider-integer narrowing |
| `sdk/rocketnet_server_uring_linux.cpp:1604` | uint64_t modulo num_queues, at most ROCKETNET_URING_MAX_QUEUES (32) by sdk/rocketnet_driver_builtin.cpp:1015 |
| `sdk/rocketnet_server_uring_linux.cpp:1651` | sizeof of a fixed control buffer, compile-time constant |
| `sdk/rocketnet_server_uring_linux.cpp:1669` | Source is int slot, validated 0 .. ROCKETNET_URING_SEND_QUEUE_SIZE-1, widened sign cast |
| `sdk/rocketnet_stats_window.h:16` | template size_t N is a compile-time bucket count, small constant |
| `sdk/rocketnet_token_tables.cpp:37` | declaration of a pointer to fixed-size array, not a cast |
| `sdk/rocketnet_token_tables.cpp:64` | no cast, uint64_t size arithmetic, not narrowing |
| `sdk/rocketnet_token_tables.cpp:67` | no cast, uint64_t size arithmetic, not narrowing |
| `sdk/rocketnet_token_tables.cpp:94` | int widened to size_t, not narrowing |
| `sdk/rocketnet_token_tables.cpp:95` | int widened to size_t, not narrowing |
| `sdk/rocketnet_token_tables.cpp:96` | pointer cast of malloc result with size_t widening, not narrowing |
| `sdk/rocketnet_token_tables.cpp:150` | uint64_t modulo num_buckets, num_buckets at most INT_MAX checked at sdk/rocketnet_token_tables.cpp:81 |
| `sdk/rocketnet_uring_codec.h:44` | macro CMSG_SPACE of sizeof uint16_t, constant |
| `sdk/rocketnet_uring_codec.h:48` | macro CMSG_SPACE of sizeof int, constant |
| `sdk/rocketnet_uring_codec.h:66` | int kind is one of the tag constants 1 to 3 at sdk/rocketnet_uring_codec.h:51 |
| `sdk/rocketnet_uring_codec.h:67` | int queue below ROCKETNET_URING_MAX_QUEUES 32, checked at sdk/rocketnet_server_uring_linux.cpp:1186 |
| `sdk/rocketnet_uring_codec.h:73` | uint64_t masked to 8 bits before the cast |
| `sdk/rocketnet_uring_codec.h:78` | uint64_t masked to 16 bits before the cast |
| `sdk/rocketnet_uring_codec.h:83` | 32-bit field, written slot below 2048 and range-checked by the reader at sdk/rocketnet_server_uring_linux.cpp:829 |
| `sdk/rocketnet_uring_codec.h:91` | uint32_t shifted right by IORING_CQE_BUFFER_SHIFT 16, at most 0xFFFF |
| `sdk/rocketnet_uring_codec.h:111` | sizeof struct cmsghdr, constant |
| `sdk/rocketnet_uring_codec.h:123` | comparison against CMSG_LEN of sizeof int, no cast |
| `sdk/rocketnet_uring_codec.h:133` | size_t cmsg_len checked at most control_bytes - offset at sdk/rocketnet_uring_codec.h:115, aligned fits int |
| `sdk/rocketnet_uring_codec.h:158` | sizeof struct io_uring_recvmsg_out, constant |
| `sdk/rocketnet_uring_codec.h:161` | int msg_namelen checked nonnegative at sdk/rocketnet_uring_codec.h:155, widened to uint32_t |
| `sdk/rocketnet_uring_codec.h:165` | sizeof struct io_uring_recvmsg_out, constant |
| `sdk/rocketnet_uring_codec.h:166` | same width sign cast of uint32_t payloadlen, kernel bounds it by the provided buffer |
| `sdk/rocketnet_uring_codec.h:169` | uint32_t namelen checked at most msg_namelen at sdk/rocketnet_uring_codec.h:161 |
| `sdk/rocketnet_uring_codec.h:171` | uint32_t payloadlen checked at most payload_left at sdk/rocketnet_uring_codec.h:166 |
| `sdk/rocketnet_uring_codec.h:174` | uint32_t controllen cast only when below msg_controllen, an int |
| `sdk/rocketnet_uring_codec.h:193` | sizeof sockaddr_in6, constant |
| `sdk/rocketnet_uring_codec.h:204` | sizeof sockaddr_in, constant |
| `sdk/rocketnet_uring_codec.h:232` | macro ROCKETNET_URING_GSO_CONTROL_BYTES, constant |
| `sdk/rocketnet_uring_codec.h:260` | CMSG_LEN of sizeof uint16_t, constant |
| `sdk/rocketnet_uring_codec.h:261` | int stride, run over 1 keeps run bytes at most max_super_bytes 65000 (sdk/rocketnet_coalesce.h:83), fits uint16_t |
| `sdk/rocketnet_window_ack.cpp:166` | h modulo a uint32_t cap is below cap |
| `sdk/rocketnet_window_ack.cpp:287` | masked by window - 1, a uint32_t power of two |
| `sdk/rocketnet_window_ack.cpp:480` | masked by W - 1, a uint32_t power of two |
| `sdk/rocketnet_window_ack.cpp:524` | modulo uint32_t P is below P |
| `sdk/rocketnet_window_ack.cpp:525` | modulo uint32_t max_refs is below max_refs |
| `sdk/rocketnet_window_ack.cpp:527` | deliberate low 32 bits of the number, matched against the live window at sdk/rocketnet_window_ack.cpp:611 |
| `sdk/rocketnet_window_ack.cpp:536` | uint32_t count at most 16, callers pass 16-entry arrays, e.g. sdk/rocketnet_endpoint.h:102 |
| `sdk/rocketnet_window_ack.cpp:537` | uint32_t receipt_count at most 16, callers pass 16-entry arrays, e.g. sdk/rocketnet_message_object.h:169 |
| `sdk/rocketnet_window_ack.cpp:611` | deliberate low 32 bits compare, with slot->number range checked against U and next on the same line |
| `sdk/rocketnet_window_ack.cpp:722` | available at most receipt_entries, set from int max_message_receipts at sdk/rocketnet_allocator.cpp:435, ring kept by line 429 |
| `sdk/rocketnet_window_ack.cpp:757` | masked by W - 1, a uint32_t power of two |
| `sdk/rocketnet_window_ack.cpp:868` | ref_tail - ref_head kept at most uint32_t max_refs by the eviction loop at sdk/rocketnet_window_ack.cpp:503 |
| `sdk/rocketnet_window_receive.cpp:43` | uint64_t R intentionally truncated then masked to U24 |
| `sdk/rocketnet_window_receive.cpp:46` | same width sign cast of uint32_t below 2^24 |
| `sdk/rocketnet_window_receive.cpp:52` | int32_t delta checked nonnegative at sdk/rocketnet_window_receive.cpp:50 |
| `sdk/rocketnet_window_receive.cpp:58` | int32_t delta checked nonnegative at sdk/rocketnet_window_receive.cpp:50 |
| `sdk/rocketnet_window_receive.cpp:59` | uint64_t number masked by W - 1, W at most 1 << 20 (sdk/rocketnet_layers.h:1496) |
| `sdk/rocketnet_window_receive.cpp:79` | same width sign cast of int record bytes, wire range 1 to 65535 |
| `sdk/rocketnet_window_receive.cpp:80` | uint32_t channel read as u8 or u16 from the wire at sdk/rocketnet_message.cpp:350 |
| `sdk/rocketnet_window_receive.cpp:81` | source is int code, a message code 0 to 4 |
| `sdk/rocketnet_window_receive.cpp:93` | uint64_t R masked by W - 1, W at most 1 << 20 |
| `sdk/rocketnet_window_receive.cpp:110` | uint64_t number masked by window - 1, window at most 1 << 20 |
| `sdk/rocketnet_window_send.cpp:74` | uint64_t span at most round16 of payload capacity or F 1152 (sdk/rocketnet_channel.cpp:988), so under 256 units |
| `sdk/rocketnet_window_send.cpp:146` | uint32_t i below count, at most ceil of 1 MiB over 1152 via max_message_bytes checked at sdk/rocketnet_channel.cpp:931 |
| `sdk/rocketnet_xdp_frame.h:50` | widening uint8_t to uint32_t |
| `sdk/rocketnet_xdp_frame.h:55` | widening uint8_t to uint32_t |
| `sdk/rocketnet_xdp_frame.h:58` | uint32_t sum folded below 0x10000 by the loop above, intentional complement |
| `sdk/rocketnet_xdp_frame.h:72` | widening uint8_t to uint32_t |
| `sdk/rocketnet_xdp_frame.h:74` | widening uint8_t to uint32_t |
| `sdk/rocketnet_xdp_frame.h:75` | uint32_t udp_length high half, intentional split |
| `sdk/rocketnet_xdp_frame.h:76` | uint32_t udp_length masked with 0xFFFF |
| `sdk/rocketnet_xdp_frame.h:80` | widening uint8_t to uint32_t |
| `sdk/rocketnet_xdp_frame.h:83` | widening uint8_t to uint32_t |
| `sdk/rocketnet_xdp_frame.h:85` | widening uint8_t to uint32_t |
| `sdk/rocketnet_xdp_frame.h:89` | uint32_t sum folded below 0x10000 by the loop above, intentional complement |
| `sdk/rocketnet_xdp_frame.h:100` | source is uint16_t shifted right 8, fits a byte |
| `sdk/rocketnet_xdp_frame.h:101` | source is uint16_t masked with 0xFF |
| `sdk/rocketnet_xdp_frame.h:148` | int payload_bytes at most ROCKETNET_MAX_PACKET_BYTES checked at sdk/rocketnet_xdp_frame.h:129 |
| `sdk/rocketnet_xdp_frame.h:161` | int payload_bytes at most ROCKETNET_RECEIVE_PACKET_BYTES checked at sdk/rocketnet_xdp_frame.h:129 |
| `sdk/rocketnet_xdp_frame.h:185` | int payload_bytes at most the family maximum checked at sdk/rocketnet_xdp_frame.h:129 |
| `sdk/rocketnet_xdp_frame.h:193` | int payload_bytes at most ROCKETNET_MAX_PACKET_BYTES checked at sdk/rocketnet_xdp_frame.h:129 |
| `sdk/rocketnet_xdp_frame.h:233` | two bytes combined in int, at most 0xFFFF |
| `sdk/rocketnet_xdp_frame.h:246` | two bytes combined in int, at most 0xFFFF |
| `sdk/rocketnet_xdp_frame.h:255` | two bytes combined in int, at most 0xFFFF |
| `sdk/rocketnet_xdp_frame.h:269` | two bytes combined in int, at most 0xFFFF |
| `sdk/rocketnet_xdp_frame.h:275` | two bytes combined in int, at most 0xFFFF |
| `sdk/rocketnet_xdp_frame.h:276` | two bytes combined in int, at most 0xFFFF |
| `sdk/rocketnet_xdp_frame.h:289` | two bytes combined in int, at most 0xFFFF |
| `sdk/rocketnet_xdp_frame.h:290` | size_t frame_bytes is a descriptor len at most ROCKETNET_XDP_FRAME_SIZE checked at sdk/rocketnet_xdp_frame.h:443 |
| `sdk/rocketnet_xdp_frame.h:291` | source is uint16_t udp_len promoted to int, widening |
| `sdk/rocketnet_xdp_frame.h:293` | source is uint16_t udp_len promoted to int, widening |
| `sdk/rocketnet_xdp_frame.h:357` | two bytes combined in int, at most 0xFFFF |
| `sdk/rocketnet_xdp_frame.h:426` | size_t packet_bytes at most ROCKETNET_RECEIVE_CEILING checked at sdk/rocketnet_xdp_frame.h:418 |
| `sdk/rocketnet_xdp_frame.h:548` | uint16_t port masked with 0xFF |
| `sdk/rocketnet_xdp_frame.h:549` | uint16_t port shifted right 8, fits a byte |
| `sdk/rocketnet_xdp_frame.h:560` | source is int probe index below ROCKETNET_XDP_REPLY_MAC_PROBES |
| `sdk/rocketnet_xdp_frame.h:588` | source is int probe index below ROCKETNET_XDP_REPLY_MAC_PROBES |
| `sdk/rocketnet_xdp_replay_marks.h:42` | uint16_t shifted right 8, fits uint8_t |
| `sdk/rocketnet_xdp_replay_marks.h:43` | uint16_t masked with 0xff, fits uint8_t |
| `sdk/rocketnet_xdp_replay_marks.h:49` | uint16_t port shifted right 8, fits uint8_t |
| `sdk/rocketnet_xdp_replay_marks.h:50` | uint16_t port masked with 0xff, fits uint8_t |
| `sdk/rocketnet_xdp_socket_config.h:44` | compile-time XDP flag constants and a bool, fits uint16_t |
| `tests/counters_expect.h:136` | sizeof of a 16384-byte local report buffer, a compile-time constant |
| `tests/counters_expect.h:150` | sizeof of a 16384-byte local report buffer, a compile-time constant |
| `tests/counters_expect.h:196` | sizeof of a 16384-byte local report buffer, a compile-time constant |
| `tests/dst_checks.h:237` | constant 1u shifted below 8, fits uint8_t |
| `tests/dst_checks.h:248` | constant 1u shifted below 8, fits uint8_t |
| `tests/dst_checks.h:250` | complement of a bit below 8, intentional low byte mask |
| `tests/dst_checks.h:251` | complement of a bit below 8, intentional low byte mask |
| `tests/dst_checks.h:296` | int bytes clamped to 65535 on the same line |
| `tests/dst_checks.h:304` | source is int bytes, positive message size at most 16384, same width |
| `tests/dst_checks.h:529` | widening uint16_t to unsigned for printf |
| `tests/dst_checks.h:640` | int bytes checked at most outstanding_bytes on the line before, same width |
| `tests/dst_checks.h:1027` | uint64_t packet_id mod ROCKETNET_DST_CHECK_MAX_PACKETS fits uint32_t |
| `tests/dst_checks.h:1036` | uint64_t packet_id mod ROCKETNET_DST_CHECK_MAX_PACKETS fits uint32_t |
| `tests/dst_harness.h:131` | intended byte fold of a uint64 into the trace hash |
| `tests/dst_harness.h:179` | declaration or pointer cast to a payload row type, no integer narrowing |
| `tests/dst_harness.h:288` | uint64 client id masked with 0xFF before the cast |
| `tests/dst_harness.h:590` | intended byte serialization of a uint32 payload field |
| `tests/dst_harness.h:591` | intended byte serialization of a uint32 payload field |
| `tests/dst_harness.h:592` | intended byte serialization of a uint32 payload field |
| `tests/dst_harness.h:593` | intended byte serialization of a uint32 payload field |
| `tests/dst_harness.h:594` | source is an int bit expression of direction and type, reduced to a byte by design |
| `tests/dst_harness.h:595` | source is int channel, checked < ROCKETNET_DST_NUM_CHANNELS on verify |
| `tests/dst_harness.h:596` | intended byte serialization of a uint32 payload field |
| `tests/dst_harness.h:597` | intended byte serialization of a uint32 payload field |
| `tests/dst_harness.h:598` | intended byte serialization of a uint32 payload field |
| `tests/dst_harness.h:599` | source is int incarnation, stored as one wire byte by design |
| `tests/dst_harness.h:600` | intended byte serialization of a uint32 payload field |
| `tests/dst_harness.h:601` | intended byte serialization of a uint32 payload field |
| `tests/dst_harness.h:602` | intended byte serialization of a uint32 payload field |
| `tests/dst_harness.h:603` | intended byte serialization of a uint32 payload field |
| `tests/dst_harness.h:605` | uint32 value reduced modulo 256 before the cast |
| `tests/dst_harness.h:616` | widening bytes to uint32, not narrowing |
| `tests/dst_harness.h:617` | widening bytes to uint32, not narrowing |
| `tests/dst_harness.h:620` | widening bytes to uint32, not narrowing |
| `tests/dst_harness.h:625` | uint32 value reduced modulo 256 before the cast |
| `tests/dst_harness.h:657` | uniform draw in [lo, hi] with hi at most 16384 (tests/dst_harness.h:648) |
| `tests/dst_harness.h:744` | uniform draw in [0, ROCKETNET_DST_NUM_CHANNELS - 1], fits int |
| `tests/dst_harness.h:754` | uint64 client id masked with 0xFF before the cast |
| `tests/dst_harness.h:770` | uint64 generation increments once per connect, bounded by steps far below 2^32 in any run that finishes |
| `tests/dst_harness.h:772` | sizeof of a 16384-byte local payload buffer, a compile-time constant |
| `tests/dst_harness.h:815` | widening bytes to uint32, not narrowing |
| `tests/dst_harness.h:816` | widening bytes to uint32, not narrowing |
| `tests/dst_harness.h:819` | uint32 value reduced modulo 256 before the cast |
| `tests/dst_harness.h:832` | widening bytes to uint32, not narrowing |
| `tests/dst_harness.h:833` | widening bytes to uint32, not narrowing |
| `tests/dst_harness.h:867` | uniform draw in [0, 99], fits int |
| `tests/dst_harness.h:888` | uniform draw in [0, 99], fits int |
| `tests/dst_harness.h:911` | uniform draw in [2, 8], fits int |
| `tests/dst_harness.h:923` | uniform draw in [0, 99], fits int |
| `tests/dst_harness.h:989` | declaration or pointer cast to a payload row type, no integer narrowing |
| `tests/dst_harness.h:1017` | declaration or pointer cast to a payload row type, no integer narrowing |
| `tests/dst_harness.h:1107` | first match of the truncated step is the true force_fail_step; any later wrap match is moot after the run has failed |
| `tests/dst_harness.h:1171` | client_id is generation*256 + index, so client_id >> 8 is the small connect count, far below 2^32 |
| `tests/dst_harness.h:1184` | uint64 generation increments once per connect, bounded by steps far below 2^32 in any run that finishes |
| `tests/live_verify.h:106` | int operands masked to 1 and 7 bits, fits a byte |
| `tests/live_verify.h:157` | int64_t value clamped to max_m on the same line |
| `tests/live_verify.h:222` | result at most hi, lo plus a remainder below hi - lo + 1, both uint32_t |
| `tests/live_verify.h:236` | modulo ROCKETNET_LIVE_SLOTS, a small constant |
| `tests/live_verify.h:269` | int boundary count, at most ROCKETNET_LIVE_MAX_BOUNDARIES |
| `tests/live_verify.h:314` | int capacity checked nonnegative on the same line |
| `tests/live_verify.h:318` | source is int channel, intentional low byte |
| `tests/live_verify.h:319` | uint32_t cycle, intentional low byte mod 256 |
| `tests/live_verify.h:320` | uint32_t sequence, intentional byte split |
| `tests/live_verify.h:321` | uint32_t sequence, intentional byte split |
| `tests/live_verify.h:322` | uint32_t sequence, intentional byte split |
| `tests/live_verify.h:323` | uint32_t sequence, intentional byte split |
| `tests/live_verify.h:327` | uint32_t modulo 256u, fits a byte |
| `tests/live_verify.h:328` | uint32_t length at most capacity, an int, checked at tests/live_verify.h:314 |
| `tests/live_verify.h:337` | widening uint8_t to uint32_t |
| `tests/live_verify.h:361` | source is int channel, intentional low byte compare |
| `tests/live_verify.h:366` | uint32_t cycle, intentional low byte compare |
| `tests/live_verify.h:381` | int64_t expected_sequence checked nonnegative above, callers pass a uint32_t expected or -1 |
| `tests/live_verify.h:384` | int bytes, at least 1 checked at tests/live_verify.h:354 |
| `tests/live_verify.h:389` | uint32_t sequence shifted, intentional byte compare |
| `tests/live_verify.h:397` | uint32_t modulo 256u, fits a byte |
| `tests/pcapng.h:120` | uint32_t masked with 0xFF, deliberate byte split |
| `tests/pcapng.h:121` | uint32_t shifted and masked with 0xFF, deliberate byte split |
| `tests/pcapng.h:127` | uint32_t shifted and masked with 0xFF, deliberate byte split |
| `tests/pcapng.h:139` | block size from an int datagram length plus short harness comments, capture capped at tests/functional/channel_harness.h:182 |
| `tests/pcapng.h:141` | uint32_t masked with 0xFF, deliberate byte split |
| `tests/pcapng.h:142` | uint32_t shifted and masked with 0xFF, deliberate byte split |
| `tests/pcapng.h:143` | uint32_t shifted and masked with 0xFF, deliberate byte split |
| `tests/pcapng.h:144` | uint32_t shifted and masked with 0xFF, deliberate byte split |
| `tests/pcapng.h:167` | same width sign cast of a constant link type |
| `tests/pcapng.h:182` | same width sign cast of int interface id 0 or 1 |
| `tests/pcapng.h:183` | uint64_t shifted right 32, deliberate high word |
| `tests/pcapng.h:184` | uint64_t masked with 0xFFFFFFFF, deliberate low word |
| `tests/pcapng.h:185` | same width sign cast of non-negative int bytes |
| `tests/pcapng.h:186` | same width sign cast of non-negative int bytes |
| `tests/pcapng.h:194` | strlen of short harness comments such as end and type names, far below 65535 |
| `tests/pcapng.h:224` | same width sign cast of int datagram bytes, wire datagrams bounded by ROCKETNET_RECEIVE_CEILING |
| `tests/pcapng.h:227` | deliberate byte split of a total bounded by the datagram ceiling |
| `tests/pcapng.h:230` | deliberate byte split of a uint32_t address |
| `tests/pcapng.h:231` | deliberate byte split of a uint32_t address |
| `tests/pcapng.h:236` | widening of uint8_t to uint32_t |
| `tests/pcapng.h:239` | ones complement checksum folded below 0x10000 by the loop above |
| `tests/pcapng.h:240` | uint16_t checksum high byte, deliberate byte split |
| `tests/pcapng.h:241` | uint16_t checksum low byte, deliberate byte split |
| `tests/pcapng.h:242` | same width sign cast of int datagram bytes, wire datagrams bounded by ROCKETNET_RECEIVE_CEILING |
| `tests/pcapng.h:245` | deliberate byte split of a uint16_t port |
| `tests/pcapng.h:246` | deliberate byte split of a uint16_t port |
| `tests/pcapng.h:247` | deliberate byte split of a UDP length bounded by the datagram ceiling |
| `tests/pcapng.h:261` | widening of uint8_t to uint32_t |
| `tests/pcapng.h:266` | widening of uint8_t to uint32_t |
| `tests/pcapng.h:324` | value from a 16-bit field, at most 0xFFFF |
| `tests/pcapng.h:348` | interface_id below interfaces.size() checked at tests/pcapng.h:334, at most two interfaces |
| `tests/pcapng.h:416` | widening of uint8_t to uint32_t |
| `tests/pcapng.h:421` | widening of uint8_t to uint32_t |
| `tests/pcapng.h:426` | widening of uint8_t to uint32_t |
| `tests/pcapng.h:429` | widening of uint8_t to uint32_t |
| `tests/pcapng.h:430` | widening of uint8_t to uint32_t |
| `tests/pcapng.h:431` | two bytes combined, at most 0xFFFF |
| `tests/pcapng.h:432` | two bytes combined, at most 0xFFFF |
| `tests/pcapng.h:434` | total is a 16-bit field equal to frame size, at least 28 checked at tests/pcapng.h:411 and :417 |
| `tests/pcapng.h:452` | wcslen of WIN32_FIND_DATAW cFileName, at most MAX_PATH 260 |
| `tests/resource_file.h:22` | size_t length of a test file path, far below INT_MAX |
| `tests/resource_file.h:26` | size_t length of a test file path, far below INT_MAX |
| `tests/suite_runner.h:263` | constant size_t array count of 5 |
| `tests/suite_runner.h:282` | DWORD n checked below buf.size at most 32768 on line 281 |
| `tests/suite_runner.h:289` | constant sizeof 4096 |
| `tests/suite_runner.h:338` | source is int requested checked positive on line 337 |
| `tests/suite_runner.h:342` | unsigned jobs clamped to at most 8 on line 340 |
| `tests/suite_runner.h:421` | long slots checked below jobs, an int, on the same line |
| `tests/suite_runner.h:515` | declaration of a function pointer parameter, not a cast |
| `tests/suite_runner.h:516` | declaration of a function pointer parameter, not a cast |
| `tests/suite_runner.h:623` | declaration of a function pointer parameter, not a cast |
| `tests/suite_runner.h:669` | declaration of a function pointer parameter, not a cast |
| `tests/suite_runner.h:670` | declaration of a function pointer parameter, not a cast |
| `tests/suite_runner.h:671` | declaration of a function pointer parameter, not a cast |
| `tests/suite_runner.h:673` | declaration of a function pointer parameter, not a cast |
| `tests/suite_runner.h:752` | running.size held at most jobs, at most 8, by this loop condition |
| `tests/suite_runner.h:937` | declaration of a function pointer parameter, not a cast |
| `tests/suite_runner.h:938` | declaration of a function pointer parameter, not a cast |
| `tests/suite_runner.h:939` | declaration of a function pointer parameter, not a cast |
| `tests/suite_runner.h:941` | declaration of a function pointer parameter, not a cast |
| `tests/suite_runner.h:991` | running.size held at most jobs, at most 8, by this loop condition |

## Already filed

- `sdk/rocketnet_base64.cpp:63` — the encoded length returned as `int`: `docs/audits/2026-10-24-code-quality-a.md` finding 5, and `docs/audits/2026-10-24-code-quality-b.md` finding 4.
- `sdk/rocketnet_base64.cpp:153` — the decoded length returned as `int`: `docs/audits/2026-10-24-code-quality-b.md` finding 4.
