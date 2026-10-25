# LAN 1000-client proof: macOS msgx server

One cell, msgx, run through `scripts/lan-proof` as
[LAN-PROOF.md](../LAN-PROOF.md) describes: 1000 clients in four shards of
250 on two Linux client machines, the server alone on the macOS machine,
traffic across the home LAN. Machines are named by bench label only.

The current result is the run below. The earlier FAIL run, at an older head
and before the send-buffer and runner fixes now on main, is kept further
down under "Superseded run" and no longer stands as the cell's result.

Date (`date -u`): Sun Oct 25 2026, cell traffic 10:55:02 to 11:05:02 UTC,
merger at 11:05:16 UTC.

Head: `813b4adbe3dec2eee496577e3da2c0d7d66b8063`, built by the script on
every host ("head 813b4adbe3dec2eee496577e3da2c0d7d66b8063 on every host";
each host's build record names that sha). Orchestrator digests (SHA-256):
`scripts/lan-proof`
`bd71c5aeaa9b8be307aa2f54492b69f1ed958047c3fac380cc529c3bc28a641e`,
`scripts/lan-proof-remote`
`e66a5f02f9ca92c8e374925a68da9a9fe1d51ac5d817b67e0758ae6793fbc880`.

## Result

The row as `scripts/lan-proof` printed it:

```text
msgx|macos-server-1|1000|1000|600|yes|0|0:pass,1:pass,2:pass,3:pass,4:pass,5:pass,6:pass,7:pass|small:pass,fragmented:pass|281|pass
```

| cell | server | speed | clients | duration | connected | timeouts | modes | sizes | min kbps | verdict |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| msgx | macos-server-1 | 1000 | 1000 | 600 | yes | 0 | 0-7 pass | small pass, fragmented pass | 281 | PASS |

Verdict: **PASS**. All 1000 clients stayed connected for the 600 s, the
server counted 0 disconnects, every reliability mode (0 to 7) and both size
classes passed the merger's delivery checks, and the minimum per-client rate
was 281 kbps against the 256 kbps target.

## Command

```text
scripts/lan-proof --inventory INVENTORY --cell msgx --duration 600 \
    --clients 1000 --commit 813b4adbe3dec2eee496577e3da2c0d7d66b8063 \
    --run-dir DIR --run-id 5a0500000000025a
```

Inventory rows (ssh names replaced by labels):

```text
macos-server-1  server  macos  default  -
linux-client-1  client  linux  eno1     -
linux-client-2  client  linux  eno1     -
```

## Hosts

From the script's host facts (`ifconfig` media on macOS, `ethtool` and
`lspci` on Linux):

| label | role | OS, kernel | test NIC model, driver | speed, duplex | logical cores |
| --- | --- | --- | --- | --- | --- |
| macos-server-1 | server | macOS 27.2 (build 26B118), Darwin 27.2.0, en0 | Ethernet (as `networksetup` names the port), driver unknown | 1000 Mb/s, full | 18 |
| linux-client-1 | client, shard0 + shard2 | Linux 7.0.0-38-generic, eno1 | Aquantia AQC113 NBase-T (rev 03), atlantic | 1000 Mb/s, full | 16 |
| linux-client-2 | client, shard1 + shard3 | Linux 7.0.0-38-generic, eno1 | Aquantia AQC113 NBase-T (rev 03), atlantic | 1000 Mb/s, full | 16 |

The server ran with the open-file limit raised by the remote script. The
script built the head on the server inside the macOS bench slot, and the
slot was held again from the end of the builds to the end of the cell, so
no other bench job started on the server machine during the cell. The
client machines' load was waited on before the run was started (both
under 6 of 16).

## Load samples

load1 / logical cores, retained by the script:

