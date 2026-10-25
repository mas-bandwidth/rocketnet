# LAN 1000-client proof: results

One row per cell of the LAN proof: 1000 clients for 600 seconds against one
server pinned to one transport, the server alone on its own machine, the
clients in shards of at most 250 on other machines, all traffic across the
LAN. Each cell is run through `scripts/lan-proof` as
[LAN-PROOF.md](../LAN-PROOF.md) describes. Machines are named by bench label
only.

Every value below is copied from the per-platform record named in the
row's last column; that record holds the commands, host facts, load
samples, proof lines, allocation manifest and log digests. A cell with no
record is NOT MADE, with its reason, and carries no values.

## Results

Reliability modes are numbered as `enum rocketnet_reliability_t` in
`sdk/rocketnet.h`: 0 UNRELIABLE, 1 UNRELIABLE_SEQUENCED, 2 RELIABLE,
3 RELIABLE_ORDERED, 4 RELIABLE_SEQUENCED, 5 UNRELIABLE_WITH_ACK_RECEIPT,
6 RELIABLE_WITH_ACK_RECEIPT, 7 RELIABLE_ORDERED_WITH_ACK_RECEIPT. The
reliable exactly-once, ordered, sequenced and unreliable columns give the
merger's per-mode verdicts for those modes and its counts. "Timeouts" is the
server's disconnect count. "Receipts" and "payload" are the server's
`receipt_errors`, `receipts_dropped` and `checksum_errors`.

### Made

| platform | server transport | server label | server NIC speed, duplex | client hosts, NIC speed | head | clients | seconds | connected throughout | timeouts | reliable exactly-once (2, 3, 6, 7) | ordered (3, 7) | sequenced (1, 4) | unreliable (0, 1, 5) | receipts | payload | min per-client kbps | sizes | verdict | record |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Linux | gso | linux-server-1 | 1000 Mb/s, full | linux-client-1 1000 Mb/s full; linux-client-2 1000 Mb/s full | `1ca477339e150220f90e96bbe7c61be5b175eb51` | 1000 | 600 | yes | 0 | 2:pass, 3:pass, 6:pass, 7:pass | 3:pass, 7:pass | 1:pass, 4:pass; sequenced_skipped=0 | 0:pass, 1:pass, 5:pass; unreliable_lost=0 | receipt_errors=0, receipts_dropped=0 | checksum_errors=0 | 278 | small:pass, fragmented:pass | pass | [lan-1000-proof-linux.md](lan-1000-proof-linux.md), run a1000c1025000002 |
| Linux | io_uring | linux-server-1 | 1000 Mb/s, full | linux-client-1 1000 Mb/s full; linux-client-2 1000 Mb/s full | `1ca477339e150220f90e96bbe7c61be5b175eb51` | 1000 | 600 | yes | 0 | 2:pass, 3:pass, 6:pass, 7:pass | 3:pass, 7:pass | 1:pass, 4:pass; sequenced_skipped=2232 | 0:pass, 1:pass, 5:pass; unreliable_lost=110680 | receipt_errors=0, receipts_dropped=0 | checksum_errors=0 | 276 | small:pass, fragmented:pass | pass | [lan-1000-proof-linux.md](lan-1000-proof-linux.md), run a1000c1025000003 |
| macOS | msgx | macos-server-1 | 1000 Mb/s, full | linux-client-1 1000 Mb/s full; linux-client-2 1000 Mb/s full | `813b4adbe3dec2eee496577e3da2c0d7d66b8063` | 1000 | 600 | yes | 0 | 2:pass, 3:pass, 6:pass, 7:pass | 3:pass, 7:pass | 1:pass, 4:pass; sequenced_skipped=20602 | 0:pass, 1:pass, 5:pass; unreliable_lost=25662 | receipt_errors=0, receipts_dropped=0 | checksum_errors=0 | 281 | small:pass, fragmented:pass | pass | [lan-1000-proof-macos.md](lan-1000-proof-macos.md), run 5a0500000000025a |

The same rows in the eleven-field result-row form of
[LAN-PROOF.md](../LAN-PROOF.md), with the values of the records' cell
tables:

```text
gso|linux-server-1|1000|1000|600|yes|0|0:pass,1:pass,2:pass,3:pass,4:pass,5:pass,6:pass,7:pass|small:pass,fragmented:pass|278|pass
io_uring|linux-server-1|1000|1000|600|yes|0|0:pass,1:pass,2:pass,3:pass,4:pass,5:pass,6:pass,7:pass|small:pass,fragmented:pass|276|pass
msgx|macos-server-1|1000|1000|600|yes|0|0:pass,1:pass,2:pass,3:pass,4:pass,5:pass,6:pass,7:pass|small:pass,fragmented:pass|281|pass
```

Merger verdict lines (`tools/check_split_load.py --load proof`), from the
records:

```text
split_proof_cell: verdict=PASS transport=gso total=1000 streams=26000 messages_sent=24000000 messages_delivered=24000000 unreliable_lost=0 sequenced_skipped=0 timing_verdict=FAIL
split_proof_cell: verdict=PASS transport=io_uring total=1000 streams=26000 messages_sent=24000000 messages_delivered=23887088 unreliable_lost=110680 sequenced_skipped=2232 timing_verdict=FAIL
split_proof_cell: verdict=PASS transport=msgx total=1000 streams=26000 messages_sent=24000000 messages_delivered=23953736 unreliable_lost=25662 sequenced_skipped=20602 timing_verdict=FAIL
```

