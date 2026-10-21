# Dedicated-server load qualification

`load_split` is a development-only, public-SDK load harness for a server on
one machine and disjoint client shards on other machines. The established
same-process `load` executable remains the correctness gate; its pass receipt
does not qualify dedicated-server performance. This split harness currently
supports at most `ROCKETNET_MAX_CLIENTS` (1000). Use the 10, 100, 250, 500,
1000 ladder with the SDK-default profile. A higher cap needs separate SDK
configuration and validation work.

Build the **same exact source revision** on every host with the development
SDK and tools enabled. Keep the default server/client configuration; the only
accepted nonempty `ROCKETNET_SERVER_*` and `ROCKETNET_CLIENT_*` environment
settings are the transport selectors. Nonempty `ROCKETNET_DEV_*` overrides
are refused. Choose an explicit aggregate limit in kbps below the measured
server and client host link capacity; the program does not invent a link rate.

Before allocating endpoints, run the role-specific, exact-binary public
memory planners on the intended hosts:

```text
load_split plan-server 1000
load_split plan-client-shard 1000 250
```

The `split_plan:` receipt covers public SDK create memory only: one server on
the server host, or the local number of clients on a shard host. It excludes
transport connect/start allocations, OS resources, and process overhead.
Provision and document a separate native/transport reserve before deployment.
Do not substitute the same-process `LOAD_PLAN_ONLY` total for these role
specific plans. Both role runs later compare exact pre-init/post-init,
pre-term/post-term handle and thread phase pairs and require successful
`rocketnet_term`.

Generate a fresh nonzero 16-digit hexadecimal run ID. Start the dedicated
server first, using an address it can bind and an IPv4 address reachable by
the client machines:

```text
load_split server 1000 60 0123456789abcdef 0.0.0.0:40000 192.0.2.10 80000
```

Capture its single `split_ready:` line. It supplies the actual address/port
and public key. Launch one or more client shards on other machines with the
same run ID, duration, total and aggregate limit, using nonoverlapping global
client-index ranges. For example, the first of four 250-client shards is:

```text
load_split client-shard 1000 60 0123456789abcdef 0 250 192.0.2.10:40000 <key-from-ready> 80000
```

The server readiness lease is 60 seconds until the first connection. From
the first server connection, all 1000 must connect within 10 seconds; each
client shard also has a local 10-second connection deadline after creating
its clients. These deadlines fail visibly on late setup. After connection,
the server sends 32-byte run-ID-bound ARM, waits up to 10 seconds for all
ACKs, sends GO and waits up to 10 seconds for all GO ACKs, then sends RUN.
RUN shares the reliable-ordered channel with later server-to-client data.
Clients begin their own full monotonic traffic window after receiving RUN;
machine UTC clocks are only reported for correlation, never used to shorten
the duration. Each role then runs exactly the requested traffic duration and
the existing three-second drain. Every traffic message is 32 bytes and
contains its run ID, global client index, reliability mode and sequence.
Client-to-server offers rotate through all eight reliability modes once per
client due time; server-to-client offers are reliable ordered. Neither role
catches up with a burst after a delayed update. Each client keeps an absolute
one-second due phase. A missed due slot is counted and makes load
qualification fail; `expected_offers` is duration times local client count.
Delivery success and target-load success are separate receipt fields.

At each local traffic boundary, clients send a run-ID-bound DONE and keep
pumping for their unchanged three-second drain. They then send DRAINED and
wait for the server's FINISH. The server keeps its endpoint alive until every
client has reported DRAINED, sends FINISH, waits for every FINISH ACK, and
sends CONFIRM. Clients close only after CONFIRM. The bounded completion
handshake has its own `barrier_actual_s` and `barrier_complete` fields; it does
not extend or relabel either measured traffic or drain interval. A late or
incomplete handshake is a failure. The server keeps a three-second
confirmation retransmission grace; missing client confirmation fails in the
merged receipts.

