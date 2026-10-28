# Static analysis triage, 2026-10-24

Triage of every finding recorded in `tools/tidy_baseline.txt` and `tools/cppcheck_baseline.txt`
for `mas-bandwidth/rocketnet` at `53cbafcf843fc9f08b7a5de5325c254cd47f9a5d` (`main`). Scope: the
51 `sdk/*.cpp` translation units that `tools/run_tidy.py` analyzes on Linux (Release,
`CMAKE_EXPORT_COMPILE_COMMANDS=ON`), plus the `sdk/*.h` headers they include. Method: the
findings were reproduced with `tools/run_tidy.py` (gate result: both baselines match exactly),
the same analyzer invocations were re-run with line and column kept, and each finding was judged
by reading the code it names and the callers that reach it. Tools and baseline counts per check:
clang-tidy 18.1.8 (Ubuntu LLVM), 390 findings, with the checks of `.clang-tidy`:
`cert-err33-c` 116, `concurrency-mt-unsafe` 101,
`bugprone-implicit-widening-of-multiplication-result` 59, `bugprone-misplaced-widening-cast` 25,
`bugprone-macro-parentheses` 19, `cert-flp30-c` 19,
`bugprone-multi-level-implicit-pointer-conversion` 15, `bugprone-branch-clone` 9,
`bugprone-suspicious-memory-comparison,cert-exp42-c,cert-flp37-c` 5,
`bugprone-switch-missing-default-case` 5, `bugprone-sizeof-expression` 3, `cert-err34-c` 3,
`cert-err58-cpp` 3, `bugprone-assignment-in-if-condition` 2, `bugprone-narrowing-conversions` 2,
`performance-no-int-to-ptr` 2, `bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp` 1,
`cert-dcl50-cpp` 1; cppcheck 2.13.0
(`--enable=warning,performance,portability --std=c++11`), 43 findings: `memsetClassFloat` 32,
`invalidPointerCast` 2, `nullPointerRedundantCheck` 2, `uninitMemberVar` 2, `ctunullpointer` 1,
`incorrectStringBooleanError` 1, `invalidLifetime` 1, `passedByValue` 1,
`returnDanglingLifetime` 1. Of the 433 baselined findings, 4 lead to the 3 findings below, 5 are
already filed, and 424 are judged not a defect. No code was changed. Findings are ordered most
severe first; each gives severity (`bug` / `risk` / `quality`), `file:line` at 53cbafcf, a
one-sentence defect naming the tool and check, a one-sentence concrete failure or cost, and the
smallest fix.

## Findings

### 1. risk — `sdk/rocketnet_xdp_frame.h:625`

- Defect: cppcheck `ctunullpointer` (reported at line 545): `rocketnet_xdp_reply_mac` tests `destination &&` at lines 625 and 627, but has already passed `destination` to `rocketnet_xdp_reply_macs_find` at line 622, which dereferences it unconditionally in `rocketnet_xdp_reply_macs_slot` (line 545); `rocketnet_xdp_reply_macs_forget` (line 607) passes it on unchecked too, while `rocketnet_xdp_reply_macs_learn` (line 574) refuses NULL.
- Failure or cost: The guards document a NULL destination as accepted, so a future caller that relies on them (a reply with no known destination) gets a NULL dereference at line 545 on the XDP send path instead of a counted refusal.
- Smallest fix: Decide the contract once: either return NULL (counted in `*refused`) at the top of `rocketnet_xdp_reply_mac` and `rocketnet_xdp_reply_macs_forget` when `destination` is NULL, or drop the two dead `destination &&` tests and assert it non-NULL.

### 2. quality — `sdk/rocketnet_driver_builtin.cpp:1054`

- Defect: cppcheck `nullPointerRedundantCheck` (lines 1049 and 1050): `xdp_create` reads `config->transport->num_xdp_queues` and `config->transport->num_os_cpus` unguarded, then tests `config->transport &&` at line 1054 before reading `second_public_address`, and reads `config->transport->packet_filter` unguarded again at line 1056.
- Failure or cost: The one guarded read implies `transport` may be NULL, which the only producer of this config (`sdk/rocketnet_server_transport.cpp:1532`, always `config`) never passes, so a reader cannot tell which of the four reads states the real contract.
- Smallest fix: Drop the `config->transport &&` test at line 1054 (the reads at 1049, 1050 and 1056 already require it non-NULL).

### 3. quality — `sdk/rocketnet_server_socket_linux.cpp:1010`

