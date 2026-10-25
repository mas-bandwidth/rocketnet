# macOS per-transport soak, 2026-10-25 UTC

Source head for both cells: `31ad78624a5d46f873678ecaaeb884bdbe1af72f`
(main `e8d209cc3f445b9dba7f8f585d7fd88f054e4b50`, which applies the soak's
requested link impairment and prints each end's applied setting, plus the
soak-macos workflow steps; no compiled file differs from that main).

Machine label: `macos-soak-1`. macOS 27.2 (build 26B118), Darwin 27.2.0
(xnu-13432.41.4~25, RELEASE_ARM64_T6050), Apple Mac17,16, 18 cores. Each
cell is one process hosting the server and the client on this machine over
loopback.

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
`[100%] Built target functional`. Unit suite: `./bin/test` exit 0,
`*** ALL TESTS PASSED ***`, `test_server_stats_current_every_update`
included.

## The cells

Both cells ran back to back, each in its own turn of the bench's single run
slot, with 5% loss, 30 ms latency, 20 ms jitter and 2% duplication at both
ends of the link simulator, the values scripts/soak-per-transport runs on
Linux. Each cell is the soak-macos workflow step "Per-transport impaired
soaks", run as written with its transport list narrowed to that cell:

```sh
env ROCKETNET_SERVER_TRANSPORT=portable ROCKETNET_CLIENT_TRANSPORT=portable \
    ROCKETNET_DEV_SERVER_PACKET_LOSS_PERCENT=5 ROCKETNET_DEV_CLIENT_PACKET_LOSS_PERCENT=5 \
    ROCKETNET_DEV_SERVER_LATENCY_MS=30 ROCKETNET_DEV_CLIENT_LATENCY_MS=30 \
    ROCKETNET_DEV_SERVER_JITTER_MS=20 ROCKETNET_DEV_CLIENT_JITTER_MS=20 \
    ROCKETNET_DEV_SERVER_DUPLICATE_PERCENT=2 ROCKETNET_DEV_CLIENT_DUPLICATE_PERCENT=2 \
    ./bin/soak --seconds 1800 --assert-flat > build/soak-macos-per-transport/portable.log 2>&1
```

```sh
env ROCKETNET_SERVER_TRANSPORT=msgx ROCKETNET_CLIENT_TRANSPORT=msgx \
    ROCKETNET_DEV_SERVER_PACKET_LOSS_PERCENT=5 ROCKETNET_DEV_CLIENT_PACKET_LOSS_PERCENT=5 \
    ROCKETNET_DEV_SERVER_LATENCY_MS=30 ROCKETNET_DEV_CLIENT_LATENCY_MS=30 \
    ROCKETNET_DEV_SERVER_JITTER_MS=20 ROCKETNET_DEV_CLIENT_JITTER_MS=20 \
    ROCKETNET_DEV_SERVER_DUPLICATE_PERCENT=2 ROCKETNET_DEV_CLIENT_DUPLICATE_PERCENT=2 \
    ./bin/soak --seconds 1800 --assert-flat > build/soak-macos-per-transport/msgx.log 2>&1
```

A cell passes when soak exits 0 (which with `--assert-flat` includes the
impairment leaving drops missing and duplicates refused in each direction),
both ends grant the pinned transport, and both "soak link simulator:" lines
show the requested setting.

| Cell | Window (UTC) | Result | Granted server / client | Log SHA-256 |
| --- | --- | --- | --- | --- |
| portable | 08:28:00 – 08:58:00 | PASS, exit 0; 1,129,914 iterations, 60 connects, 2,120,746 payloads verified, 0 failures | portable / portable | `d3507087fd01a8a2ef516b8f9b0c2ae5abb1e6409619955c13d0470c50b7a28d` |
| msgx | 08:58:00 – 09:28:01 | PASS, exit 0; 1,129,817 iterations, 60 connects, 2,120,706 payloads verified, 0 failures | msgx / msgx | `602ce48f36eddfb32760b917bc6b5a3e3d733143298f08cf5c9d8224d07b444a` |

