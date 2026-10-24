# Coverage of production paths, 2026-10-24

Line coverage of `sdk/` at `aa193eb3bab05b0f384ecdd1f0229c1ccbe41774`, from
`python3 tools/coverage_split.py --out <dir> --jobs 4`. Production is a line the
functional suite or an example executed. Unit-only is a line `./bin/test` executed
and nothing else did. Never is a line neither executed. Lines compiled only under
`ROCKETNET_TEST_SEAMS` or a `ROCKETNET_*_TEST_HOOKS` macro are counted apart, as
are lines with no mapping in the production binaries. The unit suite exited 0
(16.5s). The functional suite exited 0 (246.2s). The examples exited 1 on
`example_game_loop` (22.8s); the splitter recorded the example name and not the
assertion text. Build 84.7s, report 5.7s, total 375.9s. No source, test, tool, or
workflow file was changed. Findings are ordered most severe first. Severity is
bug, risk, or quality. A unit-only function with no production caller is quality
unless a published counter depends on it. A never-reached guard on a wire, config,
or failure input is risk.

## Per-file counts

```
file                                                 production unit_only   never     seam p/u/n not_in_prod
------------------------------------------------------------------------------------------------------------
sdk/rocketnet.cpp                                           312       154      58          0/0/0           0
sdk/rocketnet.h                                              24         7       0          0/0/0           3
sdk/rocketnet_abi.cpp                                        29        78       0          0/0/0           0
sdk/rocketnet_ack.cpp                                       923        63      42          0/1/0           3
sdk/rocketnet_ack.h                                           1        11       0          0/0/0           5
sdk/rocketnet_address.cpp                                   170        82      12          0/0/0           0
sdk/rocketnet_address.h                                       7         1       0          0/0/0           0
sdk/rocketnet_allocator.cpp                                 635       286      36          0/0/0           0
sdk/rocketnet_allocator.h                                    24         7       0          0/0/0           0
sdk/rocketnet_api.cpp                                       946       530     147          0/9/1           0
sdk/rocketnet_base64.cpp                                     67        47       5          0/0/0           0
sdk/rocketnet_buffer_handoff.h                               50         0       0          0/0/0           0
sdk/rocketnet_channel.cpp                                   781       139      76          0/0/0           0
sdk/rocketnet_channel.h                                      23         0       1          0/0/0           0
sdk/rocketnet_channel_receipts.cpp                            6         0       0          0/0/0           0
sdk/rocketnet_channel_reliable.cpp                           82        17      13          0/3/0           3
sdk/rocketnet_channel_sequenced.cpp                         238        71      51          0/0/0           0
sdk/rocketnet_client_gso.h                                    4         0       0          0/0/0           0
sdk/rocketnet_client_gso_linux.cpp                          264        29      43          6/0/0           0
sdk/rocketnet_client_io.h                                   336        30      15          0/0/0           0
sdk/rocketnet_client_protocol.cpp                           762       168      65          1/0/0           0
sdk/rocketnet_client_protocol.h                               1         0       0          0/0/0           0
sdk/rocketnet_client_slots.cpp                              172        32       0         0/32/5           0
sdk/rocketnet_client_transport.cpp                         1243       350     279       1/149/16          12
sdk/rocketnet_coalesce.h                                     39         3       0          0/0/0           0
sdk/rocketnet_config.cpp                                    363       220      49          0/5/0           6
sdk/rocketnet_config.h                                        5         0       0          0/0/0           0
sdk/rocketnet_connect_token.cpp                             258        60      32          0/1/0           1
sdk/rocketnet_connect_token.h                                24         0       0          0/0/0           0
sdk/rocketnet_connection_reset.cpp                           83        36      23          0/0/0           0
sdk/rocketnet_connection_reset.h                              0        57       0          0/0/0          39
sdk/rocketnet_constants.h                                    27         5       1          0/0/0           1
sdk/rocketnet_counters.cpp                                   29        53       6          0/0/0           0
sdk/rocketnet_counters.h                                      1         2       0          0/0/0           1
sdk/rocketnet_crypto_tally.h                                  0         3       3          0/0/0           0
sdk/rocketnet_driver.cpp                                    129         0       2          0/0/0           0
sdk/rocketnet_driver.h                                        6         0       0          0/0/0           0
sdk/rocketnet_driver_builtin.cpp                            441        21      46       8/181/39         232
sdk/rocketnet_driver_loopback.cpp                           241        54      19          0/2/0           4
sdk/rocketnet_driver_loopback.h                               1         0       0          0/0/0           0
sdk/rocketnet_encryption_map.cpp                            175         6       0        12/11/4           0
sdk/rocketnet_encryption_map.h                                1         0       0          0/0/0           0
sdk/rocketnet_endpoint.cpp                                  646        97     174          0/0/0           0
sdk/rocketnet_endpoint.h                                     11         0       0          0/0/0           0
sdk/rocketnet_fragment.cpp                                  302        88       3          0/0/0           0
sdk/rocketnet_fragment.h                                      8         1       0          0/0/0           0
sdk/rocketnet_frame_sync.cpp                                119        47      24          0/8/0           0
sdk/rocketnet_frame_sync.h                                    4         0       0          0/0/0           0
sdk/rocketnet_gateway_netlink.h                               0         5       1          0/0/0           0
sdk/rocketnet_gateway_netlink_linux.cpp                       0       173     140          0/0/0           0
sdk/rocketnet_hash.cpp                                        0        19       0          0/0/0           0
sdk/rocketnet_history.h                                       0         2       0          0/0/0           2
sdk/rocketnet_internal.h                                     72        18      25          0/0/0           5
sdk/rocketnet_jitter_tracker.h                               40         0       0          0/0/0           0
sdk/rocketnet_keys.cpp                                       46         1       0          0/0/0           0
sdk/rocketnet_layers.h                                       38        14       6          0/0/0           3
sdk/rocketnet_memory.h                                       28         0       0          0/0/0           0
sdk/rocketnet_message.cpp                                   313        71       8          0/4/0           6
sdk/rocketnet_message.h                                      24        12       0          0/0/0           0
sdk/rocketnet_message_client.cpp                            193        36      18          0/0/0           0
sdk/rocketnet_message_object.h                                6         8       0          0/0/0           8
sdk/rocketnet_message_server.cpp                            864       162     146          0/0/0           0
sdk/rocketnet_network_simulator.cpp                           0         0       0     393/39/118           0
sdk/rocketnet_out_of_order_tracker.h                          0        28       5          0/0/0           0
sdk/rocketnet_packet_builder.cpp                            290        21      15          1/0/0           0
sdk/rocketnet_packet_builder.h                                5         0       0          0/0/0           0
sdk/rocketnet_packet_lease.h                                 62         0       6          0/0/0           0
sdk/rocketnet_packet_loss_tracker.h                          56         0       0          0/0/0           0
sdk/rocketnet_pending_frames.h                                0        16       0          0/0/0           0
sdk/rocketnet_platform.cpp                                 1056        60     263        79/28/6          22
sdk/rocketnet_platform.h                                     46         0       2          0/0/0           0
sdk/rocketnet_protocol.cpp                                  227        37      18          0/0/0           0
sdk/rocketnet_protocol.h                                     15         1       0          0/0/0           1
sdk/rocketnet_read_write.h                                   69        46       4          0/0/0           0
sdk/rocketnet_replay.cpp                                     38         4       0          0/0/0           0
sdk/rocketnet_replay.h                                        1         0       0          0/0/0           0
sdk/rocketnet_rio_layout.h                                    0        89       0          0/0/0          89
sdk/rocketnet_route.h                                        56         0       5          0/0/0           0
sdk/rocketnet_send_budget.h                                 124        31       3          0/0/0           0
sdk/rocketnet_send_handoff.h                                  0        24       1          0/0/0          25
sdk/rocketnet_server.h                                        2         2       0          0/0/0           0
sdk/rocketnet_server_gso.h                                   26         0       0          0/0/0           0
sdk/rocketnet_server_gso_linux.cpp                          696        10     138         48/0/2           0
sdk/rocketnet_server_protocol.cpp                           888       338      88          3/0/0           0
sdk/rocketnet_server_protocol.h                               8         0       0          0/0/0           0
sdk/rocketnet_server_replay_marks.cpp                         0        79       6         0/48/6           0
sdk/rocketnet_server_rio.h                                    0         0       1          0/0/0           1
sdk/rocketnet_server_socket.h                                 2         0       1          0/0/0           0
sdk/rocketnet_server_socket_linux.cpp                       537        26    1447          0/0/0           0
sdk/rocketnet_server_transport.cpp                         1501       272     248      24/144/14           1
sdk/rocketnet_server_uring.h                                 24         4       2          0/1/0           1
sdk/rocketnet_server_uring_linux.cpp                        643        38     205      43/137/27         183
sdk/rocketnet_slots.cpp                                      25         2       3          0/0/0           0
sdk/rocketnet_slots.h                                         5         0       0          0/0/0           0
sdk/rocketnet_stats_window.h                                 12         0       0          0/0/0           0
sdk/rocketnet_test_seams.h                                    0         0       0          0/1/0           0
sdk/rocketnet_test_vectors.h                                  5         0       0          0/0/0           0
sdk/rocketnet_testing.h                                       0         8       0          0/0/0           8
sdk/rocketnet_token_tables.cpp                              184        54      12          0/0/0           0
sdk/rocketnet_uring_codec.h                                 112        30       1          0/0/0           0
sdk/rocketnet_window_ack.cpp                                487       111       9          0/4/0           8
sdk/rocketnet_window_ack.h                                   14         0       0          0/0/0           0
sdk/rocketnet_window_receive.cpp                             58        16       0          0/0/0           0
sdk/rocketnet_window_send.cpp                               131         6       2          0/0/0           0
sdk/rocketnet_wire.h                                         54        13       0          0/0/0          12
sdk/rocketnet_xdp_compat.h                                    0         0      25          0/0/0           0
sdk/rocketnet_xdp_frame.h                                    72       283      14          0/0/0           0
sdk/rocketnet_xdp_free_list.h                                 0        10       0          0/0/0           0
sdk/rocketnet_xdp_limits.h                                    2         1       0          0/0/0           1
sdk/rocketnet_xdp_replay_marks.h                             30        26       0          0/0/0           0
sdk/rocketnet_xdp_socket_config.h                             0        15       0          0/0/0           0
------------------------------------------------------------------------------------------------------------
TOTAL (sdk/ less sdk/rocketnet_tests.cpp)                 19200      5077    4093    619/808/238         686
```

