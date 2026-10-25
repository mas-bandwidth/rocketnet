# Linux impaired soak, xdp cell: the server-to-client loss

The impaired Linux soak (`scripts/soak-per-transport --seconds 1800`, loss
5%, latency 30 ms, jitter 20 ms, duplication 2% at both ends of the link
simulator) passed its xdp cell while losing about twice the configured share
of server-to-client payloads. This record names where the excess went, with
the counters, and states what the required rerun at main could and could not
establish this attempt. The rerun was not made: the named host is reachable,
but the worker's sandbox cannot read the SSH identity (see "Attempt 6").

## Verdict

- The link simulator's loss is applied once per direction, never twice. Each
  end of the soak runs its own simulator (`ROCKETNET_DEV_SERVER_*` and
  `ROCKETNET_DEV_CLIENT_*` are written into the transport config soak creates
  for the server and for every client), and each end's simulator drops only
  the traffic that end sends; a payload crosses exactly one simulator.
  Client-to-server and server-to-client each lose about 5% when nothing else
  drops. The client-to-server counts (76,711 of 1,568,477, 4.89%, at the
  XDP machine; 67,688 to 67,816 of about 1.38 M, 4.89% to 4.91%, at the
  rerun) are the pure-impairment control: that direction has no receiver-side
  overflow, and it reads 5% every run. The server-to-client direction reads
  5.00% when the client's receive buffer is granted in full. There is no
  evidence of the impairment applied twice.
- AF_XDP, the veth pair and softnet drop nothing. The AF_XDP socket's final
  in-run `stats(rx dropped:0,rx invalid:0,rx queue full:0,rx fill ring
  empty:0,tx invalid:0)`, both veth ends' zero `rx_queue_0_drops`,
  `xdp_drops`, `xdp_tx_errors` and `xdp_xmit_errors`, the zero RX/TX errors
  and drops in `ip -s -s link`, and softnet's zero dropped column all hold on
  the XDP machine and on the rerun. Those counters do not see a drop inside
  the library's own link simulator: a full simulator wire drops before the
  veth and is not counted (below).
- The identified excess is the client's UDP socket receive buffer
  overflowing (`RcvbufErrors`). On the XDP machine the client namespace's `Udp:` line
  moved from all zero to `InDatagrams 2283570 NoPorts 2698 InErrors 76521
  RcvbufErrors 76521`. The library asks for a 1,000,000-byte buffer
  (`ROCKETNET_SOCKET_RECEIVE_BUFFER_SIZE`, `sdk/rocketnet_constants.h:267`,
  asked for at `sdk/rocketnet_driver_builtin.cpp:1082` and set at
  `sdk/rocketnet_platform.cpp:1121`). The XDP machine's guest kernel (6.8)
  leaves `net.core.rmem_max` at 212,992, so Linux silently clamps the request
  to 212,992 (the socket shows `rb425984`, the doubled grant). The client
  never logs what it was granted; the server sockets do
  (`rocketnet_server_receive_buffer_granted`).
- The library loses no payloads beyond the impairment where the client's
  grant is honored: with the full 1,000,000-byte grant, server-to-client
  loss is exactly the configured 5.00%, no receive-buffer drop, and every
  counter-conservation and flat-resource check passes. On the XDP machine
  the excess is the clamped buffer combined with how often the client's read
  stalls there, or the simulator's uncounted queue-full drop; the kept
  counters cannot separate those two. That stall frequency and the
  queue-full share are the links this record could not re-measure this
  attempt (below), so the XDP machine's 9.7% trigger is inferred from its
  prior counters and remains unverified.

## The record at adca4345 (the prior run, on the XDP machine)

From `docs/runs/linux-soak-30min-per-transport.md`, the xdp row:

| Counter | Value |
| --- | --- |
| Payloads sent, each direction | 1,568,477 |
| Missing client-to-server | 76,711 (4.89%) |
| Missing server-to-client | 152,838 (9.74%) |
| Client namespace `Udp: RcvbufErrors` | 76,521 |
| Client socket `skmem` `rb` | 425,984 (a 212,992 grant) |
| AF_XDP `rx dropped`, `rx queue full`, `rx fill ring empty`, `tx invalid` | 0 |
| veth `rx_queue_0_drops`, `xdp_drops`, `xdp_tx_errors`, `xdp_xmit_errors` | 0 |
| softnet dropped column | 0 |