Load average (1, 5, 15 min): 3.23 2.14 2.50 at the portable start (the
unit suite had just finished), 1.48 1.60 1.75 between the cells, and
1.88 1.92 1.77 at the msgx end.

Cell lines, as the workflow step prints them:

```text
SOAK cell=portable PASS server=portable client=portable seconds=1800 verified=2120746 fds=7/7/9 threads=3/3/4 resident_bytes=23805952/24641536/24674304 link=impaired missing=55715/55927 duplicates_refused=34041/32953 log=build/soak-macos-per-transport/portable.log
SOAK cell=msgx PASS server=msgx client=msgx seconds=1800 verified=2120706 fds=7/7/9 threads=3/3/4 resident_bytes=23674880/24707072/24739840 link=impaired missing=55643/56040 duplicates_refused=34032/32662 log=build/soak-macos-per-transport/msgx.log
```

Portable, applied settings, witnesses and resource lines:

```text
soak link simulator: server latency_ms=30 jitter_ms=20 loss_percent=5 duplicate_percent=2
soak link simulator: client latency_ms=30 jitter_ms=20 loss_percent=5 duplicate_percent=2
soak link witness: client_to_server missing=55715 late=654567 repeated=0 duplicates_refused=34041
soak link witness: server_to_client missing=55927 late=654353 repeated=0 duplicates_refused=32953
soak resources: memory guard library_bytes
soak resources: fds start=7 end=7 peak=9
soak resources: threads start=3 end=3 peak=4
soak resources: resident_bytes start=23805952 end=24641536 peak=24674304
soak resources: library_bytes start=105680858 end=105680858 peak=125552591
soak finished: 1129914 iterations, 60 connects, 2120746 payloads verified, 0 failures
soak summary: server_transport_io portable
soak summary: client_transport_io portable
```

Msgx, applied settings, witnesses and resource lines:

```text
soak link simulator: server latency_ms=30 jitter_ms=20 loss_percent=5 duplicate_percent=2
soak link simulator: client latency_ms=30 jitter_ms=20 loss_percent=5 duplicate_percent=2
soak link witness: client_to_server missing=55643 late=654908 repeated=0 duplicates_refused=34032
soak link witness: server_to_client missing=56040 late=654668 repeated=0 duplicates_refused=32662
soak resources: memory guard library_bytes
soak resources: fds start=7 end=7 peak=9
soak resources: threads start=3 end=3 peak=4
soak resources: resident_bytes start=23674880 end=24707072 peak=24739840
soak resources: library_bytes start=105680858 end=105680858 peak=125552591
soak finished: 1129817 iterations, 60 connects, 2120706 payloads verified, 0 failures
soak summary: server_transport_io msgx
soak summary: client_transport_io msgx
```

Neither log has a `REFUSAL` or an `--assert-flat FAILED` line; the
already-received refusals counted are the planted duplicates'. On macOS the
flatness guard is the library's allocator bytes; resident bytes are printed,
not asserted.

Verdict: PASS for both cells.

## Superseded: the earlier run at `efb45f52`

The rows below, from an earlier run at source head
`efb45f526d49a848ff4678b750cec75d4b1a9aa4` (main `6fa04f88`), are
superseded and do not count. That run set
`ROCKETNET_SERVER_SIMULATED_*` and `ROCKETNET_CLIENT_SIMULATED_*`, names
nothing read at that head, so both cells ran on a clean link; that soak
printed no applied settings and no link witnesses.

| Cell | Window (UTC) | Result then | Granted server / client | Status |
| --- | --- | --- | --- | --- |
| portable | 05:17:17 – 05:47:17 | exit 0; 2,224,774 payloads verified | portable / portable | superseded: clean link |
| msgx | 05:47:17 – 06:17:17 | exit 0; 2,231,633 payloads verified | msgx / msgx | superseded: clean link |

The same earlier run recorded `./bin/test` exit 1 on this machine,
`check failed: test_server_stats_current_every_update peak_rss_mb=1148 bound_mb=698`; at the source head above, with the page-scaled bound, it
passes.
