# LAN 1000-client proof: results

Each row is one cell run through `scripts/lan-proof` as
[LAN-PROOF.md](../LAN-PROOF.md) describes: 1000 clients in shards of at most
250 on client machines, the server alone on its own machine, traffic across
the LAN. Machines are named by inventory label only. Rows are appended as
cells complete.

| cell | server | speed | clients | duration | connected | timeouts | modes | sizes | min kbps | verdict |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| gso | server-a | 1000 | 1000 | 120 | yes | 0 | 0:pass,1:pass,2:pass,3:pass,4:pass,5:pass,6:pass,7:pass | small:pass,fragmented:pass | 277 | pass |

Cells not yet recorded: io_uring, xdp (owed: the dedicated test port has no
carrier yet), iocp, rio, msgx.

## gso, run a1000c0062026101

Executable head `59c266ff1fbcc3e3edca82998cf7f54697b6e7c2`, built by the
script on every host. Later commits on the branch change only this file and
the procedure.

Command:

```text
scripts/lan-proof --inventory INVENTORY --cell gso --duration 120 \
    --clients 1000 --commit 59c266ff1fbcc3e3edca82998cf7f54697b6e7c2 \
    --run-dir DIR --run-id a1000c0062026101
```

Hosts (from the script's host facts):

| label | role | OS, kernel | test NIC model, driver | speed, duplex | logical cores |
| --- | --- | --- | --- | --- | --- |
| server-a | server | Linux 7.0.0-34-generic | X540-AT2 10-Gigabit, ixgbe | 1000 Mb/s, full | 64 |
| client-a | client, shard0 + shard2 | Linux 7.0.0-38-generic | AQC113 NBase-T, atlantic | 1000 Mb/s, full | 16 |
| client-b | client, shard1 + shard3 | Linux 7.0.0-38-generic | AQC113 NBase-T, atlantic | 1000 Mb/s, full | 16 |

The negotiated link speed on every host was 1000 Mb/s, so this cell proves
1000 clients over a gigabit path, not a 10-gigabit one.

Load samples (load1 / logical cores), retained by the script:

| sample | server-a | client-a | client-b |
| --- | --- | --- | --- |
| start | 1.13 / 64 | 1.03 / 16 | 1.38 / 16 |
| runtime 60 s | 1.48 / 64 | 0.78 / 16 | 2.86 / 16 |
| runtime 120 s | 1.27 / 64 | 2.50 / 16 | 1.92 / 16 |
| end | 1.24 / 64 | 2.38 / 16 | 1.63 / 16 |

Admission was re-measured every five minutes before the start; the run began
only when every host's load was under half its cores and no continuous
integration job was running on a client host. No sample came near a core
count, so the run is kept.

Server proof line:

```text
split_proof: role=server transport=gso transport_pin=gso small_bytes=1200 fragmented_bytes=2000 target_kbps=256 min_client_up_kbps=277.679400 min_client_down_kbps=278.976133 checksum_errors=0 receipt_errors=0 receipts_dropped=0 disconnects=0 verdict=PASS
```

Merger (`tools/check_split_load.py --load proof`):

```text
split_proof_cell: verdict=PASS transport=gso total=1000 streams=26000 messages_sent=4800000 messages_delivered=4800000 unreliable_lost=0 sequenced_skipped=0 timing_verdict=FAIL
```

Shard proof lines, minimum per-client kbps up / down, all `verdict=PASS`
with zero checksum, receipt and disconnect errors: shard0 280.85 / 277.95,
shard1 279.11 / 277.83, shard2 279.94 / 277.73, shard3 280.12 / 277.88.

Server call timings (not part of the proof verdict; performance is judged
as parity in framework, not as a per-call budget):

| call | count | p50 us | p99 us | max us |
| --- | --- | --- | --- | --- |
| server_update | 12300 | 1258 | 8140 | 12757.954 |
| server_receive_messages | 16886 | 8 | 49 | 311.728 |
| server_start_message | 2400000 | 1 | 2 | 754.964 |
| server_finish_message | 2400000 | 1 | 1 | 120.096 |
| server_send_packets | 12300 | 149 | 7344 | 29155.903 |

Allocation manifest (`allocation.json`, schema `split_allocation_v1`):
server `server-a` holds total 1000 as `dedicated_server`; shard0 (first 0)
and shard2 (first 500) on `client-a`, shard1 (first 250) and shard3
(first 750) on `client-b`, 250 clients each, as `client_host`.

Retained files and SHA-256 digests (the logs hold the run's connect key, so
they are kept outside the repository):

```text
e80fcf6727a5ba056274124918dd45a37d4f9928a94b2ba0d3308baee9b95d94  server.log
88ec55f60b569b5f6dae30e18fedba6c7974ee6fa5c94c50a9a6fd807beb4879  shard0.log
bdae68596a0930db5a82aa0bdfb883926558dfb3ee099a580fbecde0222fa8d6  shard1.log
5bc51af2e8344457c4c8242eead91cf211ff99411322caf51a85c1e4da0f621e  shard2.log
3db6402f43451e0fefb5aeea388850e301b051547b3b4a3d49f5d7add35b30b3  shard3.log
048dc49f013cb04721c597172a5889753d4ca8d88392dc6fce4a8d7f68da8a7e  allocation.json
8a4e04d9094658efc6838dd33773cbbf3876afbe47cd240fa15221029d89dbf5  merge.log
91fb54a5b0a291fd019f41f6c5a850c3dea74dbd048939502ec3b3dab7d4e387  row.txt
```

Every server and shard process the run started was stopped by the script's
teardown; none remained on any host afterwards.

## Discarded runs

An earlier 1000-client gso run (head `c2e5e634`, run a100c00120261006) was
discarded for load: one client host's end load was 28.34 on 16 cores from
other work while the other client host, running the same shards, stayed
under 6. Its delivery lines passed, but a run with foreign load is not
evidence.