- Defect: clang-tidy `cert-err34-c`: the XDP pre-flight converts each IRQ's `smp_affinity_list` with `atoi( affinity )`, and `rocketnet_server_socket_read_sysfs` returns true with an empty string when the file reads empty (lines 605–613), so an empty or non-numeric affinity is read as cpu 0.
- Failure or cost: An unreadable affinity is reported as "irq N is pinned to cpu 0, which is not in the isolated set" (line 1014) rather than as unreadable (the wording line 998 uses), sending the operator to fix a pin that does not exist.
- Smallest fix: Parse with `strtol` and check that at least one digit was consumed and nothing but whitespace follows; on failure take the line-998 branch (warn "unreadable", `provisioned = false`, `continue`).

## Counts

- bug: 0
- risk: 1 (finding 1)
- quality: 2 (findings 2, 3)
- total: 3

## Judged not a defect

Each row names the check, every location it covers (`file` and lines at 53cbafcf), and why the
finding is a false positive or noise for this code base. Where a whole check is noise, the row
says whether it should be turned off in `.clang-tidy` (or suppressed in `tools/run_tidy.py` for
cppcheck).

### clang-tidy

| Check | Locations | Reason |
|---|---|---|
| `cert-err33-c` (44) | `sdk/rocketnet.cpp` 561, 569; `sdk/rocketnet_client_transport.cpp` 374, 1427, 1528, 1540; `sdk/rocketnet_config.cpp` 38, 54, 457, 465, 472, 478, 484, 543, 612, 999; `sdk/rocketnet_connection_reset.cpp` 171, 177, 190, 202, 209; `sdk/rocketnet_counters.cpp` 120, 140; `sdk/rocketnet_network_simulator.cpp` 788, 793, 805, 813, 821, 829, 835, 1020; `sdk/rocketnet_platform.cpp` 328; `sdk/rocketnet_server_socket_linux.cpp` 993, 1074, 1081, 1486, 1514, 2702, 2870; `sdk/rocketnet_server_transport.cpp` 422, 1478, 1522, 1577, 1589 | Ignored `snprintf` results into fixed buffers: diagnostic and log text, failure reports, or name lists whose appends recompute `used` with `strlen` (so a truncation stays terminated and in bounds); the config string write at `sdk/rocketnet_config.cpp:612` is preceded by a length refusal (line 607), lines 38 and 54 are the config-table default macro writing compile-time defaults that fit, and the sysfs and bpffs paths are far below their buffers. A truncated diagnostic is the worst outcome. |
| `cert-err33-c` (9) | `sdk/rocketnet.cpp` 42, 263; `sdk/rocketnet_network_simulator.cpp` 250, 251, 252; `sdk/rocketnet_server_socket_linux.cpp` 330, 348, 612, 1036 | `fflush`/`fprintf` on `stdout`/`stderr` in logging and audit-failure output, and `fclose` of files opened read-only; there is nothing to recover from either. |
| `cert-err33-c` (58) | `sdk/rocketnet_tests.cpp` 169, 212, 2361, 2423, 2837, 2888, 3246, 3316, 3341, 3708, 4826, 4861, 4951, 5092, 5221, 5260, 5335, 5380, 5415, 5442, 5541, 5643, 5655, 5711, 5964, 6019, 6182, 6276, 6386, 6398, 6564, 6605, 6655, 6816, 6879, 9366, 9456, 9523, 9769, 9921, 10315, 10410, 10894, 11049, 11096, 12026, 12331, 12367, 12369, 12410, 12420, 12443, 12637, 14284, 14438, 14499, 15023, 15027 | Test output: `fflush( stdout )` inside the `rocketnet_counters_assert_conserved` and `rocketnet_counters_expect` macros (`tests/counters_expect.h:130`, `:145`) and test `printf`/`snprintf` text. Keep the check on: it would catch an ignored `fread` or allocation result, and none is ignored today. |
| `concurrency-mt-unsafe` (54) | `sdk/rocketnet_client_gso_linux.cpp` 203, 218, 227; `sdk/rocketnet_frame_sync.cpp` 190; `sdk/rocketnet_platform.cpp` 329, 1035, 1088, 1103, 1553, 1567; `sdk/rocketnet_server_gso_linux.cpp` 465, 483, 493, 510, 517, 556, 563, 574, 590, 1422, 1434; `sdk/rocketnet_server_socket_linux.cpp` 377, 397, 881, 1097, 1123, 1154, 1175, 1499, 1600, 1608, 1616, 1624, 1667, 1739, 1755, 1760, 1966, 2307, 2703, 2871; `sdk/rocketnet_server_uring_linux.cpp` 389, 407, 417, 428, 435, 470, 477, 489, 505, 847, 931, 1299, 1323 | `strerror` on error and log paths; each call is formatted straight into one log line. glibc returns a constant string for a known error code and, since 2.32, a per-thread buffer for an unknown one, so concurrent calls do not share writable state. |
| `concurrency-mt-unsafe` (2) | `sdk/rocketnet_platform.cpp` 594, 732 | `getenv` in `rocketnet_platform_getenv` (the library only reads the environment and copies the value out at once) and `sleep` in `rocketnet_platform_sleep`, which blocks by contract. |
| `concurrency-mt-unsafe` (45) | `sdk/rocketnet_tests.cpp` 220, 12338, 12350, 12352, 12454, 12691, 12692, 12693, 12694, 12695, 12736, 12737, 12742, 12744, 12745, 12795, 12796, 12797, 13236, 13237, 13251, 13256, 13262, 13267, 13354, 13356, 13362, 13364, 13366, 13383, 13405, 13413, 13415, 13678, 13721, 13722, 13857, 13882, 13883, 13893, 13895, 13899, 13911, 14999, 15000 | `setenv`/`unsetenv` and `exit` in the single-threaded test runner, setting and restoring config overrides around one test. Keep the check on: it guards the transports' worker threads against new non-reentrant calls. |
| `bugprone-implicit-widening-of-multiplication-result` (59) | `sdk/rocketnet_client_protocol.cpp` 1653, 1655, 1697; `sdk/rocketnet_client_transport.cpp` 648, 796, 1763, 1903, 2027, 2195, 2592, 2642, 2712, 2774, 2776, 2822, 2892, 3077, 3093, 3101, 3102, 3291, 3294; `sdk/rocketnet_fragment.cpp` 144, 266; `sdk/rocketnet_server_gso_linux.cpp` 1209, 1231; `sdk/rocketnet_server_protocol.cpp` 2187, 2189, 2238; `sdk/rocketnet_server_socket_linux.cpp` 524, 2059, 2886; `sdk/rocketnet_server_transport.cpp` 417, 781, 834, 2116, 2241, 2273, 2628, 2675, 2764, 2911, 2990, 3092, 3294; `sdk/rocketnet_server_uring_linux.cpp` 97, 1512; `sdk/rocketnet_tests.cpp` 5323, 6519, 11008, 11025, 11035, 11081, 11520, 11555, 11589; `sdk/rocketnet_xdp_frame.h` 152, 154; `sdk/rocketnet_xdp_replay_marks.h` 42 | An `int` index or count times a fixed stride (`ROCKETNET_FRAME_STRIDE`, `ROCKETNET_FRAME_SIZE`, `ROCKETNET_SEND_BUDGET`, a fragment stride, 2 for an IPv6 word), or a product of compile-time constants. Each index is bounded by a ring, buffer or table already allocated at that size, so every product is far below 2^31. Keep the check on: it is the one that would catch a future byte offset outgrowing `int`. |
| `bugprone-misplaced-widening-cast` (25) | `sdk/rocketnet_keys.cpp` 35; `sdk/rocketnet_protocol.cpp` 218; `sdk/rocketnet_server_socket_linux.cpp` 1811, 1824; `sdk/rocketnet_server_transport.cpp` 431, 452, 467, 2782, 2984; `sdk/rocketnet_tests.cpp` 9875, 9885, 14131, 14132, 14133, 14134, 14136, 14137, 14138, 14139, 14164, 14179, 14218, 14237, 14373; `sdk/rocketnet_xdp_frame.h` 131 | A sum widened after it is formed: packet byte counts plus a header size (at most a few thousand), UMEM frame offsets (frame index times `ROCKETNET_XDP_FRAME_SIZE` within the UMEM), a key input length plus 8, and small test totals and ids. None can overflow `int`. |
| `bugprone-macro-parentheses` (15) | `sdk/rocketnet_channel.h` 66; `sdk/rocketnet_endpoint.h` 175, 176; `sdk/rocketnet_layers.h` 101, 103, 105, 107, 109, 111, 113; `sdk/rocketnet_protocol.h` 47, 48, 49, 50; `sdk/rocketnet_server_protocol.h` 153 | Negative integer constants (`-1` … `-7`) written in the netcode style; unary minus binds tighter than every binary operator, so no expansion changes meaning. |
| `bugprone-macro-parentheses` (4) | `sdk/rocketnet_abi.cpp` 26; `sdk/rocketnet_driver.cpp` 28, 28, 72 | X-macro and argument-list macros: `ROCKETNET_KNOWN_BYTES` expands to a comma-separated argument pair and cannot be parenthesized, and the `ROCKETNET_TRANSPORT` rows take fixed tokens from `sdk/rocketnet_drivers.inc`. Keep the check on. |
| `cert-flp30-c` (19) | `sdk/rocketnet_tests.cpp` 4779, 4903, 5007, 5142, 5489, 5498, 5559, 5577, 5610, 5639, 5652, 5897, 6150, 6223, 6417, 6547, 11379, 11440, 11908 | Test loops that advance a simulated clock by a floating-point step until a time limit; the bound is a deadline, not an iteration count, and the sdk proper has no such loop. Whole check is noise for this code base; turning it off in `.clang-tidy` is proposed. |
| `bugprone-multi-level-implicit-pointer-conversion` (15) | `sdk/rocketnet_client_transport.cpp` 2993, 2995, 3153; `sdk/rocketnet_connect_token.cpp` 230; `sdk/rocketnet_message_client.cpp` 342, 353; `sdk/rocketnet_message_server.cpp` 1704, 1715; `sdk/rocketnet_server_transport.cpp` 2463, 2465, 2467, 2488, 3419, 3421, 3442 | Every hit is a pointer-to-pointer argument passed to `rocketnet_api_null_guard( const void * )` (`sdk/rocketnet_internal.h:333`), which only tests it for NULL. Whole check is noise for this code base; turning it off in `.clang-tidy` is proposed. |
| `bugprone-branch-clone` (9) | `sdk/rocketnet_api.cpp` 1475; `sdk/rocketnet_client_io.h` 269; `sdk/rocketnet_config.cpp` 54; `sdk/rocketnet_driver_builtin.cpp` 31; `sdk/rocketnet_server_protocol.cpp` 463, 1072; `sdk/rocketnet_server_transport.cpp` 1207; `sdk/rocketnet_server_uring_linux.cpp` 728; `sdk/rocketnet_xdp_frame.h` 127 | Identical arms by configuration, not by mistake: `ROCKETNET_DEFAULT_MAX_CLIENTS` is `( ROCKETNET_MAX_CLIENTS < 1000 ? ROCKETNET_MAX_CLIENTS : 1000 )`, equal at the default `ROCKETNET_MAX_CLIENTS` of 1000 (api 1475, config 54, driver_builtin 31, server_transport 1207); `ROCKETNET_RECEIVE_PACKET_BYTES` is defined as `ROCKETNET_MAX_PACKET_BYTES` (`sdk/rocketnet_layers.h:31`); the disconnect and ack minimum wire sizes are both 33 bytes (client_io 269, server_protocol 1072); the current and previous cookie keys both set `mac_matches` (server_protocol 463); two receive drop reasons share one counter (uring 728). Keep the check on. |
| `bugprone-suspicious-memory-comparison` (5) | `sdk/rocketnet_ack.cpp` 282; `sdk/rocketnet_client_transport.cpp` 330, 371; `sdk/rocketnet_server_replay_marks.cpp` 206; `sdk/rocketnet_tests.cpp` 12873 | Test-seam and test checks that a reset left an object byte-identical to a fresh one; the compared structs are reset field by field to `+0.0` and zero, and byte identity is the property being tested. |
| `bugprone-switch-missing-default-case` (5) | `sdk/rocketnet_client_protocol.cpp` 367, 1357; `sdk/rocketnet_route.h` 122; `sdk/rocketnet_tests.cpp` 5482, 12461 | Switches on an `int` state, packet type, netlink attribute type or test scenario where every unlisted value deliberately does nothing (states with no send, packet types refused earlier, attributes the route walk does not read). |
| `bugprone-sizeof-expression` (3) | `sdk/rocketnet_client_transport.cpp` 261, 276, 279 | The zero-state and reset-inventory macros take `sizeof` of each member; for a member that is a pointer, the pointer's own size is the size meant. |
| `cert-err34-c` (2) | `sdk/rocketnet_server_socket_linux.cpp` 987, 1703 | Line 987's `sscanf` result is checked (`!= 1`) on kernel-written `/proc/interrupts` text; line 1703 is the `ROCKETNET_TEST_XDP_FAIL_AT_QUEUE` test-injection variable. |
| `cert-err58-cpp` (3) | `sdk/rocketnet_api.cpp` 470, 1282; `sdk/rocketnet_tests.cpp` 6327 | Static initializers that are integer arithmetic (`rn_round_up_alignment`) or a test-case registration; nothing in them can throw. |
| `bugprone-assignment-in-if-condition` (2) | `sdk/rocketnet_platform.cpp` 2152; `sdk/rocketnet_tests.cpp` 6438 | `( sock = socket( ... ) ) == -1`, the POSIX idiom, and an assignment inside the test's random-number macro. |
| `bugprone-narrowing-conversions` (2) | `sdk/rocketnet_ack.cpp` 55; `sdk/rocketnet_replay.cpp` 14 | `(int) ( sequence % ROCKETNET_REPLAY_WINDOW_BITS )`: the value is below the window size and fits `int`. |
| `performance-no-int-to-ptr` (2) | `sdk/rocketnet.cpp` 512, 515 | The aligned-allocation arithmetic of `rocketnet_malloc`, which must round an address; this is not a hot path. |
| `bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp` (1) | `sdk/rocketnet_internal.h` 15 | `__STDC_FORMAT_MACROS` is the name C99 defines for a C++ program to request the `PRIu64` family from `<inttypes.h>`. |
| `cert-dcl50-cpp` (1) | `sdk/rocketnet_allocator.cpp` 1381 | `rn_text_append` is a printf-style variadic in the C API style and carries `format( printf, 2, 3 )`, so the compiler checks its arguments. |

