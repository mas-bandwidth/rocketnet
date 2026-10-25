# LAN proof, xdp cell, 1000 clients

Date: 2026-10-25 (UTC). Head: 534cce2e. Runner: `scripts/lan-proof --xdp-uplink`.

Verdict: PASS for delivery and connection. The 1000-client xdp cell ran for
300 s with the server bare metal on its machine's only cabled 10G port, which
is also that machine's uplink; every client stayed connected, every mode and
size class delivered, and the uplink answered ssh throughout. The merger's
timing verdict is FAIL (see below), as it is for the canned rows the runner's
self test checks; the table row's verdict is the delivery and connection
verdict.

## Result

| cell | server | NIC speed (Mb/s) | clients | duration (s) | connected | timeouts | mode delivery | size delivery | min kbps | verdict |
|---|---|---|---|---|---|---|---|---|---|---|
| xdp | linux-server-1 | 10000 | 1000 | 300 | yes | 0 | 0-7 pass | small, fragmented pass | 283 | pass |

The row as printed:

```text
xdp|linux-server-1|10000|1000|300|yes|0|0:pass,1:pass,2:pass,3:pass,4:pass,5:pass,6:pass,7:pass|small:pass,fragmented:pass|283|pass
```

Server: `split_proof: transport=xdp transport_pin=xdp min_client_up_kbps=283.964800 min_client_down_kbps=286.562187 checksum_errors=0 receipt_errors=0 receipts_dropped=0 disconnects=0 verdict=PASS`.

Merger: `split_proof_cell: verdict=PASS transport=xdp total=1000 streams=26000 messages_sent=12000000 messages_delivered=11997431 unreliable_lost=1457 sequenced_skipped=1112 timing_verdict=FAIL`.

The server's AF_XDP sockets bound in copy mode on all 7 queues: the driver
refused the zero copy bind (`Operation not supported`) and the server fell
back, which is the mode it picks by default on this NIC. Clients ran the gso
transport (the client ranking's native pick on every shard).

## Machines

| label | role | OS, kernel | NIC | driver | speed, duplex |
|---|---|---|---|---|---|
| linux-server-1 | xdp server, bare metal | Linux 6.8.0-142-generic, 17 cpus, nosmt, isolcpus 1-15 | Intel X540-AT2 port 1 | ixgbe 6.2.5 | 10000 Mb/s, full |
| linux-client-1 | 2 shards of 250 | Linux 7.0.0-38-generic, 16 threads | Aquantia AQC113 | atlantic | 1000 Mb/s, full |
| linux-client-2 | 1 shard of 250 | Linux 7.0.0-38-generic, 16 threads | Aquantia AQC113 | atlantic | 1000 Mb/s, full |
| linux-client-3 | 1 shard of 250 | Linux 7.0.0-38-generic, 16 threads | Aquantia AQC113 | atlantic | 1000 Mb/s, full |

The server port carries the machine's LAN address, its default route and its
only management link.

## Safeguards

1. The XDP program steers only the configured address and port. The built
   `server_xdp.o` at this head (sha256 `5eb8b71dc3917af6...`) was judged in a
   guest with `bin/test_xdp_filter -v build/generated/server_xdp.o`: 18 cases
   ok, among them the frames to another address, another port, ICMPv6 and
   other-port fragments returning `XDP_PASS`. Nothing was attached on the host
   for that check.
2. The interface was checked read only before the run
   (`rocketnet-provision enp9s0f1 7 verify 1-15` passed every boot and runtime
   condition; nothing was reprovisioned) and had no XDP program.
3. A self-detach was armed before the attach through the machine's own
   service manager, not tied to ssh:
   `xdp-detach-armed: unit=lan-proof-xdp-detach-<run id>.timer iface=enp9s0f1 fires_in_s=600`
   (duration plus 300 s). It kills the run's server and sets the interface
   `xdpgeneric`, `xdpdrv` and `xdp` off. The server start refuses while that
   timer is not armed.
4. A 30-client, 30 s smoke ran first on the same port, in the same mode.
   Row: `xdp|linux-server-1|10000|30|30|yes|0|0:pass,...,7:pass|small:pass,fragmented:pass|289|pass`.
   The runner's ssh check of the server machine answered every 5 s (9 of 9),
   and a separate probe from the controller answered every time; the longest
   answer was 7 s, at the attach.
5. During the 1000-client cell the runner's ssh check answered 61 of 61
   times (every 5 s from 0 to 300 s), and the separate probe answered every
   time, the longest in 6 s.
6. After each run the script detached XDP, cancelled the timer and checked
   the link:
   `xdp-detached: iface=enp9s0f1 xdp=none timer=lan-proof-xdp-detach-<run id>.timer not armed` and
   `ip-link: 5: enp9s0f1: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 qdisc mq state UP mode DEFAULT group default qlen 1000`.
   A final check after both runs found no `xdp` in `ip -d link show dev enp9s0f1`,
   no lan-proof timer or unit, and no load_split process on any host.

## Load

Every host's load at the start was under half its cores (server 1.70 of 17;
clients 3.21, 2.24, 1.42 of 16). Runtime samples of the server stayed at or
under 7.60 of 17 cores; client end loads were at most 4.26 of 16.

## Receipts

Run id `5a0be0000000c1e0`. sha256 prefixes: allocation.json `231ea29fb2912cae`,
server.log `be99d6737f53673b`, shard0.log `a657b4c44d9ed0fc`, shard1.log
`638288516dd8742c`, shard2.log `66fb4d91efc221f4`, shard3.log
`b7d091369c68e0e4`, merge.log `c656928519103813`.

## Command

```text
scripts/lan-proof --inventory INVENTORY --cell xdp --duration 300 --clients 1000 \
    --commit 534cce2e10159b52074ea9b10f7a2f0b074f7298 --run-dir DIR \
    --run-id 5a0be0000000c1e0 --xdp-uplink enp9s0f1
```

## Not done

- No zero copy: the driver refused it on every queue, so this is a copy mode
  result.
- The merger's timing verdict is FAIL; it was not investigated on this run.
- Only 300 s, not the 600 s of the other LAN cells.