| sample | macos-server-1 | linux-client-1 | linux-client-2 |
| --- | --- | --- | --- |
| start | 4.76 / 18 | 2.62 / 16 | 3.58 / 16 |
| runtime 60 s | 4.98 / 18 | 1.57 / 16 | 1.82 / 16 |
| runtime 120 s | 5.86 / 18 | 1.84 / 16 | 0.83 / 16 |
| runtime 180 s | 7.50 / 18 | 1.04 / 16 | 1.27 / 16 |
| runtime 240 s | 4.53 / 18 | 1.25 / 16 | 1.88 / 16 |
| runtime 300 s | 3.64 / 18 | 1.56 / 16 | 1.21 / 16 |
| runtime 360 s | 2.61 / 18 | 1.25 / 16 | 1.00 / 16 |
| runtime 420 s | 3.32 / 18 | 1.06 / 16 | 0.54 / 16 |
| runtime 480 s | 4.04 / 18 | 1.53 / 16 | 0.84 / 16 |
| runtime 540 s | 5.07 / 18 | 1.38 / 16 | 0.91 / 16 |
| end | 4.09 / 18 | 1.35 / 16 | 0.66 / 16 |

Every start sample was under half its cores, the server stayed under its
core count at every runtime sample and the clients' end loads were under
their core counts, so the run is kept. No load bound was crossed, so no
rerun was made.

## Proof lines

Merger (`tools/check_split_load.py --load proof`):

```text
split_proof_cell: verdict=PASS transport=msgx total=1000 streams=26000 messages_sent=24000000 messages_delivered=23953736 unreliable_lost=25662 sequenced_skipped=20602 timing_verdict=FAIL
split_merge: schema=split_merge_v2 verdict=PASS overall_verdict=FAIL non_timing_verdict=PASS timing_verdict=FAIL reason=none load=proof run_id=5a0500000000025a total=1000 transport=msgx streams=26000 messages_sent=24000000 messages_delivered=23953736 unreliable_lost=25662 sequenced_skipped=20602 combined_client_up_kbps_upper=297567.06844 combined_client_down_kbps_upper=280557.0733733333
```

The proof verdict is the non-timing verdict (PASS). The timing verdict is
FAIL and the overall verdict follows it; call timings are not part of this
proof, and the earlier 600 s msgx cell that passed on quiet hosts reported
the same timing FAIL.

Server:

```text
split_proof: role=server run_id=5a0500000000025a transport=msgx transport_pin=msgx small_bytes=1200 fragmented_bytes=2000 target_kbps=256 min_client_up_kbps=281.377080 min_client_down_kbps=288.870387 checksum_errors=0 receipt_errors=0 receipts_dropped=0 disconnects=0 verdict=PASS
```

Shards (client transport gso on every shard), min per-client kbps up / down:

| shard | host | min up | min down | checksum errors | receipt errors | receipts dropped | disconnects | verdict |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| shard0 | linux-client-1 | 292.34 | 279.99 | 0 | 0 | 0 | 0 | PASS |
| shard1 | linux-client-2 | 288.90 | 278.27 | 0 | 0 | 0 | 0 | PASS |
| shard2 | linux-client-1 | 292.40 | 280.09 | 0 | 0 | 0 | 0 | PASS |
| shard3 | linux-client-2 | 291.64 | 280.03 | 0 | 0 | 0 | 0 | PASS |

Receipts: the server and every shard report `receipt_errors=0`,
`receipts_dropped=0` and `checksum_errors=0`.

## Send drop paths

The server's send handoff counts its drops by path and prints them when the
transport opens and closes:

```text
0.244710: info: server send handoff drops: no-free-bank 0, handoff-busy 0, stopping 0, invalid-packet 0 packets; socket send failures 0
129.421149: info: server send handoff dropped 310 packets (no-free-bank): first of its kind; later ones are counted and logged when the transport closes
607.897759: info: server send handoff drops: no-free-bank 300709, handoff-busy 0, stopping 0, invalid-packet 0 packets; socket send failures 0
```

`SEND_FAILED` for `client=all` is 300709, the no-free-bank count; the socket
itself refused no send. The first no-free-bank drop came at 129 s, inside the
window where the server machine's load1 rose to its run peak (5.86 at 120 s,
7.50 at 180 s; the cell's own load was 2.6 to 3.6 in the quiet samples). The
drops cost unreliable messages only: 25662 unreliable messages were counted
as lost and 20602 sequenced messages were skipped, both counted and not
failed by the proof; every reliable message was delivered exactly once and
in order where ordered. Recorded, not diagnosed further: the run does not
show which process raised the server machine's load.