The server alone samples public server update/send API call histograms.
`split_server_timing:` covers traffic plus drain, with clock overhead retained.
Each role emits one `split_receipt:` after endpoint shutdown and SDK term,
plus one `split_stream:` line for every global client/direction/mode it owns,
and one `split_wire:` byte record per client. The server receipt separates
delivery, target load, bandwidth, server API timing, and resource verdicts;
overall PASS requires all of them. The timing budget is specifically the p99
upper bound of server update and send-packets calls; each must be at most 100
microseconds. Maxima are reported separately and do not define this p99
budget.
The server wire baseline is sampled before RUN; each client's baseline is
sampled before connect, because RUN can arrive during another client's
startup loop. Counters end at the role's traffic boundary. They conservatively
include RUN/startup bytes and divide by the
requested traffic duration, even if boundary sampling is late, so a scheduler
pause cannot dilute the reported rate. The actual monotonic interval is
reported separately. Per-client upload and download must each stay
strictly below 100 kbps; each role and the merged client shards must stay
strictly below the operator-supplied aggregate cap. These byte counters
include IP/UDP overhead, not Ethernet framing. Whole-run verified message
counts stop at each role's fixed three-second drain boundary; they are
separate from the traffic wire interval. Application packets observed during
the later completion barrier increment `late_application_deliveries` and fail
delivery qualification rather than repairing a missed drain deadline. The
receiver checks the monotonic deadline for each application message, including
batches returned by an update that crossed the boundary. The
merger recomputes aggregate and per-peer rates from the raw per-peer byte
records and conservative requested-duration denominator, accounting for
six-decimal receipt rounding.

The gate owner must provide a `split_allocation_v1` JSON manifest from the
actual runner/bench allocation record. It identifies the dedicated server
host and each shard's separate client host, allocation ID, allocation
reference, and runner ID, and binds every exact role log by SHA-256. A
hostname typed into a log is not machine-allocation evidence. The merger
checks that the server allocation is distinct from every shard allocation;
the recorded allocation references must be independently auditable. For
example, with `shard0.log`:

```json
{
  "schema": "split_allocation_v1",
  "run_id": "0123456789abcdef",
  "server": {"role": "server", "total": 1000,
             "host_id": "server-bench-id", "allocation_id": "server-lease-id",
             "allocation_ref": "gate/server-lease-receipt", "runner_id": "server-runner-id",
             "allocation_type": "dedicated_server", "log_sha256": "<sha256-of-server.log>"},
  "shards": [{"role": "client-shard", "first": 0, "count": 250,
              "host_id": "client-bench-id", "allocation_id": "client-lease-id",
              "allocation_ref": "gate/client-lease-receipt", "runner_id": "client-runner-id",
              "allocation_type": "client_host", "log_sha256": "<sha256-of-shard0.log>"}]
}
```

The manifest needs one `shards` entry for each `--shard` log in the same
order. The example must be expanded to the complete shard partition.

Collect the unmodified server and every shard log, then run:

```text
python3 tools/check_split_load.py --server server.log \
  --shard shard0.log --shard shard1.log --shard shard2.log --shard shard3.log \
  --run-id 0123456789abcdef --total 1000 --duration 60 \
  --aggregate-limit-kbps 80000 --server-transport portable \
  --client-transport portable --allocation-manifest allocation.json
```

The merger requires exactly one successful readiness and final receipt per
role, a complete nonoverlapping shard partition, matching run metadata,
full monotonic windows, bounded completion, strict cleanup phase equality,
the exact target offer count, finite complete timing, byte caps, auditable
host allocations, and every per-client stream count to match across roles. It
rejects missing, duplicated or mixed-run records. A local PASS alone is not
an end-to-end PASS. The initial same-host smoke, if run later, proves only
protocol and receipt correctness; it is not a dedicated-server performance
result. Use paired uninstrumented releases for performance comparison and
keep any diagnostic build as a separate attribution run.