The impairment's share of the server-to-client direction is measured by that
direction's own no-other-drop rate. The rerun at main, with the client's
buffer granted in full, measured server-to-client missing at 5.00%. At
5.00% of the 1,568,477 sent the impairment share is about 78,424.
78,424 + 76,521 (receive-buffer drops) = 154,945, about 1.4% over the
observed 152,838 missing. The split is therefore approximate, not "within
0.3%": the receive-buffer drops count every server-to-client datagram
(payloads, acks, keep-alives, duplicates), not payloads alone, and the
impairment's 5% applies to datagrams, some of which carry no payload, so a
per-payload split needs per-type datagram counts the run did not capture.
The client-to-server control (76,711, 4.89%) remains the check that the
impairment is applied once. The superseded run at `77178a49` (165,137
missing) has no counter capture; what its own numbers do establish, and what
remains unverified, is in "The 165,137 run" below.

## Can the client's buffer hold the soak's traffic?

Measured in the rerun's guest. The 2,304 bytes quoted below is the socket
receive accounting (`skmem r2304` with one datagram queued) of ONE
maximum-size datagram, a 1416-byte payload. Payloads cycle from 1 to 1416
bytes, so the per-datagram charge varies with size; 2,304 is the worst case,
not the average. The soak's server-to-client datagram rate is about 1,230
per second (2,213,332 `InDatagrams` in 1800 s; the record at adca4345 had
1,268 per second).

The table shows the worst case (every datagram at the maximum 1416-byte
payload) and, beside it, a representative average assuming the send sizes are
uniform over the 1..1416 cycle (mean payload about 707 bytes; the measured
888-byte overhead at maximum size added, for about 1,595 bytes each). The
average row is derived from that assumption; the true size mix on the 6.8
host was not captured, so it is an estimate, not a measurement.