The suite's own source, `sdk/rocketnet_tests.cpp`, is reported apart: 0 production,
9679 unit-only, 108 never. It is not in the findings.

Lines left out of the findings, with counts. Seam lines, compiled only under the
test-seam macros: 619 production, 808 unit-only, 238 never. `sdk/rocketnet_rio_layout.h`
is 89 unit-only lines with no mapping in the Linux production objects (another OS).
`sdk/rocketnet_testing.h` is 8 unit-only lines, also absent from those objects.
Log-only names (`sdk/rocketnet.cpp:200` through `:202`, the debug and spam level
strings) are not findings. The device path this run does not open accounts for the
bulk of the never column in `sdk/rocketnet_server_socket_linux.cpp` (1447),
`sdk/rocketnet_server_uring_linux.cpp` (205), `sdk/rocketnet_server_gso_linux.cpp`
(138), `sdk/rocketnet_client_gso_linux.cpp` (43), and `sdk/rocketnet_xdp_compat.h`
(25). Those are attach, thread, and device-query lines, not a separate finding each.

## Findings

### 1. risk — `sdk/rocketnet.cpp:492`

- Defect: When `bytes > SIZE_MAX - overhead` (`sdk/rocketnet.cpp:490`), `rocketnet_malloc` increments the thread's refusal count and returns NULL, and that body is in the never set.
- Failure or cost: An allocation that cannot hold its own header is specified to fail closed, and no tier passes a size that large, so a comparison that wrapped the length would hand it to the allocator with nothing failing.
- Smallest fix: A unit test that calls `rocketnet_malloc` with a size above `SIZE_MAX - overhead`, expects NULL, and expects the refusal count to rise by one. The test takes no clock and no socket.

