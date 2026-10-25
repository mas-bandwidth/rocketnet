# Linux impaired soak, xdp cell: the server-to-client loss

The impaired Linux soak (`scripts/soak-per-transport --seconds 1800`, loss
5%, latency 30 ms, jitter 20 ms, duplication 2% at both ends of the link
simulator) passed its xdp cell while losing about twice the configured share
of server-to-client payloads. This record names where the excess went, with
the counters, and reruns the cell at main.

## Verdict

- The link simulator's loss is applied once per direction. Client-to-server
  and server-to-client each lose 5% when nothing else drops.
- AF_XDP, the veth pair and softnet drop nothing.
- The excess is the client's UDP socket receive buffer overflowing
  (`RcvbufErrors`). The library asks for a 1,000,000-byte buffer
  (`ROCKETNET_SOCKET_RECEIVE_BUFFER_SIZE`, `sdk/rocketnet_constants.h:267`,
  asked for at `sdk/rocketnet_driver_builtin.cpp:1082` and set at
  `sdk/rocketnet_platform.cpp:1121`). The XDP machine's guest kernel
  (6.8) leaves `net.core.rmem_max` at its default of 212,992, so Linux
  silently clamps the request to 212,992 (the socket shows `rb425984`, the
  doubled grant). The client never logs what it was granted. The server
  sockets do (`rocketnet_server_receive_buffer_granted`).
- The library loses no payloads beyond the impairment. Its only fault is
  that it reports nothing when the client's grant is clamped. The fixes go on
  their own cards, below.

## The record at adca4345 (the prior run, on the XDP machine)

From `docs/runs/linux-soak-30min-per-transport.md`, the xdp row:

| Counter | Value |
| --- | --- |
| Payloads sent, each direction | 1,568,477 |
| Missing client-to-server | 76,711 (4.9%) |
| Missing server-to-client | 152,838 (9.7%) |
| Client namespace `Udp: RcvbufErrors` | 76,521 |
| Client socket `skmem` `rb` | 425,984 (a 212,992 grant) |
| AF_XDP `rx dropped`, `rx queue full`, `rx fill ring empty`, `tx invalid` | 0 |
| veth `rx_queue_0_drops`, `xdp_drops`, `xdp_tx_errors`, `xdp_xmit_errors` | 0 |
| softnet dropped column | 0 |

76,711 (the impairment's share, measured in the direction with no other
drop) + 76,521 (receive-buffer drops) = 153,232, within 0.3% of the 152,838
missing. The receive-buffer drops count every server-to-client datagram
(payloads, acks, keep-alives, duplicates), so this split is an estimate, not
a per-payload count.

The superseded run at `77178a49` (165,137 missing of the same order) has no
counter capture. Its excess is not split here.

## Can the client's buffer hold the soak's traffic?

Measured in the guest at main. One server-to-client datagram costs 2,304
bytes of the socket's receive accounting (`skmem r2304` with one datagram
queued). The kernel charges this per-datagram size, not the payload bytes.
The soak's server-to-client datagram rate is about 1,230 per second
(2,213,332 `InDatagrams` in 1800 s; the record at adca4345 had 1,268 per
second). Payloads cycle from 1 to 1416 bytes.

