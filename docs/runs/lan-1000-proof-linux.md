# LAN 1000-client proof: Linux server, gso and io_uring

Recorded 2026-10-25 (UTC; `date -u` at the end of the runs read
`Sun Oct 25 05:34:39 UTC 2026`). Both cells were run through `scripts/lan-proof`
as [LAN-PROOF.md](../LAN-PROOF.md) describes. Each had 1000 clients in four
shards of 250 on two client machines and the server alone on its own machine,
with all traffic crossing the LAN. Machines are named by bench label only.

Executable head `1ca477339e150220f90e96bbe7c61be5b175eb51`. The script built it
on every host, and every host's last build line was that sha. The orchestrator
at that head has SHA-256
`b890f2c30e39df622e7c2d9674dd8bab79452bbd0b7c40147c261eb593f17b39`
(`scripts/lan-proof`) and
`5322fdbe1d01dcd675ea1445d2503e64c4891d0338918c1a2d0dd2975fc560f2`
(`scripts/lan-proof-remote`).

## Cells

| cell | server | speed | clients | duration | connected | timeouts | modes | sizes | min kbps | verdict |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| gso | linux-server-1 | 1000 | 1000 | 600 | yes | 0 | 0:pass,1:pass,2:pass,3:pass,4:pass,5:pass,6:pass,7:pass | small:pass,fragmented:pass | 278 | pass |
| io_uring | linux-server-1 | 1000 | 1000 | 600 | yes | 0 | 0:pass,1:pass,2:pass,3:pass,4:pass,5:pass,6:pass,7:pass | small:pass,fragmented:pass | 276 | pass |

Verdict: both cells PASS.

Commands, one per cell, run one at a time:

```text
scripts/lan-proof --inventory INVENTORY --cell gso --duration 600 \
    --clients 1000 --commit 1ca477339e150220f90e96bbe7c61be5b175eb51 \
    --run-dir DIR --run-id a1000c1025000002
scripts/lan-proof --inventory INVENTORY --cell io_uring --duration 600 \
    --clients 1000 --commit 1ca477339e150220f90e96bbe7c61be5b175eb51 \
    --run-dir DIR --run-id a1000c1025000003
```

The inventory has one server row (`linux-server-1`, interface `enp9s0f0`) and two
client rows (`linux-client-1` and `linux-client-2`, interface `eno1`). Each of
these interfaces is its host's uplink.

## Hosts

From the script's host facts (`ethtool`, `ethtool -i`, `lspci`, `uname -r`):

| label | role | OS, kernel | test NIC model, driver | speed, duplex | logical cores |
| --- | --- | --- | --- | --- | --- |
| linux-server-1 | server | Linux 7.0.0-34-generic | Intel X540-AT2 10-Gigabit (rev 01), ixgbe | 1000 Mb/s, full | 64 |
| linux-client-1 | client, shard0 + shard2 | Linux 7.0.0-38-generic | Aquantia AQC113 NBase-T (rev 03), atlantic | 1000 Mb/s, full | 16 |
| linux-client-2 | client, shard1 + shard3 | Linux 7.0.0-38-generic | Aquantia AQC113 NBase-T (rev 03), atlantic | 1000 Mb/s, full | 16 |

Every host negotiated 1000 Mb/s. These cells therefore prove 1000 clients over a
gigabit path, not a 10-gigabit one.

## Load (load1 / logical cores)

Admission: a cell started only after every host's foreign load1 was under half
its cores. The script refuses to start otherwise. While each cell ran, the
server stayed far below its 64 cores, and every client's end load stayed below
its 16 cores. Both runs are kept.

