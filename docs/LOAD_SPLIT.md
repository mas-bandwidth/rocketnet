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
specific plans. Both role runs, and both planners, first pay one warm cycle
(init, a one-client loopback server, one client, term) so first-use
operating-system retention falls outside the measured cycle. They then take a
census of threads and handles by identity at pre-init, post-init, pre-term and
post-term (`load_census.h`) and fail on any thread or handle that survives
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
load_split server 1000 60 0123456789abcdef 192.0.2.10:40000 192.0.2.10 80000
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
load_split client-shard 1000 60 0123456789abcdef 0 250 192.0.2.10:40000 <key-from-ready> 80000
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
connects (`tools/load_split_startup.h`, `tools/test_load_split_startup.c`).
Every start message is reliable ordered: a lost packet is the SDK's resend
and costs delay inside these windows, never the message. The server is the
one authority on the start windows. A client shard waits for ARM, GO and RUN
against one deadline from its own connection: the server's 10-second
population window, both 10-second start windows, and one client
`reliable_message_timeout_ms` for RUN itself
(`tools/load_split_barrier.h`, modelled in `tla/SplitStart.tla`). Shards
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

The merger always exits nonzero unless the entire run qualifies. It also
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

## Space profile

The `space` profile replays the space game server's traffic at 1000 players
in place of the light default profile. It is opt-in: give `space` as one more
argument after the aggregate limit, to the server and to every client shard.
Without it (or with `default`) the harness runs the default profile, and its
command lines and receipts are unchanged.

```text
load_split server 1000 60 0123456789abcdef 192.0.2.10:40000 192.0.2.10 400000 space
load_split client-shard 1000 60 0123456789abcdef 0 250 192.0.2.10:40000 <key-from-ready> 400000 space
```

Traffic runs in 60 Hz frames on one absolute phase from the traffic start
(`tools/load_split_schedule.h`):

- Client to server: each client sends one 360-byte UNRELIABLE input a frame.
  A shard visits every local client once a frame: update, receive, offer,
  send.
- Server to client: each frame the server sends every client one 100-byte
  UNRELIABLE player message, and a 1184-byte UNRELIABLE_WITH_ACK_RECEIPT
  delta to the clients whose index mod 3 equals the frame's, so each client
  gets a delta every third frame (20 Hz), staggered as the game staggers
  them. The game's delta is up to 1400 bytes; 1184 keeps one player message
  and one delta in one packet. The server calls `rocketnet_server_send_packets`
  once a frame.

A late wake runs the latest due frame, never a burst, and counts every
client's share of each frame it skipped in `missed_due_slots`, which fails
load qualification as in the default profile. `expected_offers` is
60 x duration x local clients on a shard, and on the server that plus every
client's deltas (duration x 20 per client). Each stream tracks its sequence
numbers in the shared 4096-entry bitmap, so a space run is at most 68
seconds; a longer one is refused at the command line.

The readiness lines and receipts print `profile=space payload_bytes=360
rate_target_hz=60` and the full shape: `c2s_payload_bytes`, `c2s_rate_hz`,
`s2c_player_bytes`, `s2c_player_rate_hz`, `s2c_delta_bytes`,
`s2c_delta_rate_hz`, `s2c_delta_stagger=client_mod_3`, `server_frame_hz`,
`client_up_cap_kbps` and `client_down_cap_kbps`. A receipt also prints its
counts per mode: `c2s_unreliable_{offered,accepted,received}`,
`s2c_player_{offered,accepted,received}` and
`s2c_delta_{offered,accepted,received}`, the server's
`s2c_delta_receipts_acked` and `s2c_delta_receipts_lost` (only deltas ask for
receipts), and `messages_dropped_over_budget`. Every existing field stays.
`split_stream:` lines are `c2s combo=0`, `s2c combo=0` (player) and
`s2c combo=5` (delta) per client.

Per-client caps are strict and include IP/UDP headers, as in the default
profile. Each packet is allowed 96 bytes beyond its message bytes (28 of
IPv4/UDP, 26 of RocketNet packet, 42 for the acknowledgement block, payload
form and record headers), and every delta is assumed to take a packet of its
own:

| direction | derivation | need | cap |
| --- | --- | --- | --- |
| client to server | 60 x (360 + 96) x 8 / 1000 | 218.9 kbps | 250 kbps |
| server to client | (60 x (100 + 96) + 20 x (1184 + 96)) x 8 / 1000 | 298.9 kbps | 350 kbps |

The 100 kbps cap is the default profile's only. A role's per-client up rate
is what it sends, so the server's is checked against the download cap. The
aggregate cap is still the operator's argument: 1000 clients need about
219 Mbps up and 299 Mbps down.

Unreliable traffic may be lost on a real network, so loss is a number and
never a verdict. A receiver counts what it verifies; a gap in a stream is
loss, not an order error. Identity, payload and duplicate errors, offer
refusals, protocol errors and late deliveries still fail, and a stream that
receives more than its sender offered fails. A space message does not share
RUN's ordered channel and can overtake a RUN that was lost and resent; a
client that has sent its GO ACK accepts it.

Merge space logs with `--profile space` (the merger's default is `default`,
and a log of the other profile fails). The `split_merge_json:` line then adds
`profile`, `delivery_verdict` and a `loss` object with `offered`, `accepted`,
`received`, `lost` and `loss_ratio` for `c2s_unreliable`, `s2c_player` and
`s2c_delta`, and the server's delta receipt counts.