### cppcheck

| Check | Locations | Reason |
|---|---|---|
| `memsetClassFloat` (32) | `sdk/rocketnet_allocator.cpp` 488, 521; `sdk/rocketnet_api.cpp` 1186, 1209, 2162, 2170, 2196, 2238, 2253, 2292, 2302; `sdk/rocketnet_channel.cpp` 1145, 1153, 1163; `sdk/rocketnet_client_transport.cpp` 313, 353, 3172; `sdk/rocketnet_encryption_map.cpp` 218; `sdk/rocketnet_frame_sync.cpp` 293, 393; `sdk/rocketnet_network_simulator.cpp` 286; `sdk/rocketnet_send_budget.h` 200; `sdk/rocketnet_server_transport.cpp` 3396, 3461; `sdk/rocketnet_tests.cpp` 2327, 8042, 8088, 8102, 10003, 10009, 10010; `sdk/rocketnet_window_ack.cpp` 331 | Zeroing a C struct with `memset` before filling it; on every supported platform floating point is IEEE 754, where all-zero bytes are `+0.0`. Whole check is noise for this code base; suppressing it in `tools/run_tidy.py` is proposed. |
| `invalidPointerCast` (2) | `sdk/rocketnet_allocator.cpp` 442; `sdk/rocketnet_platform.h` 1279 | Line 442 carves a `double` array out of an aligned arena at its layout offset, as every other part on lines 439–446 is carved; line 1279 publishes a `double`'s bit pattern (moved with `memcpy`) through a same-size, same-alignment 64-bit atomic builtin, the GCC and Clang way to store a lock-free double. Neither reinterprets a value's representation. |
| `uninitMemberVar` (2) | `sdk/rocketnet_driver.cpp` 78, 78 | `servers` and `clients` belong to a function-local static (`rocketnet_transport_lists`, line 123), which is zero-initialized before the constructor runs, and only the first `num_servers`/`num_clients` entries are ever read. |
| `invalidLifetime` (1) | `sdk/rocketnet_server_transport.cpp` 2639 | `frame` is the transport frame returned by `rocketnet_server_transport_io_start_packet`, which is also passed `&packet_id`; cppcheck assumes the returned pointer may point at that local, but it points into the transport's send memory. |
| `returnDanglingLifetime` (1) | `sdk/rocketnet_message_server.cpp` 896 | Same pattern: `start_packet` is passed `&context` and returns a transport frame; the returned pointer is into that frame, and `context` is copied into `object->in_place_context` before the return. |
| `incorrectStringBooleanError` (1) | `sdk/rocketnet_tests.cpp` 12449 | `rocketnet_check( false && "unknown config field kind" )`: the `false &&` idiom that puts a message in an always-failing check. |
| `passedByValue` (1) | `sdk/rocketnet_crypto_tally.h` 31 | `rocketnet_crypto_tally_set` takes a small counter struct by value, mirroring `rocketnet_crypto_tally_get`; the struct is a handful of counters and the call is on no hot path. |

## Already filed

- `sdk/rocketnet_address.cpp` 131, 142, 154, 175, 186 — clang-tidy `cert-err33-c` (five ignored `snprintf` results in `rocketnet_address_to_string` and `rocketnet_address_to_string_without_port`): `docs/audits/2026-10-24-code-quality-a.md`, finding 10.
