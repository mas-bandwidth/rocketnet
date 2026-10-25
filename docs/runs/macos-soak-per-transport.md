# macOS per-transport soak, 2026-10-25 UTC

Source head for both cells: `efb45f526d49a848ff4678b750cec75d4b1a9aa4`
(main `6fa04f88de20c5cc8eaa2b35952f15166eae3a98` plus the soak-macos
workflow steps; no compiled file differs from main).

Machine label: `macos-soak-1`. macOS 27.2 (build 26B118), Darwin 27.2.0
(xnu-13432.41.4~25, RELEASE_ARM64_T6050), Apple Mac17,16. Each cell is one
process hosting the server and the client on this machine over loopback.

Interfaces, from `networksetup -listallhardwareports`,
`system_profiler SPEthernetDataType` and `ifconfig <if>` at the run head.
A dash means the interface was inactive and had no negotiated link.

| Interface | Model / port | Driver | Negotiated speed and duplex |
| --- | --- | --- | --- |
| en0 | Realtek 8127 (10 GbE) | `com.apple.driver.AppleEthernetRL` | 1000baseT, full duplex |
| en1 | Wi-Fi | not read | — (inactive) |
| en2, en3, en7 | Thunderbolt 1, 3, 2 (bridge0 members) | not read | — (inactive) |
| en4, en5, en6 | Ethernet adapter | not read | — (media none, inactive) |
| bridge0 | Thunderbolt Bridge | macOS software bridge | — (inactive) |

Build: `make -j4` at the source head, exit 0, 0 warnings, last line
`[100%] Built target test`.

Both cells ran back to back, each inside the bench's single run slot, with
5% loss, 30 ms latency, 20 ms jitter and 2% duplication on both ends, the
same settings as the Linux per-transport cells. `timeout` bounds teardown to
120 s past the 1800-second soak (exit 124 would be a failure).

```sh
env ROCKETNET_SERVER_TRANSPORT=portable ROCKETNET_CLIENT_TRANSPORT=portable \
    ROCKETNET_SERVER_SIMULATED_LOSS_PERCENT=5 ROCKETNET_CLIENT_SIMULATED_LOSS_PERCENT=5 \
    ROCKETNET_SERVER_SIMULATED_LATENCY_MS=30 ROCKETNET_CLIENT_SIMULATED_LATENCY_MS=30 \
    ROCKETNET_SERVER_SIMULATED_JITTER_MS=20 ROCKETNET_CLIENT_SIMULATED_JITTER_MS=20 \
    ROCKETNET_SERVER_SIMULATED_DUPLICATE_PERCENT=2 ROCKETNET_CLIENT_SIMULATED_DUPLICATE_PERCENT=2 \
    timeout 1920 ./bin/soak --seconds 1800 --assert-flat > portable.log 2>&1
```

```sh
env ROCKETNET_SERVER_TRANSPORT=msgx ROCKETNET_CLIENT_TRANSPORT=msgx \
    ROCKETNET_SERVER_SIMULATED_LOSS_PERCENT=5 ROCKETNET_CLIENT_SIMULATED_LOSS_PERCENT=5 \
    ROCKETNET_SERVER_SIMULATED_LATENCY_MS=30 ROCKETNET_CLIENT_SIMULATED_LATENCY_MS=30 \
    ROCKETNET_SERVER_SIMULATED_JITTER_MS=20 ROCKETNET_CLIENT_SIMULATED_JITTER_MS=20 \
    ROCKETNET_SERVER_SIMULATED_DUPLICATE_PERCENT=2 ROCKETNET_CLIENT_SIMULATED_DUPLICATE_PERCENT=2 \
    timeout 1920 ./bin/soak --seconds 1800 --assert-flat > msgx.log 2>&1
```

| Cell | Window (UTC) | Result | Granted server / client | Log SHA-256 |
| --- | --- | --- | --- | --- |
| portable | 05:17:17 – 05:47:17 | PASS, exit 0; 1,148,889 iterations, 60 connects, 2,224,774 payloads verified, 0 failures | portable / portable | `10c02105516cc58dbd7b6035b913972fd4057fa8c7c572092825f82e3b15a9bc` |
| msgx | 05:47:17 – 06:17:17 | PASS, exit 0; 1,148,654 iterations, 60 connects, 2,231,633 payloads verified, 0 failures | msgx / msgx | `81af0a9f50ef383dc59bceda42a7609ca3b7821f7296c194caf226a55775b302` |

Load average (1, 5, 15 min): 4.74 2.23 1.96 at the portable start (the
unit suite had just finished), 0.82 0.94 1.19 between the cells, and
1.10 0.93 0.94 at the msgx end, on a machine with 18 cores.

Portable resource lines:

```text
soak resources: memory guard library_bytes
soak resources: fds start=7 end=7 peak=9
soak resources: threads start=3 end=3 peak=4
soak resources: resident_bytes start=23724032 end=24117248 peak=24150016
soak resources: library_bytes start=105680858 end=105680858 peak=125552591
soak finished: 1148889 iterations, 60 connects, 2224774 payloads verified, 0 failures
soak summary: server_transport_io portable
soak summary: client_transport_io portable
```

Msgx resource lines:

```text
soak resources: memory guard library_bytes
soak resources: fds start=7 end=7 peak=9
soak resources: threads start=3 end=3 peak=4
soak resources: resident_bytes start=23609344 end=23822336 peak=23855104
soak resources: library_bytes start=105680858 end=105680858 peak=125552710
soak finished: 1148654 iterations, 60 connects, 2231633 payloads verified, 0 failures
soak summary: server_transport_io msgx
soak summary: client_transport_io msgx
```

Neither log has a `REFUSAL` or an `--assert-flat FAILED` line. On macOS the
flatness guard is the library's allocator bytes; resident bytes are printed,
not asserted.

Unit suite on this machine at the source head: `./bin/test` exit 1,
`check failed: test_server_stats_current_every_update peak_rss_mb=1148
bound_mb=698`. The same failure reproduces at main `6fa04f88`, so it
predates this record and is not part of either soak cell.

Verdict: PASS for both cells.
