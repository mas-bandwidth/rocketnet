# macOS per-transport soak, 2026-10-25 UTC

Source head for both cells: `74b046cd48793c5dda7bf26a39ab2292f825db7c`.
Machine label: `macos-soak-1`; macOS 27.2 (build 26B118), Darwin
27.2.0, Apple Mac17,16. Each run uses one process hosting local server and
client transports on this machine.

Hardware-port inventory from `networksetup -listallhardwareports`,
`system_profiler SPEthernetDataType SPAirPortDataType SPThunderboltDataType`,
`ioreg -p IOService`, and `ifconfig <if>` at the run head. A dash in the
negotiated column means the interface was inactive and had no negotiated link.

| Interface | Model / port | Driver reported by macOS | Negotiated speed and duplex |
| --- | --- | --- | --- |
| en0 | Realtek 8127 (10 GbE) | `com.apple.driver.AppleEthernetRL` | 1000baseT, full duplex |
| en1 | Apple Wi-Fi, chip ID 0x11 | `IO80211_driverkit-1587.10` | — (inactive) |
| en2 | Thunderbolt 1 | `AppleThunderboltIPPort` | — (inactive) |
| en3 | Thunderbolt 3 | `AppleThunderboltIPPort` | — (inactive) |
| en7 | Thunderbolt 2 | `AppleThunderboltIPPort` | — (inactive) |
| en4 | USB NCM Ethernet Adapter | `AppleUSBDeviceNCM11Data` | — (media none, inactive) |
| en5 | USB NCM Ethernet Adapter | `AppleUSBDeviceNCM11Data` | — (media none, inactive) |
| en6 | USB NCM Ethernet Adapter | `AppleUSBDeviceNCM11Data` | — (media none, inactive) |
| bridge0 | Thunderbolt Bridge | macOS software bridge | — (inactive) |

Each cell was invoked through `/Users/glenn/bin/benchslot -- /bin/bash`
with its named run script, from the checkout at the source head. The scripts
ran the commands below and kept full stdout and stderr in their named logs.
The `timeout` bounds teardown to 120 seconds beyond the requested
1800-second soak; exit 124
would be a failure. The simulator settings match the Linux per-transport
cells: both ends have 5% loss, 30 ms latency, 20 ms jitter, and 2%
duplication.

```sh
env ROCKETNET_SERVER_TRANSPORT=portable ROCKETNET_CLIENT_TRANSPORT=portable \
    ROCKETNET_SERVER_SIMULATED_LOSS_PERCENT=5 ROCKETNET_CLIENT_SIMULATED_LOSS_PERCENT=5 \
    ROCKETNET_SERVER_SIMULATED_LATENCY_MS=30 ROCKETNET_CLIENT_SIMULATED_LATENCY_MS=30 \
    ROCKETNET_SERVER_SIMULATED_JITTER_MS=20 ROCKETNET_CLIENT_SIMULATED_JITTER_MS=20 \
    ROCKETNET_SERVER_SIMULATED_DUPLICATE_PERCENT=2 ROCKETNET_CLIENT_SIMULATED_DUPLICATE_PERCENT=2 \
    timeout 1920 ./bin/soak --seconds 1800 --assert-flat > ../logs/portable.log 2>&1
```

```sh
env ROCKETNET_SERVER_TRANSPORT=msgx ROCKETNET_CLIENT_TRANSPORT=msgx \
    ROCKETNET_SERVER_SIMULATED_LOSS_PERCENT=5 ROCKETNET_CLIENT_SIMULATED_LOSS_PERCENT=5 \
    ROCKETNET_SERVER_SIMULATED_LATENCY_MS=30 ROCKETNET_CLIENT_SIMULATED_LATENCY_MS=30 \
    ROCKETNET_SERVER_SIMULATED_JITTER_MS=20 ROCKETNET_CLIENT_SIMULATED_JITTER_MS=20 \
    ROCKETNET_SERVER_SIMULATED_DUPLICATE_PERCENT=2 ROCKETNET_CLIENT_SIMULATED_DUPLICATE_PERCENT=2 \
    timeout 1920 ./bin/soak --seconds 1800 --assert-flat > ../logs/msgx.log 2>&1
```

| Cell | Result | Granted server / client | Log |
| --- | --- | --- | --- |
| portable | FAIL, exit 1; 2,282,131 payloads verified; client `SEND_RING_FULL = 107` and resident-byte flatness failed | portable / portable | `portable.log`, SHA-256 `017f0cf8fab00cad72e644f3aaee6d613a71ff586004747619a6cce13d67c017` |
| msgx | FAIL, exit 1; 2,281,734 payloads verified; client `SEND_RING_FULL = 74` and resident-byte flatness failed | msgx / msgx | `msgx.log`, SHA-256 `331ead57ed60e69b696915e07b0d0cabc6ec2eb8f3a7c178036e69490b5f8020` |

Portable ran from 01:53:28 to 02:23:28 UTC. The full raw log is retained
with the card evidence. Its final resource lines were:

```text
soak resources: fds start=7 end=7 peak=9
soak resources: threads start=3 end=3 peak=4
soak resources: resident_bytes start=24182784 end=28606464 peak=28639232
soak: --assert-flat FAILED: resident_bytes start=24182784 end=28606464 (allowed growth 49152 bytes over 1770 s)
soak resources: library_bytes start=105680858 end=105680858 peak=125552591
```

Msgx ran from 02:24:23 to 02:54:23 UTC. Its final resource lines were:

```text
soak resources: fds start=7 end=7 peak=9
soak resources: threads start=3 end=3 peak=4
soak resources: resident_bytes start=23543808 end=28442624 peak=28475392
soak: --assert-flat FAILED: resident_bytes start=23543808 end=28442624 (allowed growth 49152 bytes over 1770 s)
soak resources: library_bytes start=105680858 end=105680858 peak=125552591
```

Verdict: HOLD. Both cells ran for the full 1800 seconds at the same source
head, and both failed on a client send-ring refusal and resident-byte
flatness. These results do not establish why the resident bytes increased;
the library allocator count being equal at the beginning and end covers only
allocations made through that allocator.