Server timings (not part of the proof verdict): `server_update` p50 299 us,
p99 8835 us, max 15049 us; `server_send` p99 3485 us.

## Superseded run

The run below was made at head `1ca477339e150220f90e96bbe7c61be5b175eb51`,
before the send-buffer fix and the runner fix now on main. Its FAIL row is
**superseded** by the PASS run above and is kept for the record only.

Date (`date -u`): Sun Oct 25 2026, cell traffic 06:23:09 to 06:33:09 UTC.

Head: `1ca477339e150220f90e96bbe7c61be5b175eb51`, built by the script on
every host ("head 1ca477339e150220f90e96bbe7c61be5b175eb51 on every host").
Orchestrator digests (SHA-256): `scripts/lan-proof`
`b890f2c30e39df622e7c2d9674dd8bab79452bbd0b7c40147c261eb593f17b39`,
`scripts/lan-proof-remote`
`5322fdbe1d01dcd675ea1445d2503e64c4891d0338918c1a2d0dd2975fc560f2`.

### Result

| cell | server | speed | clients | duration | connected | timeouts | modes | sizes | min kbps | verdict |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| msgx | macos-server-1 | 1000 | 1000 | 600 | no | 250 | unknown | unknown | 1 | FAIL (superseded) |

`scripts/lan-proof` printed no row of its own for this cell: the merger
refused the evidence (`barrier_complete: expected 1, got 0`), its
`split_proof_cell` line carries no `streams=` field, and `--cell-row` stopped
with `cell-row: a proof field is missing`. The row above is written by hand
from the server and shard proof lines below; the per-mode and per-size
verdicts are `unknown` because the merger did not validate the streams, as
LAN-PROOF.md specifies for a failed aggregate check. "timeouts" is the
server's `disconnects=` count.

Verdict: **FAIL**, superseded by the PASS run above.

### Command

```text
scripts/lan-proof --inventory INVENTORY --cell msgx --duration 600 \
    --clients 1000 --commit 1ca477339e150220f90e96bbe7c61be5b175eb51 \
    --run-dir DIR --run-id ac0200006add9f39
```

Inventory rows (ssh names replaced by labels):

```text
macos-server-1  server  macos  default  -
linux-client-1  client  linux  eno1     -
linux-client-2  client  linux  eno1     -
```

### Hosts

From the script's host facts (`ifconfig` media on macOS, `ethtool` and
`lspci` on Linux):

| label | role | OS, kernel | test NIC model, driver | speed, duplex | logical cores |
| --- | --- | --- | --- | --- | --- |
| macos-server-1 | server | macOS 27.2 (build 26B118), Darwin 27.2.0, en0 | Ethernet (as `networksetup` names the port), driver unknown | 1000 Mb/s, full | 18 |
| linux-client-1 | client, shard0 + shard2 | Linux 7.0.0-38-generic, eno1 | Aquantia AQC113 NBase-T (rev 03), atlantic | 1000 Mb/s, full | 16 |
| linux-client-2 | client, shard1 + shard3 | Linux 7.0.0-38-generic, eno1 | Aquantia AQC113 NBase-T (rev 03), atlantic | 1000 Mb/s, full | 16 |

The server ran with the open-file limit raised by the remote script
(`ulimit -n 65535`; the machine's launchd maxfiles is 65536 soft, unlimited
hard, `kern.maxfilesperproc` 245760), and the macOS bench slot was held from
06:23:08 to 06:33:40 UTC so no other bench job shared the machine during the
cell.

### Load samples

load1 / logical cores, retained by the script:

| sample | macos-server-1 | linux-client-1 | linux-client-2 |
| --- | --- | --- | --- |
| start | 1.58 / 18 | 0.21 / 16 | 0.02 / 16 |
| runtime 300 s | 3.28 / 18 | 1.14 / 16 | 1.21 / 16 |
| end | 2.04 / 18 | 0.56 / 16 | 1.45 / 16 |

Every start sample was under half its cores, the server stayed under its
core count at every runtime sample (60 s to 540 s) and the clients' end loads
were under their core counts, so the run is kept: the failure is not a load
discard.

### Proof lines

Server:

```text
split_proof: role=server run_id=ac0200006add9f39 transport=msgx transport_pin=msgx small_bytes=1200 fragmented_bytes=2000 target_kbps=256 min_client_up_kbps=278.511600 min_client_down_kbps=286.965760 checksum_errors=0 receipt_errors=132 receipts_dropped=0 disconnects=250 verdict=FAIL
```

Merger (`tools/check_split_load.py --load proof`), the whole of `merge.log`:

```text
split_merge: verdict=FAIL reason=barrier_complete: expected 1, got 0
split_merge_json: {"non_timing_verdict": "FAIL", "overall_verdict": "FAIL", "reason": "invalid_evidence", "schema": "split_merge_v2", "timing_verdict": "UNKNOWN", "verdict": "FAIL"}
split_proof_cell: verdict=FAIL reason=barrier_complete:_expected_1,_got_0
```

Shards, min per-client kbps up / down and errors:

| shard | host | min up | min down | checksum errors | receipt errors | receipts dropped | disconnects | verdict |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| shard0 | linux-client-1 | 6.91 | 1.10 | 0 | 1 | 0 | 2 | FAIL |
| shard1 | linux-client-2 | 286.99 | 257.00 | 0 | 0 | 0 | 0 | FAIL |
| shard2 | linux-client-1 | 382.61 | 256.79 | 0 | 0 | 0 | 0 | FAIL |
| shard3 | linux-client-2 | 373.52 | 256.82 | 0 | 0 | 0 | 0 | FAIL |

Every shard's receipt has `barrier_complete=0`; that is the reason the
merger names.

Receipts: the server and every shard report `receipts_dropped=0` and
`checksum_errors=0`. The server reports `receipt_errors=132` and shard0
reports `receipt_errors=1`. The server receipt has `identity_errors=0
payload_errors=0 order_errors=0 duplicate_errors=0 protocol_errors=0`,
`load_complete=0`, `wire_valid=0` and `delivery_verdict=FAIL`.

### What the logs show (recorded, not diagnosed)

- Two shard0 clients went from connected to disconnected at 13.09 s and
  13.18 s into the shard's run. The server log shows the matching ends:
  `14.242567: info: message connection 88 ended: reason -3` and
  `14.347274: info: message connection 18 ended: reason -3`. That shard's
  minimum per-client rate (6.91 / 1.10 kbps) comes from those clients.
- Server counters for the whole run (`client=all`): `SEND_FAILED=5128615`,
  `CLIENT_CONNECTED=1000`, `CLIENT_DISCONNECTED=250`,
  `SERVER_DISCONNECTED_CLIENT=2`, `DISCONNECT_RECEIVED=2480`,
  `DISCONNECT_DISCONNECTED=248`, `MESSAGE_RECEIPT_LOSS=170975`,
  `MESSAGE_DUPLICATE=645`, `MESSAGE_UNRELIABLE_IN_PLACE_FALLBACK=2189550`.
- shard0's last client disconnects are at 604.04 s; shards 1 to 3 end at
  about 618.9 s.
- Server call timings (not part of the proof verdict): `server_update` p50
  796 us, p99 7763 us, max 14911 us; `server_send_packets` p50 720 us, p99
  3699 us, max 4748 us; `server_receive_messages` p99 17 us.

Server log tail:

```text
619.957942: info: message connection 998 ended: reason 0
619.957961: info: message connection 999 ended: reason 0
split_server_timing: run_id=ac0200006add9f39 scope=traffic_plus_drain server_update_count=60300 server_update_p50_upper_us=796 server_update_p99_upper_us=7763 server_update_max_us=14911.000 server_send_count=60300 server_send_p99_upper_us=3699 clock_pair_count=10000
split_proof: role=server run_id=ac0200006add9f39 transport=msgx transport_pin=msgx small_bytes=1200 fragmented_bytes=2000 target_kbps=256 min_client_up_kbps=278.511600 min_client_down_kbps=286.965760 checksum_errors=0 receipt_errors=132 receipts_dropped=0 disconnects=250 verdict=FAIL
```

shard0 log tail:

```text
604.036493: info: client changed state from 'connected' to 'disconnected'
604.036511: info: message connection 0 ended: reason 0
split_proof: role=client-shard run_id=ac0200006add9f39 transport=portable transport_pin=none small_bytes=1200 fragmented_bytes=2000 target_kbps=256 min_client_up_kbps=6.912747 min_client_down_kbps=1.096440 checksum_errors=0 receipt_errors=1 receipts_dropped=0 disconnects=2 verdict=FAIL
```

### Attempts

1. Run id `ac0100006add9510`, 05:35 to 05:49 UTC: not made. Both client hosts
   built the head. The server build hit the script's 870-second build bound
   while it waited for the busy macOS bench slot
   (`lan-proof-remote: build: checkout, build, or 870-second bound failed`).
   No server, shard or traffic was started. The head was then prebuilt on
   the server under the bench slot with no bound.
2. Run id `ac0200006add9f39`, cell traffic 06:23 to 06:33 UTC: made, FAIL as
   recorded above. This was the one retry the card allows. It is recorded
   as it ran, and was not rerun.

### Diagnosis of the failed cell (measured after the run, no new run made)

Measured on 2026-10-25 from the run's own logs, the source at the head above
and read-only counters on the machines. No system setting was changed and
the cell was not rerun.

Send failures. `SEND_FAILED` is the server socket's `send_failures`
(`sdk/rocketnet_platform.cpp`): one per refused `sendmsg_x` chunk of up to
32 datagrams, plus one per refused per-packet send; the chunk's per-packet
replay refusals go to `send_replay_failures`, which no counter prints. The
errno is written only by `rocketnet_debug`, and the run logs at info, so
**the run does not record the errno**. What is recorded bounds it: an errno
outside the weather set (`EINTR EAGAIN EWOULDBLOCK ENOBUFS ECONNREFUSED
EHOSTUNREACH ENETUNREACH EMSGSIZE EADDRNOTAVAIL EPERM EACCES EHOSTDOWN
ENETDOWN`) pauses the msgx pump with a `warn` line, and the server log has
no such line, so every refusal carried a weather errno. On the server
machine, after the run: the test NIC's output queue (FQ_CODEL, limit 3888)
shows 1 dropped packet since boot and no flow control; IP output shows 0
packets dropped for lack of buffers. A full interface queue (`ENOBUFS`) is
therefore not what refused the sends. The server asks for a fixed
`SO_SNDBUF` of 1,000,000 bytes (`ROCKETNET_SOCKET_SEND_BUFFER_SIZE`), not
sized by client count; a full send buffer (`EAGAIN`) is the remaining
candidate and is not measured.

Socket limits on the server machine (`sysctl`):

| name | value |
| --- | --- |
| kern.ipc.maxsockbuf | 8388608 (8 MiB) |
| net.inet.udp.maxdgram | 9216 |
| net.inet.udp.recvspace | 786896 |
| net.link.generic.system.sndq_maxlen | 128 |

The server asked for a 25,000,000-byte receive buffer and was granted
8,388,608 (its own warn line: "raise kern.ipc.maxsockbuf"). The send buffer
request (1,000,000) is under the limit.

The two disconnects. Reason -3 is `ROCKETNET_DISCONNECT_REASON_RELIABLE_TIMED_OUT`
(`sdk/rocketnet.h`): a reliable message went unacknowledged past
reliable_message_timeout_ms. It was the server's messages to the client that
timed out. The server's captures at the kick: about 4,870 packets sent to
each client in 12.3 s, 261 reliable messages unacknowledged, the oldest
9.8 s old, 260 queued for resend. The clients' captures: 228 and 239 packets
received against a highest received sequence of 4,180 and 4,215, so about
95% of the server's packets to those two clients never arrived, while the
server received every packet the clients sent. The client machine was not
the cause on its own: its load1 was 0.21 at start and 1.15 at 60 s of 16
cores, and the other shard on the same machine kept every client at 256.79
kbps or more. The receive-side drop counters on the client machine are
cumulative over days of runs and cannot be attributed to this window.

Not established: the errno, and whether the lost packets were refused by
the server's kernel or dropped at the client sockets. Settling it needs a
run that records the send errno (a library change) or the per-socket drop
counts over the run window.