### 2. risk — `sdk/rocketnet_client_transport.cpp:598`

- Defect: `rocketnet_client_race_send_batch` is installed when the inner operations have a batch send (`sdk/rocketnet_client_transport.cpp:1619`) and its body is in the never set, as are the receive-batch, wait, and send-failure operations installed at lines 1620, 1621, and 1623.
- Failure or cost: Before a cookie commits, a batch addressed to the first server is also sent to the second server, one packet at a time (lines 619–623). No tier sends that batch, so a missed copy, or a send on the wrong family, is invisible to both suites.
- Smallest fix: A functional test. The operations are not a public entry and a unit test takes no socket. Connect a client at two server addresses and send a batch before the first cookie, and check both families were sent that packet.

### 3. risk — `sdk/rocketnet_gateway_netlink_linux.cpp:466`

- Defect: `rocketnet_gateway_netlink_route` (and `rocketnet_gateway_netlink_neighbor` below it) calls `rocketnet_gateway_netlink_dump` at line 470, and the dump and both entries are in the never set. The byte-buffer parsers they forward to are unit-only.
- Failure or cost: A kernel error, a short read, or a socket failure returns through this entry, and no tier opens the socket, so those returns are untested. The byte-buffer match is a different, already filed defect.
- Smallest fix: A functional test, because a unit test takes no socket. Call `rocketnet_gateway_netlink_route` for each address family and require a named result (found, not found, malformed, kernel error, or socket error) rather than an unterminated read.