| sample | gso server-1 | gso client-1 | gso client-2 | io_uring server-1 | io_uring client-1 | io_uring client-2 |
| --- | --- | --- | --- | --- | --- | --- |
| start | 1.95 / 64 | 0.09 / 16 | 3.62 / 16 | 1.64 / 64 | 0.89 / 16 | 1.90 / 16 |
| 60 s | 1.87 | 0.82 | 3.02 | 1.16 | 1.18 | 1.47 |
| 120 s | 1.94 | 0.77 | 1.57 | 1.14 | 0.50 | 0.88 |
| 180 s | 2.00 | 0.78 | 1.59 | 1.17 | 0.88 | 2.75 |
| 240 s | 1.21 | 1.33 | 2.10 | 1.41 | 0.76 | 2.65 |
| 300 s | 1.09 | 1.48 | 1.39 | 1.42 | 1.84 | 2.58 |
| 360 s | 1.44 | 3.87 | 1.36 | 1.53 | 2.35 | 1.96 |
| 420 s | 2.44 | 2.21 | 3.14 | 1.99 | 2.17 | 1.65 |
| 480 s | 2.43 | 2.16 | 4.00 | 1.57 | 1.51 | 2.46 |
| 540 s | 1.94 | 1.67 | 3.09 | 1.05 | 1.49 | 3.71 |
| end | 2.01 / 64 | 1.14 / 16 | 2.15 / 16 | 1.39 / 64 | 0.78 / 16 | 2.35 / 16 |

## gso, run a1000c1025000002

Server proof line:

```text
split_proof: role=server run_id=a1000c1025000002 transport=gso transport_pin=gso small_bytes=1200 fragmented_bytes=2000 target_kbps=256 min_client_up_kbps=278.233400 min_client_down_kbps=278.662427 checksum_errors=0 receipt_errors=0 receipts_dropped=0 disconnects=0 verdict=PASS
```

Merger verdict line (`tools/check_split_load.py --load proof`):

```text
split_proof_cell: verdict=PASS transport=gso total=1000 streams=26000 messages_sent=24000000 messages_delivered=24000000 unreliable_lost=0 sequenced_skipped=0 timing_verdict=FAIL
```

Shard proof lines: every one is `verdict=PASS` with 0 checksum errors, 0 receipt
errors, 0 receipts dropped and 0 disconnects. Minimum per-client kbps, up / down:
shard0 279.00 / 278.50, shard1 278.74 / 278.21, shard2 278.69 / 278.43,
shard3 278.82 / 278.77.

Server receipt: connected=1000, 0 identity, payload, order or duplicate errors,
0 unexpected disconnects, 0 offer refusals, 0 protocol errors,
delivery_verdict=PASS, load_verdict=PASS, bandwidth_verdict=PASS. Traffic ran
for 600.000086 s.

## io_uring, run a1000c1025000003

Server proof line:

```text
split_proof: role=server run_id=a1000c1025000003 transport=io_uring transport_pin=io_uring small_bytes=1200 fragmented_bytes=2000 target_kbps=256 min_client_up_kbps=276.740280 min_client_down_kbps=283.739373 checksum_errors=0 receipt_errors=0 receipts_dropped=0 disconnects=0 verdict=PASS
```

Merger verdict line:

```text
split_proof_cell: verdict=PASS transport=io_uring total=1000 streams=26000 messages_sent=24000000 messages_delivered=23887088 unreliable_lost=110680 sequenced_skipped=2232 timing_verdict=FAIL
```

Shard proof lines: every one is `verdict=PASS` with 0 checksum errors, 0 receipt
errors, 0 receipts dropped and 0 disconnects. Minimum per-client kbps, up / down,
by shard: 284.06 / 273.03, 285.26 / 267.28, 283.76 / 274.21, 284.18 / 269.30.

Server receipt: delivery_verdict=PASS, load_verdict=PASS, bandwidth_verdict=PASS.

In this cell the unreliable streams lost 110680 messages and the sequenced
streams skipped 2232. The owner's checks count loss on unreliable streams
without failing it, and a sequenced skip means an older message was not
delivered. Every reliable stream was delivered exactly once and in order, and
the receipts matched. The gso cell, on the same hosts and the same head, lost
nothing. This difference is recorded here and was not investigated.

## Server call timings (not part of the proof verdict)

