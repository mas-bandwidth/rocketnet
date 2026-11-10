# Dedicated-server load qualification

`load_split` is a development-only, public-SDK load harness for a server on
one machine and disjoint client shards on other machines. The established
same-process `load` executable remains the correctness gate; its pass receipt
does not qualify dedicated-server performance. This split harness currently
supports at most the build's `ROCKETNET_MAX_CLIENTS` (1000 by default). Use the 10, 100, 250, 500,
1000 ladder with the SDK-default profile. A higher cap needs separate SDK
configuration and validation work.

Build the **same exact source revision** on every host with the development
SDK and tools enabled. Keep the default server/client configuration; the only
accepted nonempty `ROCKETNET_SERVER_*` and `ROCKETNET_CLIENT_*` environment
settings are the transport selectors and the server's AF_XDP core layout,
`ROCKETNET_SERVER_NUM_XDP_QUEUES` and `ROCKETNET_SERVER_NUM_OS_CPUS`. Nonempty `ROCKETNET_DEV_*` overrides
are refused. Choose an explicit aggregate limit in kbps below the measured
server and client host link capacity; the program does not invent a link rate.

Build the harness from the repository root on each host:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DROCKETNET_DEV=ON -DROCKETNET_BUILD_TOOLS=ON
cmake --build build --target load_split --parallel 4
```

Commands below use POSIX shell syntax and `./bin/load_split` from the
repository root. With Visual Studio, pass `--config Release` to the build,
run `bin\Release\load_split.exe`, and set environment variables in the
[Windows shell form](WINDOWS.md#pinning-a-transport).

The basic example pins the server and clients to `portable` so the merger's
expected transport names match. Set these before both planning and running:

```sh
# On the server host:
export ROCKETNET_SERVER_TRANSPORT=portable
# On every client host:
export ROCKETNET_CLIENT_TRANSPORT=portable
```

For native-server qualification, change the server pin and the merger's
`--server-transport` together. Do not infer the actual client transport from
the host OS; retain each role's reported transport.

Before allocating endpoints, run the role-specific, exact-binary public
memory planners on the intended hosts:

```text
./bin/load_split plan-server 1000
./bin/load_split plan-client-shard 1000 250
```

The `split_plan:` receipt reports `scope=lifecycle_allocator_peak`: one server
on the server host, or the local number of clients on a shard host. It includes
create and planned built-in transport allocations over repeated lifecycle
cycles, with unchanged configuration and environment overrides. It excludes
OS resources (`os_owned_resources=excluded`), custom-transport allocations
and process overhead. Provision and
document separate headroom for those exclusions before deployment.
Do not substitute the same-process `LOAD_PLAN_ONLY` total for these role
specific plans. Both role runs, and both planners, first pay one warm cycle
(init, a one-client loopback server, one client, term) so first-use
operating-system retention falls outside the measured cycle. They then take a
census of threads and handles by identity at pre-init, post-init, pre-term and
post-term (`tests/load/load_census.h`) and fail on any thread or handle that survives
post-init to pre-term or pre-init to post-term, and on a failed
`rocketnet_term`. Windows process thread pool workers (start routine in ntdll)
and their handles belong to the operating system: they are reported as
`os_pool_threads` and never counted. The raw counts are still reported; they
move with the operating system on Windows and are not the verdict
(`resource_baseline_schema=census_v1`).

Generate a fresh nonzero 16-digit hexadecimal run ID. Start the dedicated
server first, using its concrete client-reachable IPv4 endpoint. The first
address argument is passed to `rocketnet_server_create` and establishes the
server's protocol-advertised address. The second argument supplies the IPv4
host printed in `split_ready`; it must name the same address. It does not
override the SDK's advertised address. Replace `192.0.2.10` in this example
with the server's actual client-reachable IPv4 address:

```text
./bin/load_split server 1000 60 0123456789abcdef 192.0.2.10:40000 192.0.2.10 80000
```

Do not pass `0.0.0.0:40000` as the first argument while publishing a concrete
LAN address in readiness. Clients put the readiness endpoint in their connect
tokens, and the server rejects a token whose IPv4 endpoint differs from its
protocol-advertised endpoint, before issuing a connection cookie.

Capture its single `split_ready:` line. It supplies the actual address/port
and public key. Launch one or more client shards on other machines with the
same run ID, duration, total and aggregate limit, using nonoverlapping global
client-index ranges. For example, the first of four 250-client shards is:

```text
./bin/load_split client-shard 1000 60 0123456789abcdef 0 250 192.0.2.10:40000 <key-from-ready> 80000
```

The server readiness lease is 60 seconds until the first connection. From
the first server connection, all 1000 must connect within 10 seconds; each
client shard also has a local 10-second connection deadline after creating
its clients. These deadlines fail visibly on late setup. After connection,
the server sends 32-byte run-ID-bound ARM, waits up to 10 seconds for all
ACKs, sends GO and waits up to 10 seconds for all GO ACKs, then sends RUN.
RUN shares the reliable-ordered channel with later server-to-client data.
A client answers ARM and GO only once it reports CONNECTED: its transport
hands payloads from before then, and a send before CONNECTED is dropped, so
an ARM that arrives in that window is answered at the first update after it
connects (`tools/load_split_startup.h`, `tests/test_unit_load_split_startup.cpp`).
Every start message is reliable ordered: a lost packet is the SDK's resend
and costs delay inside these windows, never the message. The server is the
one authority on the start windows. A client shard waits for ARM, GO and RUN
against one deadline from its own connection: the server's 10-second
population window, both 10-second start windows, and one client
`reliable_message_timeout_ms` for RUN itself
(`tools/load_split_barrier.h`, modelled in `proofs/tla/SplitStart.tla`). Shards
finish setup at different times, and the first to connect waits for the
last; its own clock never cuts short a start the server would complete.
Either role ends the wait at once when a client it still waits for
disconnects. A start that does not complete prints one `split_start_failure:`
line naming the role, the shard's first client and count, the phase
(`connect`, `arm`, `go` or `run`), the reason (`timed_out` or
`disconnected`) and the number of clients still owed, followed by up to 32
`split_start_waiting:` lines, one per owed client, with that client's state
and packet counters.

Both roles also keep a delivery capture (`tools/load_split_capture.h`, read
through the development library's capture in `sdk/rocketnet_layers.h`; no
public API). Once a second each connection is sampled: packets sent and
received, its resends where the side keeps them per connection (clients
only), the age of its last accepted receive, the highest session sequence
received, sent and acknowledged, the reliable window (unacknowledged
numbers, the resend queue and the age of the oldest), the RTT estimate and
the send budget. RUN is marked when the server queues it, with the first
sequence any packet sent after it can carry, and when each client receives
it. On the server a largest acknowledged sequence at or past the mark prints
`acked_past_run=yes`: the client acknowledged a packet sent after RUN was
queued (RUN's own or a later one). `reliable_unacked` is the total outstanding
ordered window; a nonzero count cannot identify RUN specifically. Client
lines print `acked_past_run=na`. Server lines also retain `server_slot` and
`remote_endpoint` (numeric address and UDP port) from the connected mapping,
so a logical client can be correlated after its slot has been reset. These
fields are marked unavailable on client lines and never-connected entries.
XDP queue assignment and ring drops still require the per-XSK OS evidence.
A connection found ended between two samples prints its last connected
sample at once as a `split_capture:` line with `why=kick` (the SDK resets the
slot when it ends it), followed by its own counters on a client shard and
the side's totals. The server stops sampling in its confirmation grace,
where confirmed shards disconnect on purpose. A connection that ends less
than a second after it connected is never sampled connected and prints only
`end_never`. At the end of the
run every connection prints one `split_capture:` line (`why=end` live,
`end_ended` or `end_never`), then `split_counter:` lines: a named set of
delivery and refusal counters always, every other counter when nonzero,
and, on a client shard, each client's nonzero named counters from its last
connected sample. The merger ignores both prefixes. Detailed output is
synchronous: the first detected kick predates that output, but printing may
affect the timing of subsequent disconnects in the same run.
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
The server also times every main server call it makes in traffic plus
drain and prints one `split_server_call:` line per call, for
`server_update`, `server_receive_messages` (every call, including the one
that returns no batch), `server_start_message`, `server_finish_message`
and `server_send_packets`, each with its count, p50 and p99 upper bounds,
maximum and over-budget count. The budget for every one of them is 100
microseconds at p99, judged at the worst case only: 1000 clients, all
connected, in the full load mode. Pass `full` as the last argument to the
server and to every client shard: the server then ticks at 10 Hz and offers
one message to every client every update, and every client updates at 10 Hz
and offers one message every update (`rate_target_hz=10 load=full` in the
receipts; pass `--load full` to the merger). The default mode (one offer
per client per second, `load=rate_1hz`) and the 1, 10, 100 and 500 rungs are
correctness runs, not timing. `tools/check_server_calls.py` reads the server
logs and prints a row per log and call; only the worst-case rows carry a
budget verdict, the others print `NA`, and a set without a worst-case log is
invalid:

```text
python3 tools/check_server_calls.py --report-only s1000-full.log s1.log s10.log s100.log s500.log
```

A log missing a call, repeating one, mixing runs or with contradicting
quantiles is invalid evidence and exits 2. With `--report-only` a budget
miss prints `verdict=FAIL` and exits 0; without it the miss exits 1. The
check runs report-only until every call is inside the budget at the worst
case, and then without the flag.

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
merger recomputes aggregate and per-client rates from the raw per-client byte
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

The merger requires exactly one readiness and final receipt per role, a
complete nonoverlapping shard partition, matching run metadata,
full monotonic windows, bounded completion, no resource survivor in either phase pair,
the exact target offer count, finite complete timing, byte caps, auditable
host allocations, and every per-client stream count to match across roles. It
rejects missing, duplicated or mixed-run records. A local PASS alone is not
an end-to-end PASS. The initial same-host smoke, if run later, proves only
protocol and receipt correctness; it is not a dedicated-server performance
result. Use paired uninstrumented releases for performance comparison and
keep any diagnostic build as a separate attribution run.

For the default and `full` modes, the merger exits nonzero unless the entire
run qualifies. The `proof` mode below judges delivery separately and can exit
zero while reporting a failed timing verdict. It also
prints one `split_merge_json:` line with `schema=split_merge_v2` as JSON. A
fully validated run whose only failure is the unchanged server API p99
100-microsecond budget reports `overall_verdict=FAIL`,
`non_timing_verdict=PASS`, `timing_verdict=FAIL`, and
`reason=server_api_p99_budget`; the CLI still exits 1. It reaches that result
only after checking every stream, shard summary, raw wire cap, resource
phase, and exact-log allocation record. Missing or contradictory timing
evidence is invalid evidence, not a timing-only result. Malformed logs,
delivery errors, underload, resource imbalance, bandwidth excess, or
allocation gaps report `non_timing_verdict=FAIL` and never authorize a
performance-ladder continuation. A runner may continue to the next rung
only when `non_timing_verdict=PASS` and should retain the overall FAIL and
timing evidence in its report.

## Proof load

The proof load checks every delivery method end to end at 256 kbps per
client in each direction. Pass `proof` as the last argument to the server and
to every client shard, run the server pinned to one transport, and pass
`--load proof` to the merger:

```text
ROCKETNET_SERVER_TRANSPORT=gso \
./bin/load_split server 1000 60 0123456789abcdef 192.0.2.10:40000 192.0.2.10 600000 proof
./bin/load_split client-shard 1000 60 0123456789abcdef 0 250 192.0.2.10:40000 <key-from-ready> 600000 proof
```

The offer rate is full load's: every client updates at 10 Hz and offers once
per update, and the server offers once per client every 100 ms
(`rate_target_hz=10 load=proof payload_bytes=proof` in the receipts). The
server itself ticks at 100 Hz, as in the default mode, with the clients'
offer slots spread over five ticks. This reduces bursts between socket
drains: 1000 clients at 256 kbps offer 32 MB/s inbound. UDP send-buffer
requests are 1,000,000 bytes per socket, while receive-buffer requests scale
with runtime `max_clients` (`ceil(max_clients * 1000 kbps * 125 * 2 / 10 Hz)`,
at least 1,000,000 bytes: 25,000,000 bytes per socket at 1000 clients) across
portable, GSO and io_uring; actual grants depend on the host. Inspect the
startup grants as described in [Receive buffer sizing](MEMORY.md#receive-buffer-sizing).
Each offer, in each direction, is two messages: a 1200-byte small message in one
of the eight reliability modes and a 2000-byte fragmented message in one of
the five reliable modes, each rotating by offer from the client's index.
That is 3200 payload bytes per offer, an offered 256 kbps per client per
direction before packet overhead, so each client's wire rate is at least
256 kbps each way. An unreliable mode has no fragmented stream: the SDK sends
an unreliable message larger than one packet as its reliable twin. A stream
is one client, one direction, one mode and one size class, 13 per client and
direction, each on its own channel. The SDK default server and client
configuration is unchanged; the aggregate cap stays the operator's argument
(1000 clients at 256 kbps is 256 Mbit/s each way, under a 1 GbE path).

Every payload carries its stream, its sequence, its length, the run ID and an
FNV-1a checksum over its bytes, written and read by
`tools/load_split_verify.h`. The receiver of each stream records each
delivery against the rule for its mode:

| Mode | Rule |
| --- | --- |
| RELIABLE, RELIABLE_WITH_ACK_RECEIPT | exactly once against the sender's count |
| RELIABLE_ORDERED, RELIABLE_ORDERED_WITH_ACK_RECEIPT | exactly once, in order, no gaps |
| RELIABLE_SEQUENCED | never older than the newest delivered, never a duplicate; a stale message the receiver drops is a skip, counted and never failed |
| UNRELIABLE_SEQUENCED | never older than the newest delivered, never a duplicate; loss is counted, never failed |
| UNRELIABLE, UNRELIABLE_WITH_ACK_RECEIPT | never a duplicate; loss is counted, never failed |

The sender keeps a receipt ledger for the three `_WITH_ACK_RECEIPT` modes:
every send gets exactly one outcome, a reliable one is only ever ACKED, and
`receipts_dropped` is zero. A stream tracks sequences 1 to 8191 and a ledger
8192 receipt sends per client and direction, enough for a 10-minute run
(about 750 small and 1200 fragmented messages per stream, about 4650 receipt
sends); a run past either bound is reported, never wrapped.

With `proof`, the server refuses to run (exit 2, `split_transport_refused:`,
before any client connects) unless `ROCKETNET_SERVER_TRANSPORT` names one
transport and the granted transport is that one; `split_ready:` prints
`transport_pin=` beside `transport=`, and the server's `split_receipt:` prints
the same `transport_pin=` field, so the pin at readiness and the pin at the
end are the same one the merger binds. In any mode, a pin naming one transport
that is not the one granted is the same refusal. Any client disconnect
between the end of the population window and the planned close fails the
run, and the role prints one `split_disconnect:` line per client with its
phase and reason.

At the end each role prints, instead of `split_stream:` lines, one
`split_verify:` line per client, direction, mode and size class: `side=send`
with the count sent (and, for a receipt mode, the outcomes and the bitmap of
the sequences ACKED), or `side=receive` with the deliveries, the duplicates,
the order and stale counts (and, for a receipt mode, the bitmap of the
sequences delivered). One `split_proof:` line per role carries the checksum
failures, the local receipt failures, `receipts_dropped`, the disconnects,
the lowest per-client wire rate each way and the role's proof verdict. The
per-client bandwidth verdict in `split_receipt:` is the 256 kbps floor in
place of the 100 kbps cap.

`tools/check_split_load.py --load proof` joins the two ends of every stream
across roles: the sender's count is the proof's rotation; exactly-once
streams deliver the sender's count; ordered streams have no gap or reorder;
sequenced streams never deliver an older message; no stream has a duplicate
or a delivery the sender never sent; every receipt send has one outcome, a
reliable one ACKED, and every ACKED sequence is one the receiver recorded;
every client is at least 256 kbps on the wire each way in both roles' raw
records; no role reports a disconnect, a checksum failure or a dropped
receipt; and the granted transport is the pinned one in the readiness line,
in the server's receipt and in the server's proof line. It prints one line,
the cell verdict:

```text
split_proof_cell: verdict=PASS transport=gso total=1000 streams=26000 messages_sent=2400000 messages_delivered=2399996 unreliable_lost=4 sequenced_skipped=0 timing_verdict=FAIL
```

The cell verdict is the delivery proof, and the merger exits 0 when it
passes. The server API timing evidence is still validated and its verdict
printed beside it (`timing_verdict`, and `overall_verdict` in the JSON line
is FAIL when it fails); the timing budget is judged at full load, not here.

### A bounded proof run

One server bench and two client benches, 1000 clients in four shards of 250,
60 seconds, the server pinned to one transport. The addresses below are
placeholders; replace them with the benches' client-reachable IPv4
addresses. The server runs first and its `split_ready:` line gives the
address, the port and the public key the shards need:

```text
# on the server bench
ROCKETNET_SERVER_TRANSPORT=gso \
  ./bin/load_split server 1000 60 0123456789abcdef 192.0.2.10:40000 192.0.2.10 600000 proof