### 4. quality — `sdk/rocketnet_ack.cpp:251`

- Defect: `rocketnet_sent_record_state_equal`, `rocketnet_received_record_state_equal`, and `rocketnet_aggregate_state_equal` are in the never set, and so are their only call lines (`sdk/rocketnet_ack.cpp:285`, `:288`, `:291`) inside `rocketnet_ack_endpoint_state_equal`.
- Failure or cost: A written sent, received, or aggregate record that differs is specified to make the endpoint compare return false, and the connection-reset test and `sdk/rocketnet_server_transport.cpp:411` can return before those loops, so a wrong field compare would not fail a run.
- Smallest fix: A unit test that sets one written bit, differs one field, and expects false, plus one that matches and expects true. No clock.

### 5. quality — `sdk/rocketnet.cpp:104`

- Defect: A second `rocketnet_config_parse_int_value` is compiled only into the XDP and IOCP test binaries (they do not link `sdk/rocketnet_config.cpp`), and its body is in the never set. The definition the unit suite runs is `sdk/rocketnet_config.cpp:426`.
- Failure or cost: The copy those binaries link can drift from the parser the suite actually checks, and nothing in this run enters the copy, so the drift would not fail a test.
- Smallest fix: Call the local copy from those binaries with the same vectors `tests/test_unit_status.cpp` already uses, or link the one definition.

### 6. quality — `sdk/rocketnet_read_write.h:135`

- Defect: `rocketnet_read_uint8_bounded` (`:117`), `rocketnet_read_uint16_bounded` (`:126`), and `rocketnet_read_uint32_bounded` (`:135`) are unit-only, and their only callers are in `sdk/rocketnet_tests.cpp`.
- Failure or cost: A short buffer's refusal is checked only by the sdk test file. No production parser calls the three bounded readers, so a production read that ignores its end pointer is not this code and is not caught here.
- Smallest fix: Leave them as the suite's checked readers and say so in the header, or call them from the production reader that walks an end-bounded packet. Do not add a second unbounded read beside them.

### 7. quality — `sdk/rocketnet_crypto_tally.h:31`

- Defect: `rocketnet_crypto_tally_set` has no caller in `sdk/` or `tests/`, and its body is in the never set. The header says the tally exists so a test can show that a cheap refusal did no expensive cryptography.
- Failure or cost: A test cannot install a baseline through this function, and a later edit that starts writing the tally from a production path has no reader expecting the installed value.
- Smallest fix: Delete the setter, or call it from one unit test that checks `rocketnet_crypto_tally_get` returns the value just set.

## Counts

- risk: 3 (findings 1–3)
- quality: 4 (findings 4–7)
- bug: 0
- total: 7

## Already filed

- `sdk/rocketnet_out_of_order_tracker.h:49`, the tracker has no production caller. `docs/audits/2026-10-24-code-quality-aurora.md` finding 13. This run agrees: 28 unit-only lines, 5 never, 0 production.
- `rocketnet_message_client_receive_channel` and `rocketnet_message_server_receive_channel` are in the never set (each 9 lines). `docs/audits/2026-10-24-area-protocol-channels-messages.md` finding 2.
- The multipath match at `sdk/rocketnet_gateway_netlink_linux.cpp:225` is `docs/audits/2026-10-24-area-transports-linux-af-xdp.md` finding 5. The parsers that finding describes are unit-only here. Finding 3 above is the socket entry, which that finding does not cover.