Skips on RELIABLE_SEQUENCED and losses on the unreliable modes are counted
and never failed, as the "Proof load" rule in
[LOAD_SPLIT.md](../LOAD_SPLIT.md) states. `timing_verdict=FAIL` is the
per-call timing budget, which is not part of the proof verdict: performance
is judged as parity in framework. Each record gives the reason for its
counts as far as it was measured.

### Not made

| platform | server transport | status | reason | record it would come from |
| --- | --- | --- | --- | --- |
| Linux | xdp | NOT MADE | the xdp server needs a dedicated, cabled test port bridged to the LAN switch (see [LAN-PROOF.md](../LAN-PROOF.md)); the server machine has no cabled test NIC yet | docs/runs/lan-1000-proof-xdp.md (not yet written) |
| Windows | iocp | NOT MADE | the Windows server machine has no inbound firewall rule for the proof's UDP port 40000 yet, so the clients cannot reach the server | docs/runs/lan-1000-proof-windows.md (not yet written) |
| Windows | rio | NOT MADE | the same firewall rule on the Windows server machine | docs/runs/lan-1000-proof-windows.md (not yet written) |
| Linux | portable | NOT MADE | void: the proof covers native server transports only (ruling of 2026-10-24) | none |
| macOS | portable | NOT MADE | void: the proof covers native server transports only (ruling of 2026-10-24) | none |
| Windows | portable | NOT MADE | void: the proof covers native server transports only (ruling of 2026-10-24) | none |

## Heads

The Linux cells ran at `1ca477339e150220f90e96bbe7c61be5b175eb51` and the
macOS cell at `813b4adbe3dec2eee496577e3da2c0d7d66b8063`. Between them:

```text
$ git diff --stat 1ca477339e150220f90e96bbe7c61be5b175eb51 813b4adbe3dec2eee496577e3da2c0d7d66b8063 -- sdk load_split.c tools/load_split_verify.h tools/check_split_load.py
 load_split.c                          |   2 +-
 sdk/rocketnet.h                       |  61 +++-
 sdk/rocketnet_api.cpp                 |  23 +-
 sdk/rocketnet_client_transport.cpp    | 140 +++++++++-
 sdk/rocketnet_constants.h             |  10 +
 sdk/rocketnet_driver.h                |   5 +-
 sdk/rocketnet_driver_builtin.cpp      | 282 +++++++++++++++++--
 sdk/rocketnet_drivers.inc             |   8 +-
 sdk/rocketnet_frame_sync.cpp          |  11 +
 sdk/rocketnet_frame_sync.h            |   3 +
 sdk/rocketnet_memory.h                |  35 +++
 sdk/rocketnet_platform.cpp            | 504 +++++++++++++++++++++++++++++++---
 sdk/rocketnet_platform.h              | 129 +++++++++
 sdk/rocketnet_server_iocp_windows.cpp |  38 ++-
 sdk/rocketnet_server_replay_marks.cpp |  39 ++-
 sdk/rocketnet_server_replay_marks.h   |  18 +-
 sdk/rocketnet_server_rio.h            |   4 +
 sdk/rocketnet_server_rio_windows.cpp  | 255 +++++++++++++++--
 sdk/rocketnet_server_socket.h         |  12 +-
 sdk/rocketnet_server_socket_linux.cpp | 158 ++++++++++-
 sdk/rocketnet_server_transport.cpp    |  79 ++++--
 sdk/rocketnet_server_uring_linux.cpp  |  62 ++++-
 sdk/rocketnet_xdp_replay_marks.h      | 112 +++++++-
 23 files changed, 1788 insertions(+), 202 deletions(-)
```

The difference is not empty. By the commit subjects in that range, it is:

- msgx: the server send banks are sized to the client count, and send
  failures and send handoff drops are counted by errno and by path (the
  macOS record cites this fix);
- the client transport ranking: the gso client ranks first on Linux and the
  iocp client above portable on Windows, portable last everywhere;
- the SDK's direct OS blocks and the Windows lifecycle resources are
  counted beside the allocator, and the memory plan reports the lifecycle
  peak (the only `load_split.c` change is the wording of its `split_plan`
  line, `scope=lifecycle_allocator_peak`);
- xdp reconnect freshness (replay marks) and the rio stop burst, which touch
  transports with no cell in this pair.

`tools/load_split_verify.h` and `tools/check_split_load.py`, the proof's
checks, are unchanged between the two heads.

## Earlier cell

Before the 600-second duration, one gso cell was recorded for 120 seconds
(run a1000c0062026101, head `59c266ff1fbcc3e3edca82998cf7f54697b6e7c2`,
labels from its own inventory):

```text
gso|server-a|1000|1000|120|yes|0|0:pass,1:pass,2:pass,3:pass,4:pass,5:pass,6:pass,7:pass|small:pass,fragmented:pass|277|pass
```

It is superseded by the 600-second gso row above and is kept for the
record; its host facts, load samples, proof lines and digests are in this
file's history.