| Grant | Accounting limit (`rb`) | Datagrams held | Seconds of soak traffic held |
| --- | --- | --- | --- |
| 212,992 (the 6.8 guest's `rmem_max`) | 425,984 | 184 | 0.15 |
| 1,000,000 (the library's request) | 2,000,000 | 868 | 0.70 |

What one update needs: the soak updates about every 1.1 ms (1,388,684
iterations in 1800 s), about 1.4 datagrams per update. Latency is a constant
delay and adds no burst. Jitter of 20 ms can bunch at most the 40 ms of sends
it spreads, about 49 datagrams (113 KB of accounting). Both grants hold that
with room to spare. An overflow therefore needs the client's I/O thread to
stop reading for more than about 150 ms at the clamped grant, or 700 ms at
the requested one.

At the library's own budget the request also holds: the default
server-to-client rate is 10,000 kbps (1.25 MB/s). One update at the slowest
supported 10 Hz is 125 KB, plus 62.5 KB for a 50 ms latency-and-jitter
burst, times the server sizing's margin of 2, comes to 375 KB of payload.
That is under the 1,000,000 requested.

## Rerun at main, 1800 s, with counters

- Source head: `7d70806b6c7bf678d081fac09b611dd18ccefe5b` (main), built
  with `make -j4` under the bench slot lock, exit 0.
- Machine: a Linux bench, 16 cores, `uname -r` = `7.0.0-38-generic`. Its
  virtme-ng guest of the same kernel, 16 vCPUs, 4G, the documented
  `--append "irqaffinity=0 isolcpus=1 nohz_full=1"`. The guest setup, the
  environment and the soak command line are those of
  `scripts/soak-per-transport`'s xdp cell, with counter captures added:
  `/proc/net/snmp` in both namespaces, softnet, and `ip -s -s link` before
  and after, plus `ss -uamne` in the client namespace and `ss --xdp -e`
  every 30 s. Nothing was attached outside the guest.
- This bench is not the XDP machine. That machine was not used. This
  kernel's default `rmem_max` is 4,194,304, so run A sets the guest's
  `net.core.rmem_max` to 212,992 to reproduce the XDP machine's grant. Run
  B leaves it at the default, which grants the full 1,000,000.
- The two runs ran at the same time, one guest each. The bench's one-minute
  load was 6.59 at the start and 60.52 just after the end, from other work
  (compilers and a Go build). Load was not sampled in between. The rerun is
  loss evidence, not timing evidence.

| Run | Grant (`rb`) | Payloads sent each way | Missing c->s | Missing s->c | `RcvbufErrors` | AF_XDP / veth / softnet drops | soak exit |
| --- | --- | --- | --- | --- | --- | --- | --- |
| A | 212,992 (425,984) | 1,382,200 | 67,816 (4.91%) | 68,931 (4.99%) | 193 | 0 / 0 / 0 | 0 |
| B | 1,000,000 (2,000,000) | 1,384,120 | 67,688 (4.89%) | 69,200 (5.00%) | 0 | 0 / 0 / 0 | 0 |

Both runs logged `soak link simulator: server latency_ms=30 jitter_ms=20
loss_percent=5 duplicate_percent=2` and the same for the client, and no
`SEND_RING_FULL`, `SEND_LEASE_FULL` or `SEND_NO_LINK_ADDRESS`. Run A's 193
receive-buffer drops all fell in one 60 s window about 25 minutes in: 0 at
every 30 s sample before it, 33 at one sample, and 193 at every sample
after. On this bench the clamped buffer overflowed only once, which matches
one read stall longer than its 150 ms. With the requested buffer granted
there were no drops. Server-to-client loss at main is the configured 5%,
applied once.

## Not measured

- Why the client's reads stalled so often on the XDP machine at adca4345
  (76,521 drops against 193 here). That host was not used for this record.
  Its guest ran 16 vCPUs on a 17-core host whose load reached 7.87, and a
  6.8 kernel. A rerun there with the client's read timing sampled would name
  the stall.
- The per-datagram accounting on the 6.8 guest kernel. The 2,304 bytes
  above were measured on 7.0.

## Fixes, each for its own card

1. Library: the client logs its socket's receive buffer grant against its
   request, as the server sockets do, and warns naming
   `net.core.rmem_max` when the grant is smaller. Then a clamped client
   buffer shows in every log instead of only as `RcvbufErrors`. Files:
   `sdk/rocketnet_driver_builtin.cpp`, `sdk/rocketnet_platform.cpp`,
   `sdk/rocketnet_internal.h`.
2. Harness: the xdp cell's guest in `scripts/soak-per-transport` raises
   `net.core.rmem_max` and `net.core.wmem_max` to 4,194,304 before the
   soak. That matches the socket cells' machines (4,194,304 and
   67,108,864) and lets the library's 1,000,000-byte request be granted.
   This changes the test machine's limit and loosens no bound or
   assertion.