# On the client benches, keep ROCKETNET_CLIENT_TRANSPORT=portable.
# Launch all four shards concurrently in separate terminals or managed jobs:
# two shards on each client bench, never these four commands sequentially.
./bin/load_split client-shard 1000 60 0123456789abcdef 0 250 192.0.2.10:40000 <key-from-ready> 600000 proof
./bin/load_split client-shard 1000 60 0123456789abcdef 250 250 192.0.2.10:40000 <key-from-ready> 600000 proof
./bin/load_split client-shard 1000 60 0123456789abcdef 500 250 192.0.2.10:40000 <key-from-ready> 600000 proof
./bin/load_split client-shard 1000 60 0123456789abcdef 750 250 192.0.2.10:40000 <key-from-ready> 600000 proof
```

The aggregate cap is the operator's argument: 1000 clients at 256 kbps is
256 Mbit/s each way before packet overhead. The example's 600000 kbps cap
requires a path and every participating host with measured capacity above it;
do not infer available capacity from a nominal 1 GbE link alone. The four
shards carry disjoint client index ranges, two shards on each client bench.

Keep the five logs unmodified (`server.log`, `shard0.log` ... `shard3.log`),
the run ID, and the allocation manifest, whose `shards` array names the four
shards in the order the merger reads them:

```json
{
  "schema": "split_allocation_v1",
  "run_id": "0123456789abcdef",
  "server": {"role": "server", "total": 1000,
             "host_id": "server-bench-id", "allocation_id": "server-lease-id",
             "allocation_ref": "gate/server-lease-receipt", "runner_id": "server-runner-id",
             "allocation_type": "dedicated_server", "log_sha256": "<sha256-of-server.log>"},
  "shards": [{"role": "client-shard", "first": 0, "count": 250,
              "host_id": "client-bench-a", "allocation_id": "client-lease-a",
              "allocation_ref": "gate/client-lease-a", "runner_id": "client-runner-a",
              "allocation_type": "client_host", "log_sha256": "<sha256-of-shard0.log>"},
             {"role": "client-shard", "first": 250, "count": 250,
              "host_id": "client-bench-b", "allocation_id": "client-lease-b",
              "allocation_ref": "gate/client-lease-b", "runner_id": "client-runner-b",
              "allocation_type": "client_host", "log_sha256": "<sha256-of-shard1.log>"},
             {"role": "client-shard", "first": 500, "count": 250,
              "host_id": "client-bench-a", "allocation_id": "client-lease-c",
              "allocation_ref": "gate/client-lease-c", "runner_id": "client-runner-c",
              "allocation_type": "client_host", "log_sha256": "<sha256-of-shard2.log>"},
             {"role": "client-shard", "first": 750, "count": 250,
              "host_id": "client-bench-b", "allocation_id": "client-lease-d",
              "allocation_ref": "gate/client-lease-d", "runner_id": "client-runner-d",
              "allocation_type": "client_host", "log_sha256": "<sha256-of-shard3.log>"}]
}
```

Then the merger, with the proof cell:

```text
python3 tools/check_split_load.py --server server.log \
  --shard shard0.log --shard shard1.log --shard shard2.log --shard shard3.log \
  --run-id 0123456789abcdef --total 1000 --duration 60 \
  --aggregate-limit-kbps 600000 --server-transport gso \
  --client-transport portable --allocation-manifest allocation.json \
  --load proof
```

The run passes only when the merger exits 0 and prints
`split_proof_cell: verdict=PASS`. A failure names its cause on the
`split_merge:` line and in the cell verdict's `reason=`.

A proof run's logs and allocation manifest are operator artifacts: keep them
with the run's receipt. The merger's exit status and its
`split_proof_cell: verdict=PASS` line are the run's proof; the timing verdict
beside it is the server API budget, judged at full load and not by this cell.