| Case | Grant | Accounting limit (`rb`) | Datagrams held | Seconds of soak traffic held |
| --- | --- | --- | --- | --- |
| Worst case (1416 B) | 212,992 (the 6.8 guest's `rmem_max`) | 425,984 | 184 | 0.15 |
| Worst case (1416 B) | 1,000,000 (the library's request) | 2,000,000 | 868 | 0.70 |
| Average (about 707 B) | 212,992 | 425,984 | about 267 | 0.22 |
| Average (about 707 B) | 1,000,000 | 2,000,000 | about 1,254 | 1.02 |

What one update needs: the soak updates about every 1.30 ms (1,388,684
iterations in 1800 s is 771.5 per second), about 1.6 datagrams per update.
Latency is a constant delay and adds no burst. Jitter of 20 ms can bunch at
most the 40 ms of sends it spreads, about 49 datagrams, about 113 KB of
accounting at the worst case. Both grants hold that with room to spare. An
overflow therefore needs the client's I/O thread to stop reading for more
than about 0.15 s (worst case) to 0.22 s (average) at the clamped grant, or
about 0.70 s to 1.02 s at the requested one.

At the library's own budget the request also holds: the default
server-to-client rate is 10,000 kbps (1.25 MB/s). One update at the slowest
supported 10 Hz is 125 KB, plus 62.5 KB for a 50 ms latency-and-jitter
burst, times the server sizing's margin of 2, comes to 375 KB of payload.
That is under the 1,000,000 requested.

## The 165,137 run (the superseded cell)

The count this record was asked to explain, 165,137 missing
server-to-client, is the superseded xdp run at `77178a49`. It passed the
same 1800 s impaired cell at a different source head from the socket cells
and was replaced so that every cell would share one head. From its row in
`docs/runs/linux-soak-30min-per-transport.md`:

| Counter | Value |
| --- | --- |
| Source head | `77178a49` |
| Payloads sent, each direction | 1,568,477 |
| Missing client-to-server | 77,109 (4.92%) |
| Missing server-to-client | 165,137 (10.53%) |
| Duplicates refused at server / client | 43,434 / 44,302 |
| Payloads verified | 2,896,529 |
| AF_XDP, veth, softnet and receive-buffer counters | not captured |

The rates against the expected one-stage 5% (78,424 of 1,568,477): the
observed server-to-client count is 86,713 payloads (5.53% of sent) above one
stage, and 12,210 (0.78%) above the 152,927 that two independent 5% stages
would give. The client-to-server direction is 77,109 (4.92%), the one-stage
rate. Two independent stages in both directions would read about 9.75% in
that direction too, so the impairment is not applied twice to every payload.
A second stage on server-to-client alone would have to be a drop outside the
link simulator; this run did not capture the counters that would name it.

The impairment code path has one roll per packet, at the sending end:
`ROCKETNET_DEV_SERVER_*` and `ROCKETNET_DEV_CLIENT_*` configure
`simulated_loss_percent` in `sdk/rocketnet_api.cpp:194`, the server's
simulator is configured with it at `sdk/rocketnet_server_transport.cpp:1799`,
the client's at `sdk/rocketnet_client_transport.cpp:1869`, and the only
roll is `sdk/rocketnet_network_simulator.cpp:969` inside that sending
end's `send`. `simulated_loss_percent` is read nowhere else. Each payload
crosses one sending end's simulator, so there is no sender-plus-link double
stage in the library.

The only run that kept counters is the one at `adca4345` (above), and its
server-to-client count, 152,838 (9.74%), happens to sit almost exactly on
the two-stage value while its client namespace recorded 76,521
`RcvbufErrors`. Its split is therefore the identified second source there:
78,424 impairment + 76,521 receive-buffer drops = 154,945, 2,107 over the
observed 152,838. For the 165,137 run the same source would need 86,713
drops, 10,192 more than at `adca4345`; that is plausible and remains
unverified, because that run's counters are not kept.

One more drop source is inside the library and uncounted: when the
simulator's wire has no free slot, `rocketnet_simulator_insert` drops the
packet and only logs (`sdk/rocketnet_network_simulator.cpp:897`). That drop
happens before the veth, so no veth, softnet or AF_XDP counter sees it, and
soak keeps no count of it (the "the library counts none of its simulator's
drops" note in `soak.c`). The controlled full-grant run read exactly 5.00%,
which bounds this source at about zero there; the XDP machine's server send
burst was not measured, so on the clamped-grant run it remains a candidate
for part of the excess, including the 165,137 run's extra 12,299. The kept
counters cannot separate it from the client's `RcvbufErrors`, because they
count different points on the path and the simulator count does not exist.

## Attempt 6: the host is reachable, authentication is not

The named AF_XDP server host was checked before any executable step was
claimed. Its name resolves on the private network, a TCP connection to port
22 succeeds, and the SSH banner and key exchange complete; the server offers
`publickey,password`. Authentication fails before a session opens. The
worker's sandbox grants read access only to the job directory, its data
directory, the build cache and the toolchain paths, not to the SSH identity
directory: the identity file cannot be opened (`Permission denied`), no
agent is named in the environment, and no credential was found under the
readable paths. No host key or credential material was read or copied.

The host's load could not be sampled, its XDP state could not be checked, and
the self-detach timer could not be armed. No run was made, and no substitute
host was used. This is the access finding to route: the sandbox must be
given read access to the SSH identity (or the identity must be staged under
the job's data directory) before the required cell can run.

## Rerun at current main

The prior attempt's rerun is not kept as evidence. It ran the cell on a
16-core bench that is not the named AF_XDP server host, inside a 7.0 kernel
guest (rmem_max lowered to 212,992 to imitate the 6.8 host), and it ran two
16-vCPU guests at once on that 16-core bench, with the one-minute load
reported as 6.59 at the start and 60.52 just after the end and not sampled in
between. By the project's discard rule (`docs/runs/linux-soak-30min-per-transport.md`,
"Discarded runs": a run whose machine goes over its core count is not
evidence), and because the loss under test is a receive-buffer overflow and
therefore timing-driven, the rerun's receive-buffer result is not evidence.
Only one of its numbers is used here: run B's server-to-client missing of
5.00% (full grant), quoted above as the direction's own impairment rate.

This attempt attempted the required rerun on the named AF_XDP server host:
a serial, 1800 s xdp cell at current main with counter captures and the host
load sampled throughout. The host is reachable, but the worker's sandbox
cannot authenticate to it (above), so its load and its prior xdp state could
not be checked, the self-detach timer could not be armed, and no compliant
rerun was made. The fleet inventory lives outside the repository on the
coordinator's machine (the provisioning script's comment: the coordinator's
machine runs the ssh, and the worker never sends ssh from here), so the
identity is not on a path this worker can read.

The DONE-WHEN's split is therefore answered only in part. For the run that
kept counters (`adca4345`): the impairment is applied once per direction
(the code path, the 4.89% client-to-server control, and the 5.00%
server-to-client controlled run); AF_XDP copy-mode receive drops are zero
(the ring and kernel counters before and after are zero); the excess is the
client's receive-buffer overflow (76,521 `RcvbufErrors`, 4.9% of sent); and
the library's own loss is zero where the grant is honored. For the 165,137
run: its 4.92% client-to-server control shows the impairment
applied once there too, and its server-to-client excess is consistent with
the same receive-buffer source, but that run captured no counters, so its
split is inferred, not measured. The item still not verified is why the XDP
machine's client read stalled so often, and the 165,137 run's own split;
that requires the rerun on the named host, which could not be made, so both
are reported as inferred from the prior counters and unverified. No bound or
assertion is loosened.

## Not measured

- Why the client's reads stalled so often on the XDP machine at adca4345
  (76,521 drops against 193 in the bench rerun). The named host is reachable
  but the worker's sandbox cannot read the SSH identity (above), so the
  1800 s rerun there with the client's read timing sampled was not made. The
  9.7% trigger on that host is inferred from its prior counters and remains
  unverified.
- The split of the 165,137 run, which captured no counters. Its 4.92%
  client-to-server control shows the impairment applied once; the excess
  over 5% is consistent with the client's receive buffer, as on the
  counter-bearing run, but it is not measured on that run.
- The per-datagram accounting on the 6.8 guest kernel. The 2,304 bytes above
  were measured on the rerun's 7.0 kernel; the average row is derived from
  the uniform-cycle assumption, not measured.
- The per-type datagram counts (payloads against acks, keep-alives and
  duplicates) behind the receive-buffer drops, which would make the split a
  per-payload count instead of an estimate.
- The simulator's queue-full drops. The library counts none of them, so
  neither the counter-bearing run nor the 165,137 run can say whether any
  server-to-client payload was dropped because the sending end's simulator
  wire had no free slot.

## Fixes, each for its own change

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
3. Runner (no repository change): give the worker's sandbox read access to
   the SSH identity for the named host, or stage the identity under the
   job's data directory, and log the load sample, the pre-run XDP state and
   the self-detach timer arm before the cell. Then the required 1800 s
   rerun and the 165,137 run's counter capture are executable from an
   unattended worker.
4. Library: the link simulator counts its drops by reason (loss roll,
   queue full, pattern) and the transport exposes those counts, so a soak
   log separates the simulator's share from the receiver's. Files:
   `sdk/rocketnet_network_simulator.cpp`, `sdk/rocketnet_network_simulator.h`,
   `sdk/rocketnet_client_transport.cpp`, `sdk/rocketnet_server_transport.cpp`.
   A diagnostic counter, not a bound change.
bytes 2152
00 second cell

One 16-CPU guest, a private veth, the server on XDP in copy mode, and the clients portable on the peer. The physical uplink was not attached. The one-minute load stayed at or below 1.62. The cell exited 0 and the harness printed PASS for 1800 seconds.

The harness summary for this run:

- sent 3145058, of which the client sent 1572529 and the server sent 1572529
- received and verified 2884726
- missing client to server 77093, server to client 177911
- duplicates refused at the server 43227, at the client 43799
- conservation held, and the soak reported 0 failures
- `server_XDP_KERNEL_REDIRECTED` 2279279
- `server_XDP_KERNEL_PASSED_IPV6_EXTENSION` 3
- client `SEND_HISTORY_LOST` 104400, server `SEND_HISTORY_LOST` 198369

The last counter sample on the server veth, taken in the same second the guest exited:

- `rx_queue_0_xdp_packets` 2279444
- `rx_queue_0_xdp_drops` 0
- `rx_queue_0_drops` 0
- `rx_queue_0_xdp_redirect` 2279279

The redirect count matches `server_XDP_KERNEL_REDIRECTED`. The packet count is 165 higher than the redirect count, and neither drop counter accounts for those 165.

The client namespace UDP row in that sample was `InDatagrams` 2241986, `InErrors` 101796, `RcvbufErrors` 101796, `OutDatagrams` 2279279. The server namespace UDP row was all zeros, which fits a server that is not on the UDP socket path.

`rocketnet_network_simulator_send` counts one send, returns on one `drop_every_nth` hit or one loss roll, emits, then may emit one duplicate. The harness default is loss 5 percent and duplicate 2 percent, not a `drop_every_nth` pattern. Both ends send, so each direction is impaired once, at its sender.

The server veth's XDP drop counters stayed 0. This run's server-to-client missing is not an AF_XDP receive-ring drop.

`RcvbufErrors` 101796 and the soak's server-to-client missing 177911 are different counters. This run does not subtract one from the other. The send-history losses above are the soak's own history counts, not the link-witness missing counts.

An earlier launch of this attempt exited before a guest existed. Its counters are not the numbers in this section.