The merger's `timing_verdict=FAIL` comes from the 100 us per-call budget. That
budget is not the proof's bar: performance is judged as parity in framework,
not as a per-call budget. The timings below are for the record only.

| cell | call | count | p50 us | p99 us | max us |
| --- | --- | --- | --- | --- | --- |
| gso | server_update | 60300 | 1157 | 7818 | 12975.063 |
| gso | server_receive_messages | 84489 | 8 | 36 | 1229.100 |
| gso | server_start_message | 12000000 | 1 | 1 | 120.126 |
| gso | server_finish_message | 12000000 | 1 | 1 | 6259.513 |
| gso | server_send_packets | 60300 | 104 | 6999 | 17049.715 |
| io_uring | server_update | 60300 | 448 | 14711 | 26797.984 |
| io_uring | server_receive_messages | 78995 | 5 | 68 | 2390.898 |
| io_uring | server_start_message | 12000000 | 1 | 2 | 3779.546 |
| io_uring | server_finish_message | 12000000 | 1 | 1 | 4630.580 |
| io_uring | server_send_packets | 60300 | 139 | 22014 | 51544.773 |

## Allocation and retained files

Allocation manifest (`allocation.json`, schema `split_allocation_v1`) for each
cell: `linux-server-1` holds all 1000 clients as `dedicated_server`. On
`linux-client-1`, shard0 (first 0) and shard2 (first 500). On `linux-client-2`,
shard1 (first 250) and shard3 (first 750). Each shard has 250 clients and the
type `client_host`.

The logs hold the run's connect key, so they are kept outside the repository.
SHA-256 digests:

```text
gso (a1000c1025000002)
12a970ad24281f73eb1ff2b3e7f261bc81c67e0a89326c22dd225eb9192420c1  server.log
10da95e3c6af986521657801a0749cb4ee9ef0ac5e37aee0993c7d63910f72fb  shard0.log
7e2805276c7f250c57bd7fc239112c0c006a10b52b4dbe59b645861bae8b063e  shard1.log
06757be90c87c3cf037113f35794e6c92209f71506534541c299ec979b68f75f  shard2.log
c3b9fbf407081c8d4c923cf384dda5a18586bd57e5ecb06aad072a9b4a0e773f  shard3.log
02532953999a0e5b7b6ab991c781b6e55ca90864bce075187ff9c6b55179ecd1  allocation.json
14bc0c4a131987069cc890cf835285e530b15551afcd1f089a02402355212c0e  merge.log
ad1394034e791e7a963c502f79a0966703f5cbcdd55967751d6ff73bcd78a096  row.txt

io_uring (a1000c1025000003)
27ad9e59ef250284db5dd8717f4b1e182eda321cc1036cb0e420242dc3c84a5a  server.log
6fada98abde64e096f725671fc6157fa18e2c7fc98959f19f2fa417285990217  shard0.log
51b8443ecf8b304eee2dccd43ed5e55e95821f791b42c76340e9ce2648e39f7b  shard1.log
6f64dc6570ccaaa79af25b3b04ec98d193078ff1933116af6c83e9c11a0a2e61  shard2.log
f49e7e839145372495c8b3b170ab939b4654046cf83243816fef5a0792f10372  shard3.log
102066c824d9d9cb1632032b30a0cce2dcd0ff02d57688a2103637f66ab7e0f4  allocation.json
6bbda08cf03545a902d340d57db972fc34bc52c8f0bba5188ad45d51a0c31880  merge.log
9aa60f4fe6861a640b4a5ef805226af710a6b7f14988b40757b4d67ffeb2600a  row.txt
```

After each cell, the script's teardown stopped every server and shard process
the run had started. A check afterwards found no `load_split` process on any of
the three hosts.

## Refused invocation

An earlier invocation (run id a1000c1025000001) passed the abbreviated sha
`1ca477339` as `--commit`. The remote build refused it ("checkout is not
1ca477339") before any host facts were read or any process was started. No cell
ran, so it is not a cell attempt and it is not a retry. Neither cell needed a
retry.
