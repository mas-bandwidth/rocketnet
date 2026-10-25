# LAN 1000-client proof: macOS msgx server

Card rn-lan-1000-proof-macos. One cell, msgx, run through `scripts/lan-proof`
as [LAN-PROOF.md](../LAN-PROOF.md) describes: 1000 clients in four shards of
250 on two Linux client machines, the server alone on the macOS machine,
traffic across the home LAN. Machines are named by bench label only.

Date (`date -u`): Sun Oct 25 2026, cell traffic 06:23:09 to 06:33:09 UTC.

Head: `1ca477339e150220f90e96bbe7c61be5b175eb51`, built by the script on
every host ("head 1ca477339e150220f90e96bbe7c61be5b175eb51 on every host").
Orchestrator digests (SHA-256): `scripts/lan-proof`
`b890f2c30e39df622e7c2d9674dd8bab79452bbd0b7c40147c261eb593f17b39`,
`scripts/lan-proof-remote`
`5322fdbe1d01dcd675ea1445d2503e64c4891d0338918c1a2d0dd2975fc560f2`.

## Result

| cell | server | speed | clients | duration | connected | timeouts | modes | sizes | min kbps | verdict |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| msgx | macos-server-1 | 1000 | 1000 | 600 | no | 250 | unknown | unknown | 1 | FAIL |

`scripts/lan-proof` printed no row of its own for this cell: the merger
refused the evidence (`barrier_complete: expected 1, got 0`), its
`split_proof_cell` line carries no `streams=` field, and `--cell-row` stopped
with `cell-row: a proof field is missing`. The row above is written by hand
from the server and shard proof lines below; the per-mode and per-size
verdicts are `unknown` because the merger did not validate the streams, as
LAN-PROOF.md specifies for a failed aggregate check. "timeouts" is the
server's `disconnects=` count.

Verdict: **FAIL**. The cell is HOLD.

## Command

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

## Hosts

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

## Load samples

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

## Proof lines

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

## What the logs show (recorded, not diagnosed)

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

## Attempts

1. Run id `ac0100006add9510`, 05:35 to 05:49 UTC: not made. Both client hosts
   built the head. The server build hit the script's 870-second build bound
   while it waited for the busy macOS bench slot
   (`lan-proof-remote: build: checkout, build, or 870-second bound failed`).
   No server, shard or traffic was started. The head was then prebuilt on
   the server under the bench slot with no bound.
2. Run id `ac0200006add9f39`, cell traffic 06:23 to 06:33 UTC: made, FAIL as
   recorded above. This was the one retry the card allows. It is recorded
   as it ran, and was not rerun.
