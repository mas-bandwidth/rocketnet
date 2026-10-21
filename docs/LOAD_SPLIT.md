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
catches up with a burst after a delayed update.

The server alone samples public server update/send API call histograms.
`split_server_timing:` covers traffic plus drain, with clock overhead retained.
Each role emits one `split_receipt:` after endpoint shutdown and SDK term,
plus one `split_stream:` line for every global client/direction/mode it owns.
Traffic wire counters are sampled just before RUN and at the role's traffic
boundary. They conservatively include RUN/startup bytes and divide by only
the measured traffic duration. Per-client upload and download must each stay
strictly below 100 kbps; each role and the merged client shards must stay
strictly below the operator-supplied aggregate cap. These byte counters
include IP/UDP overhead, not Ethernet framing. Whole-run verified message
counts include the drain; they are separate from the traffic wire interval.

Collect the unmodified server and every shard log, then run:

```text
python3 tools/check_split_load.py --server server.log \
  --shard shard0.log --shard shard1.log --shard shard2.log --shard shard3.log \
  --run-id 0123456789abcdef --total 1000 --duration 60 \
  --aggregate-limit-kbps 80000 --server-transport portable \
  --client-transport portable
```

The merger requires exactly one successful readiness and final receipt per
role, a complete nonoverlapping shard partition, matching run metadata,
full monotonic windows, strict cleanup phase equality, accepted offers,
byte caps, and every per-client stream count to match across roles. It
rejects missing, duplicated or mixed-run records. A local PASS alone is not
an end-to-end PASS. The initial same-host smoke, if run later, proves only
protocol and receipt correctness; it is not a dedicated-server performance
result. Use paired uninstrumented releases for performance comparison and
keep any diagnostic build as a separate attribution run.
