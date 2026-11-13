# Many-client load harness

For a dedicated server machine with remote client shards, see
[`LOAD_SPLIT.md`](LOAD_SPLIT.md). This same-process harness remains the
correctness gate and does not qualify dedicated-server performance.

`bin/load` is the many-client load harness for up to
`ROCKETNET_MAX_CLIENTS` clients. It uses the public client/server objects and
the ordinary five-step frame order. Both profiles offer at most one packet
per client per update. The harness visits up to 100 clients every 10 ms, so
at 1000 clients each client updates at 10 Hz; for 1–100 clients each client
updates at 100 Hz, with intermediate rates determined by the number of slices
per round. `LOAD_PROFILE=default` (the default)
keeps the SDK's default message and transport sizing. `LOAD_PROFILE=light`
uses the same traffic and checks with the SDK's public light-client profile,
`rocketnet_server_light_config` and `rocketnet_client_light_config`, sized for
one message of up to 1,415 bytes (`ROCKETNET_MAX_PACKET_PAYLOAD_BYTES - 1`,
sent in up to two 1,500-byte MTU packets) per 100 ms in each direction at
10 Hz. It does not
change public defaults; the `default` profile remains the stock acceptance.

## Low-bandwidth population steps

The requested steps is 10, 100, 250, 500 and 1,000 clients. At each rung,
offer one 32-byte application message per client per second in each direction
with the fixed-rate option below. For the stock 1,000-client acceptance, run
the `default` profile so the result covers SDK-default sizing. The `light`
profile is a separate, additional bounded-memory workload; it cannot substitute
for the default-profile acceptance. The current `bin/load` harness is capped
at the build's `ROCKETNET_MAX_CLIENTS` (1,000 by default) and records no
fixed-rate step results (below). The
recorded 1,000-client results are the LAN proof cells in
[`runs/lan-1000-proof.md`](runs/lan-1000-proof.md)
([Linux gso and io_uring](runs/lan-1000-proof-linux.md),
[AF_XDP](runs/lan-1000-proof-xdp.md)) (1,000 clients for 600 s,
300 s for AF_XDP, against Linux `gso`, `io_uring`, and AF_XDP copy-mode
servers and a macOS `msgx` server; minimum per-client rates of 276–283 kbps)
and the 60-second non-timing correctness rungs in
[`QUALIFICATION-v1.0.0-preview-2.md`](QUALIFICATION-v1.0.0-preview-2.md);
they apply only under their exact workload and deployment assumptions.
Above 1,000 players, callers research, stabilize, and qualify their own
configuration, memory budget, and host/NIC deployment; no arbitrary client
count or general qualification above 1000 is available. The older
[10000-client build record](QUALIFICATION-max-clients-10000.md) covers its own
bounded light-profile loopback workload only.

For every rung, require each client's maximum wire rate to stay strictly below
100 kbps in both directions, and compare aggregate traffic in each direction
with that direction's available link rate. Record the exact source head,
population, profile, transports and receipt. Report the highest population that
passed all functional and rate criteria, the first population with a functional
failure, and any higher population blocked by a capacity guard as `UNRUN`.
A capacity refusal is not a functional failure, and an unrun rung has no pass
result. Product documentation records no current step results.

`bin/load` puts the public server and all clients in one process on loopback.
Its byte and rate counters measure this colocated logic workload; they do not
measure utilization of a physical NIC or prove physical line rate. Keep a
separate test with traffic crossing the intended physical link for line-rate
acceptance, and compare its aggregate directional rate with measured link
capacity.

All load and performance measurements must isolate the server on a dedicated
machine and run the load-generating clients on other machines, with traffic
crossing the intended network path. Same-host or single-process client/server
runs, including the current `bin/load` harness, are correctness tests only.
Their timing diagnostics are confounded by colocated client and server work and
never qualify server performance. Preserve previously collected results with
their original topology and scope labels as historical diagnostics; do not
erase or present colocated timings as server-performance measurements.

Set `LOAD_PLAN_ONLY=1` with the same client count, profile and SDK environment
as the intended run to get a `load_plan:` receipt after the harness's brief
local warm-up connection cycle. The public planner itself opens no socket or
endpoint. The receipt reports `client_public_required_bytes`,
`server_public_required_bytes` and checked
`total_public_required_bytes = server + clients * client`. A zero required
size, arithmetic overflow or failed SDK termination returns a FAIL receipt and
nonzero exit. Required-memory figures bound allocator memory over the
lifecycle for the largest planned built-in transport in the configured transports,
including create and connect/start allocations. Development builds include the
link simulator. Operating-system-owned memory, custom-transport allocations
and configuration or environment overrides changed after planning remain
outside this bound. `transport_selection=unresolved_before_start` means the
eventual transport is still unknown. The planner cannot certify host
capacity or the eventual transport choice; a runner needs a separately
justified reserve and should use the exact binary and environment for both
planning and execution. The reserve byte count and evidence string are
operator-supplied inputs. The helper checks that the evidence text names the
source head, OS, transport and population, but cannot verify that the reserve
was measured correctly or that the host can supply that capacity. The plan and
reserve are planning inputs, not machine-verifiable proof of total runtime
memory or link capacity.

### The nightly AF_XDP cell's guest memory

The nightly many-client step (`.github/workflows/nightly-soak.yml`) runs the
xdp cell as root inside a virtme-ng guest, with the server and all 1,000
default-profile clients in one `bin/load` process. The plan
(`LOAD_PLAN_ONLY=1 ./bin/load 1000 60` with the cell's environment) is the
lifecycle allocator peak: an upper bound on the bytes requested, most of
which is never touched, since UMEM, rings and pools are allocated whole and
filled only by traffic. Two things must fit the guest:

- the server's plan, `server_public_required_bytes`, requested as one block:
  the guest kernel's overcommit admits it only if the guest holds it;
- the memory the run touches, its peak RSS.

The plan, with the cell's environment (`ROCKETNET_SERVER_TRANSPORT=xdp
ROCKETNET_CLIENT_TRANSPORT=portable ROCKETNET_SERVER_NUM_XDP_THREADS=1
ROCKETNET_SERVER_NUM_OS_CPUS=1`), on the build in
[Platform, build, and allocator assumptions](#platform-build-and-allocator-assumptions):

| Plan | Bytes |
|---|---|
| `total_public_required_bytes` | 42,130,217,245 |
| `server_public_required_bytes` (one block) | 20,057,063,245 |

The touched memory is a measurement of an earlier source, whose server plan
was larger: peak RSS 1,687,416 kB in a 64G guest and 1,701,968 kB in a 24G
guest, and a 16G guest refused the server create with `OUT_OF_MEMORY`.

The step sizes the guest as `need = server plan + 1000 * touched_per_client_bytes`
(1,742,815, the larger peak over 1,000 clients) and `need * 5 / 4 + 1 GiB`
(the 1 GiB is the guest kernel's own), rounded up to whole GiB: 27G for the
plan above. It
writes the plan, the server block, the need, the guest and the runner's
`MemAvailable` to the job summary, and prints `LOAD cell=xdp FAIL runner has
... MiB available` when the runner holds less than the guest. The guest's
load exit status, peak RSS, `/proc/meminfo` totals and `dmesg | tail -n 50`
go into `vm.log` on every run, and the step prints `guest_peak_rss_kib` with
a WARN line when the peak passes the measured touched bytes.
The step reads the plan on every run, so the guest follows the source. Measure
residual OS and process overhead again against the exact current head,
configuration and population before treating this allowance as measured
headroom or reducing it. A profile change or a WARN line also requires new
measurement.

For a constant offered application rate across population sizes, set
`LOAD_MESSAGES_PER_CLIENT_SECOND=1`. This offers one 32-byte message per client
per second in each direction while keeping the same client update schedule.
An explicitly present empty, oversized or invalid rate is rejected; only an
unset variable selects the original workload.
Client-to-server messages still rotate through all eight reliability modes;
server-to-client messages remain reliable-ordered. A delayed client update
offers at most one message pair and sets its next due time from that update,
so the harness never catches up with a burst. Without this variable, the
original one-pair-per-update workload is unchanged. The summary prints the
target, offered and accepted-message rates per client per second; the offered
rate can fall below target when updates are slow, and the accepted rate can
fall further when sends are refused. `application_offers_accepted=PASS` means
every message that was offered was accepted by the API; it does not qualify the
workload against its requested rate. Report the requested target, observed
offered and accepted rates, and offered-to-target ratio separately for each
direction. The finite traffic window can leave the observed offered rate
slightly below the requested rate even on a healthy run. Keep that difference
visible; no numerical tolerance is defined here, so do not round it away or add
an acceptance threshold. A below-target workload is not evidence that the
requested rate was sustained. Fixed-rate runs report
`application_offers_accepted=FAIL` and fail the verdict if either direction
refuses an offered message. In every profile, a client that offered sends in a
direction and had every one of them refused for the whole run fails the
verdict: the summary prints `sends_accepted=FAIL` with
`clients_c2s_refused_whole_run=` and `clients_s2c_refused_whole_run=` counts
and `failed_criterion=sends_refused_whole_run`, and stderr names the first
eight such clients.

Run one fixed-rate rung with the profiling helper on a clean Linux checkout
and a unique output path. This example selects native GSO for the server:

```sh
src=/path/to/clean/rocketnet
head=$(git -C "$src" rev-parse HEAD)
python3 "$src/tools/profile_1000_load.py" \
  --mode timings --source "$src" --expected-head "$head" \
  --server-transport gso --seconds 60 --clients 100 \
  --profile light --messages-per-client-second 1 \
  --output /path/to/evidence/light-100
```

This example selects the additional light profile. The stock-1,000 acceptance
uses `--clients 1000 --profile default`; that profile also requires a
positive `--transport-overhead-reserve-bytes` and matching
`--transport-overhead-evidence` metadata.

The `wire_window=traffic` counters are read before the traffic clock
starts and at the first drain transition. `wire_window_seconds` measures from
the traffic clock through the transition read, including any last traffic
slice overrun.
`max_client_wire_up_kbps` and `max_client_wire_down_kbps` are the maximum over
individual clients of their SDK byte-counter deltas divided by that window;
they are not derived from an aggregate average. The client and server traffic
byte totals and aggregate kbps are also reported separately. The drop-one
negative fixture labels the traffic window `PARTIAL`; its destroyed client's
maximum uses `dropped_client_wire_observed_seconds`, ending when that client
is destroyed, while the aggregate rate still uses the full traffic window.
Fixed-rate runs
require both per-client maxima to stay strictly below 100 kbps; the summary
reports `client_wire_under_100kbps`. `session_*_wire_bytes_*` covers
handshake, traffic and drain before teardown, as labeled by
`wire_session_scope`. These SDK counters do not establish the physical NIC's
line rate, so a scaling series must compare its aggregate measured traffic
with the actual host link capacity separately. The fixed-rate workload has
`timing_baseline=fixed_rate_v1`; compare its frame timings only with runs that
share rate, population, transport and measurement scope.

The light profile is the SDK's public light-client geometry: eight channels (or
`ROCKETNET_MAX_CHANNELS` in a build of fewer), a
256-message in-flight reliability window, a 142,400-byte receive buffer, a
1415-byte (`ROCKETNET_MAX_PACKET_PAYLOAD_BYTES - 1`) maximum message, a
141,500-byte send buffer, a 128-message send queue, 256 packets in flight, 4
packets and 4 unreliable messages per update, 100 sequenced messages held, and
128 receipts. Its
256-kbps per-client send budget is 2.13 times the roughly 120-kbps wire rate
of one 1,500-byte packet per 100 ms in each direction (10 * 1,500 * 8 =
120,000 bits/s). The reliable timeout stays at its default, 0 (off). On the Linux x86-64
Release development build (`ROCKETNET_DEV=1`, io_uring and XDP built) described
in the tables below, at `max_clients = 1000`
the server's `rocketnet_server_required_memory` is 2,147,358,194 bytes and the
client's `rocketnet_client_required_memory` is 15,195,026 bytes. Both include
the planned lifecycle allocator peak plus the 1 MiB caller-allocator allowance,
with buffers sized from runtime configuration and the 512-client allocation
floor.
These are configuration calculations and planned allocator peaks, not
measured wire usage. The summary reports each endpoint's
`rocketnet_*_required_memory` figure. Those figures exclude the memory the
operating system owns (thread stacks, XDP UMEM, io_uring rings, kernel socket
buffers) and process overhead, so they are not whole-process memory estimates. The harness's own
payloads are 32 bytes, so its wire rates stay far below the profile's budget.

The summary also reports per-call Release timing samples during the traffic and
drain window. Each public frame API call is bracketed by the monotonic clock;
the fixed histograms use 1-us buckets through 100 ms, with an overflow bucket
bounded by the exact maximum. Quantile fields are explicitly upper bounds;
counts, maxima, and calls at or above 100 us are exact. The separate
`clock_pair_*` fields report 10,000 back-to-back clock-pair controls. Clock
overhead remains in each API duration and is not subtracted. The legacy
`server_update_p50_us` and `server_update_p99_us` keys remain aliases of the
corresponding upper bounds. Setup and connection, injected-drop stop/destroy,
final statistics, and general teardown calls are not timed.

On Windows, the periodic resource sample keeps the cheap process handle count
but does not repeat the global Toolhelp thread census inside the timed loop.
The resource-baseline verdict is a census at pre-init, post-init, pre-term
and post-term after one warm cycle (init, a one-client loopback server, one
client, term; `tests/load/load_census.h`): no thread or handle may survive post-init to
pre-term or pre-init to post-term, by identity. Windows process thread pool
workers (start routine in ntdll) and their handles belong to the operating
system and are reported as `os_pool_threads`, never counted. The raw counts
are still reported (`resource_baseline_schema=census_v1`); on Windows they
move with the operating system and are not the verdict. The reported `threads_peak` is the maximum
**observed at boundaries**: post-init, after setup, after connect, after the
traffic-and-drain loop, and pre-term. It is not a continuous thread peak;
`threads_peak_scope` names that sampling policy. A thread census at the
traffic-to-drain transition would consume the unchanged three-second drain
deadline, so there is no mid-drain boundary sample. `fds_peak` still uses
periodic handle counts, labeled by `fds_peak_scope`. Windows timing now carries
`timing_baseline=fixed_rate_win_boundary_threads_v2` (or
`fair_drain_win_boundary_threads_v2` without fixed rate), because removing
periodic Toolhelp scans changes the observed workload. Other platforms keep
their existing sampling and timing baseline.

For example, `LOAD_PROFILE=light ./bin/load 1000 60` selects the bounded
workload configuration. Omit `LOAD_PROFILE` or set it to `default` to preserve
the SDK-default sizing.

Every payload carries a deterministic client, reliability, channel and sequence
identity. The harness drains the endpoint-wide receive queue and checks:

- every client connects within the configured deadline;
- no unexpected disconnect occurs;
- reliable messages arrive exactly once and ordered streams stay in order;
- every delivered unreliable message is intact and at most once;
- receipt and drop counters reconcile with attempted sends (`counters_agree`).
  After the drain the harness quiesces one direction at a time and, at each
  cut, requires packets sent = packets received + every counted drop, with
  nothing left in flight and the XDP ring empty. The counted drops are the
  library's `SEND_FAILED`, `XDP_KERNEL_DROPPED_*` and
  `XDP_RECEIVE_FRAME_DROPPED`, and the kernel's UDP `SndbufErrors` (sender's
  namespace) and `InErrors` (receiver's namespace) from `/proc/net/snmp`;
  Linux counts a send its device queue refused in `SndbufErrors` while
  `sendmsg` reports success. The message check is a bound, not an exact
  count: client-to-server messages not covered by the library's message drops
  must be at most (client-to-server packets counted as dropped) x (the most
  unreliable messages any one client `send_packets` call carried in the run).
  The harness cannot tell which packets were dropped or what each carried, so
  the bound assumes every dropped packet carried the most; it catches gross
  loss only. In a measured run 398 dropped packets allowed 398 lost messages
  while about 64 were really lost, and a planted loss of every 100th
  unreliable message (about 318 more) still passed. The packet equations are
  the exact part. When the clients share the
  server's namespace the kernel counters are host-wide, so they bound the
  remainder instead of naming it. Every term prints to stderr on every run.
  Untested: the server-side send term (`SndbufErrors` in the server's
  namespace beside the server's `SEND_FAILED`). Every recorded run had zero
  server-side send drops, so that term has never been nonzero. Exercising it
  needs a socket server cell whose server-side veth is constrained (a short
  transmit queue on the server end) so that its sends overflow; that run is
  owed;
- in the `drop-one` mode the destroyed client's disconnect burst and the
  server's later packets to it fall outside both packet equations, so they
  are printed and not judged;
- in fixed-rate runs (`fixed_message_rate`), a refused `start_message` or
  `finish_message` is counted and fails the `application_offers_accepted`
  criterion.

Short 10- and 100-client cases serve as integration gates. The
1,000-client LAN case is recorded in
[runs/lan-1000-proof.md](runs/lan-1000-proof.md) for the native server
transports (Linux gso and io_uring for 600 seconds at
`1ca477339e150220f90e96bbe7c61be5b175eb51`, Linux xdp copy mode for 300
seconds at `534cce2e10159b52074ea9b10f7a2f0b074f7298`, and macOS msgx for
600 seconds at `813b4adbe3dec2eee496577e3da2c0d7d66b8063`). The portable
transport is a fallback, not a production server transport, so its LAN cells
do not count, and the Windows iocp and rio cells were not made. The thirty-minute soaks are recorded in
[runs/linux-soak-30min-per-transport.md](runs/linux-soak-30min-per-transport.md),
[runs/macos-soak-per-transport.md](runs/macos-soak-per-transport.md) and
the Windows soak records. Those records are connection-and-delivery
evidence: each LAN record's `timing_verdict` is FAIL, so the harness's
call timings are not a performance or throughput claim.

## Choosing configuration for concrete game workloads

Exact byte counts in this section use the Linux build identified under
[Platform, build, and allocator assumptions](#platform-build-and-allocator-assumptions).
Recompute with the exact deployed library; source changes can alter headers,
worker banks and lifecycle peaks without changing the public configuration.

Neither `rocketnet_*_default_config` nor `rocketnet_*_light_config` is a
universal preset suitable for every game:

- **`rocketnet_client_default_config` / `rocketnet_server_default_config`**
  reserves general-purpose headroom across every dimension: 32 channels, a
  1 MiB (`1,048,576`-byte) maximum message and receive
  ring, a 4 MiB (`4,194,304`-byte) send buffer, a 1,024-message reliable window, 1,024
  packets in flight, 256 packets and 1,024 unreliable messages per update,
  a 1,000-kbps client send budget, and a 10,000-kbps server per-client send
  budget. The server budget produces a `19,043,456`-byte (18.16 MiB) message
  region per connection and a `20,224,798,194`-byte
  (18.84 GiB, `ROCKETNET_DEV=1`) worst-case server allocator footprint at
  `max_clients = 1000`. A game with small messages, fewer channels, or smaller
  match sizes pays for unused per-client arena space if it leaves every field
  at `0` (default).
- **`rocketnet_client_light_config` / `rocketnet_server_light_config`** is
  sized specifically for a 10 Hz (100 ms) update cadence carrying at most one
  message of up to 1,415 bytes (`ROCKETNET_MAX_PACKET_PAYLOAD_BYTES - 1`)
  every 100 ms in each direction across 8 channels. A 1,415-byte message is
  above every one-record limit (`rocketnet_max_unfragmented_message_bytes`,
  at most 1,411), so it goes in up to two parts, and an unreliable one is sent
  as its reliable type. The profile sets
  `packets_per_update = 4`, `unreliable_messages_per_update = 4`,
  `send_queue_messages = 128`, `reliable_messages_in_flight = 256`, and a
  256-kbps server `per_client_send_kbps` budget. That cuts the server's
  per-client message region to `967,744` bytes (0.92 MiB), but any game that
  sends messages larger than 1,415 bytes, emits more than 4 unreliable messages
  or 4 packets per `update` tick, or bursts above 256 kbps per client will hit
  a refused send (`start_message` returns `NULL`, counted in `send_refused`;
  the connection stays up) or `messages_dropped_over_budget`.

Size a game's configuration by mapping its concrete simulation and traffic
requirements to each configuration group:

### 1. Client capacity (`max_clients`, `ROCKETNET_MAX_CLIENTS`)

- **Runtime `max_clients`** (`rocketnet_server_config_t.max_clients`,
  environment `ROCKETNET_SERVER_MAX_CLIENTS`, range `1 .. ROCKETNET_MAX_CLIENTS`,
  default `min(ROCKETNET_MAX_CLIENTS, 1000)`; `0` is refused at create with
  `CONFIG_OUT_OF_RANGE`): scales both the
  message-layer connection array (`max_clients * connection_bytes`) and
  per-client transport state (session slots and challenge/encryption/replay
  tables).
- **Tradeoff**: Set `max_clients` to the actual maximum player count of the
  server instance (for example `16` for a squad server, `64` for an arena
  match, or `100` for a survival match). Lowering `max_clients` from `1000` to
  `64` on the `default` profile reduces `rocketnet_server_required_memory` from
  `20,224,798,194` bytes (18.84 GiB) to `1,910,112,962` bytes (1.78 GiB) with
  unpinned `transport = NULL` (`"auto"`), or to `1,512,642,055` bytes
  (1.41 GiB) with `transport = "gso"`.
- **Compile-time `ROCKETNET_MAX_CLIENTS`**: the most clients a config may ask
  for (default `1000`, supported build range `512 .. 32768`). Server buffers
  that scale with clients are sized from runtime `max_clients`, raised to the
  512-client allocation floor, so the build ceiling adds no memory to a
  smaller server. Use runtime `max_clients` for smaller matches and rebuild
  every consumer with the same compile-time ceiling.
- **Scale qualification boundary**: The recorded 1,000-client results in
  [`runs/lan-1000-proof.md`](runs/lan-1000-proof.md) and
  [`QUALIFICATION-v1.0.0-preview-2.md`](QUALIFICATION-v1.0.0-preview-2.md)
  apply only with their exact workload and deployment
  assumptions. Above 1,000 players, callers must research, stabilize, and
  qualify their own configuration, memory budget, and host/NIC deployment; no
  arbitrary client count or validated-above-1000 guarantee is available.

### 2. Channels and ordering domains (`num_channels`, `ROCKETNET_MAX_CHANNELS`)

- **Runtime `num_channels`** (`rocketnet_*_config_t.num_channels`, environment
  `ROCKETNET_CLIENT_NUM_CHANNELS` / `ROCKETNET_SERVER_NUM_CHANNELS`, range
  `1 .. ROCKETNET_MAX_CHANNELS`, default `32`, light `8`, each at most
  `ROCKETNET_MAX_CHANNELS`): each channel owns
  independent ordered (`ROCKETNET_RELIABILITY_RELIABLE_ORDERED` and
  `ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT`) and sequenced
  (`ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED` and
  `ROCKETNET_RELIABILITY_RELIABLE_SEQUENCED`) state.
- **Tradeoff**: Head-of-line blocking on `RELIABLE_ORDERED` is scoped to a
  single channel. Separate unrelated streams onto distinct channels (for
  example: channel 0 for session control and RPCs, channel 1 for high-rate
  entity state, channel 2 for combat/inventory events, channel 3 for chat or
  telemetry) so packet loss on one ordered stream never stalls another. Per-channel
  state is only `88 * C` bytes per connection (`64 * C` send + `24 * C`
  receive), so channel count has negligible memory cost compared to
  arenas and windows, but `client` and `server` must configure the same
  `num_channels` or the connection ends in
  `ROCKETNET_CLIENT_STATE_CONFIG_MISMATCH` /
  `ROCKETNET_DISCONNECT_REASON_CONFIG_MISMATCH`.

### 3. Message size and arenas (`max_message_bytes`, `receive_message_buffer_bytes`, `send_message_buffer_bytes`)

- **Single-packet vs. fragmented messages**: `max_message_bytes` is the public
  ceiling for every reliability mode, from
  `ROCKETNET_MAX_PACKET_PAYLOAD_BYTES - 1` (`1415`) through
  `ROCKETNET_MAX_MESSAGE_BYTES` (`1,048,576`). Exact one-packet limits depend
  on the reliability form's headers; larger messages use the message layer's
  bounded reliable-fragment path. The transport carries at most one packet's
  payload, `ROCKETNET_MAX_PACKET_PAYLOAD_BYTES` (`1416`), per send.
- **Validation invariants**:
  - `1415 <= max_message_bytes <= ROCKETNET_MAX_MESSAGE_BYTES` (`1,048,576`).
  - `receive_message_buffer_bytes` is a multiple of 16 in `4096 .. 1048576`,
    and `max_message_bytes <= receive_message_buffer_bytes`.
  - `12288 <= send_message_buffer_bytes <= 268435456`.
  - The number of 1,152-byte fragments of one `max_message_bytes` block must
    not exceed `reliable_messages_in_flight`. `send_message_buffer_bytes` has
    no size relation to `max_message_bytes`: a message larger than one
    1,152-byte slab is held in the send spans (`SR`), which are sized from
    `receive_message_buffer_bytes`.
  - Violating any of these bounds causes `rocketnet_client_create` or
    `rocketnet_server_create` to return `ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE`
    (and `rocketnet_*_required_memory` to return `0`).
- **Receive ring (`receive_message_buffer_bytes`, `B`)**: Must hold all
  delivered messages and out-of-order `RELIABLE_ORDERED` messages waiting for a
  gap to fill between `update` calls. Size `B` to at least
  `max_message_bytes` plus the peak bytes that can arrive across the
  `reliable_messages_in_flight` window during a burst or retransmit stall.
- **Send store (`send_message_buffer_bytes`, `S`)**: Holds outbound reliable
  and queued unreliable payloads across three slab classes (`128`, `512`, and
  `1,152` bytes). When `send_kbps` / `per_client_send_kbps` is positive, the
  message layer adds a payload reserve for `message_resend_delay_max_ms`
  (`ceil(kbps * 125 * message_resend_delay_max_ms / 1000)` bytes) to `S`,
  `Q`, and `SR` so a rate-limited link does not run out of slab blocks while
  waiting to resend. The default maximum resend delay is 2,000 ms; the
  separate `reliable_message_timeout_ms` default is 0 (off).

### 4. Queues, reliable windows, per-update caps, sequenced holds, and receipts (`send_queue_messages`, `reliable_messages_in_flight`, `packets_in_flight`, `packets_per_update`, `unreliable_messages_per_update`, `sequenced_messages_held`, `max_message_receipts`)

| Field | Symbol | Range | Default | Light | Sizing rule and tradeoff |
| --- | --- | --- | --- | --- | --- |
| `send_queue_messages` | `Q` | `16 .. 1048576` | `1024` | `128` | Maximum messages queued for transmission before/during send pacing (`72` bytes per descriptor plus resend-delay reserve). Increase if a frame queues many small messages before `update`. |
| `reliable_messages_in_flight` | `W` | power of two, `64 .. 1048576` | `1024` | `256` | Sliding window of unacknowledged reliable messages (`112.125 * W` bytes plus delivered tracking). Size `W >= peak_reliable_msg_rate * RTT_sec * 2` so normal RTT never blocks reliable sends. |
| `packets_in_flight` | `P` | `64 .. 1048576`, and `P >= W` | `1024` | `256` | Sent-packet tracking ring (`128 * P` bytes). If `P < W`, create returns `ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE`. Entries are held by packets carrying reliable messages or `UNRELIABLE_WITH_ACK_RECEIPT` messages; other unreliable packets take none. Occupancy is the reliable packets plus the receipt-bearing unreliable packets sent within one resend delay, and `P >= W` covers only the reliable part. When unreliable messages are sent with receipts alongside reliable messages, set `P > W` (e.g. `2 * W`) so an entry is never evicted before its ack arrives; an evicted entry's receipts report loss. |
| `packets_per_update` | — | `1 .. 1024` | `256` | `4` | Maximum UDP packets emitted per connection in a single `update` call. Must cover peak per-tick packet bursts (including message-layer block fragments and retransmits). |
| `unreliable_messages_per_update` | `Du` | `1 .. 65536` | `1024` | `4` | Maximum unreliable/sequenced messages accepted and delivered per connection per `update` (`24 * (W + Du)` bytes in delivered table). Must exceed the peak unreliable messages sent or received in one frame. |
| `sequenced_messages_held` | `Ru` | `0 .. 4096` | `16` | `100` | Slots holding the latest sequenced message per channel (`1,440 * Ru` bytes). Needs at least as many slots as active sequenced channels (`Ru >= active_sequenced_channels`). |
| `max_message_receipts` | `Rq` | `16 .. 65536` | `256` | `128` | Receipt ring capacity (`16 * Rq` bytes) for `*_WITH_ACK_RECEIPT` modes drained via the receipt APIs. Size to the maximum receipt events that can complete between two drains. |

### 5. Wire send budgets (`send_kbps`, `per_client_send_kbps`, `reliable_message_timeout_ms`)

- **`send_kbps`** (client, capped range `1 .. ROCKETNET_SEND_KBPS_UNLIMITED - 1`,
  default `1000`, light `256`) and **`per_client_send_kbps`** (server, the same capped range, default
  `10000`, light `256`) bound per-connection outbound wire rate. Zero selects
  that endpoint's default; `ROCKETNET_SEND_KBPS_UNLIMITED` disables the budget.
- **Double effect on behavior and memory**:
  1. At runtime, packets that would exceed the connection's token-bucket wire
     budget delay reliable transmission or drop unreliable messages (counted in
     `messages_dropped_over_budget`).
  2. At create time, the message connection planner reserves additional send
     descriptors (`Q`), send store slabs (`S`), and send spans (`SR`) proportional
     to `send_kbps` / `per_client_send_kbps` over
     `message_resend_delay_max_ms` (2 seconds by default).
     Lowering the server budget from `10000` kbps (`default`) to `256` kbps
     (`light`) together with the light buffer geometry reduces its
     `connection_bytes` from `19,043,456` bytes (18.16 MiB) to `967,744`
     bytes (0.92 MiB). (When calling the internal layer helper
     `rocketnet_message_connection_bytes` or `rocketnet_message_connection_sizing`
     directly, set both `client_send_kbps` and `server_send_kbps` on
     `rocketnet_message_config_t` to that endpoint's resolved kbps so it matches
     `rn_message_object_required_bytes`.)
- **`reliable_message_timeout_ms`** (`0`, or `1000 .. 600000`; default `0`, off):
  when nonzero, disconnects a connection with
  `ROCKETNET_DISCONNECT_REASON_RELIABLE_TIMED_OUT` once the oldest reliable
  message has waited this long since its first send, outage or not. Leave it 0
  to survive outages shorter than the connection timeout
  ([surviving a network outage](USAGE.md#surviving-a-network-outage)); if you
  set it, choose a value well above the connection timeout.
- **Cross-field rules** (each refused at create with `CONFIG_OUT_OF_RANGE`,
  never clamped): `message_resend_delay_min_ms` is at most
  `message_resend_delay_max_ms`; `message_resend_delay_initial_ms` is from
  `message_resend_delay_min_ms` to `message_resend_delay_max_ms`; and a
  nonzero `reliable_message_timeout_ms` is at least four times
  `message_resend_delay_initial_ms`. At the default initial delay of `2000`
  that makes `8000` the smallest timeout, so `1000` is legal only with an
  initial delay of `250` or less.

### 6. Transports, queues, CPUs, and memory budgets (`transport`, `num_xdp_threads`, `num_os_cpus`, `ROCKETNET_*_MEMORY_BUDGET_BYTES`, `ROCKETNET_XDP_MEMORY_MIB`)

- **Transport selection (`transport`)**:
  - Leaving `transport = NULL` or `"auto"` selects the platform transport order (`xdp -> gso -> io_uring -> portable`
    on Linux server; `gso -> portable` on Linux client; `msgx -> portable` on
    macOS; production Windows servers prefer `iocp -> rio -> portable`, while
    development Windows servers prefer `portable -> iocp -> rio`).
  - Because `rocketnet_server_required_memory` and `rocketnet_client_required_memory`
    are called **before** `rocketnet_server_start` / `rocketnet_client_connect`
    probes the host, an unpinned transport order must size the caller allocator for the
    largest compiled-in candidate in that order. Which candidate is largest
    depends on the configuration: at the defaults on the Linux build of
    Table 2 it is `io_uring`, not `xdp`.
  - If a deployment will run on `gso` (for example in a container or VM
    without AF_XDP), pin `config.transport = "gso"` (or set
    `ROCKETNET_SERVER_TRANSPORT=gso`) **before** calling
    `rocketnet_server_required_memory` and `rocketnet_server_create`. At
    `max_clients = 64` (`default` profile, `ROCKETNET_DEV=1`), pinning
    `transport = "gso"` reduces `rocketnet_server_required_memory` from
    `1,910,112,962` bytes (1.78 GiB) to `1,512,642,055` bytes (1.41 GiB).
    The portable transport, a fallback rather than a production server
    transport, plans `1,293,814,523` bytes (1.20 GiB).
- **AF_XDP queues, CPU count, and compile-time cap (`num_xdp_threads`, `num_os_cpus`, `ROCKETNET_XDP_MEMORY_MIB`)**:
  - Preserve the pinned AF_XDP geometry: `ROCKETNET_XDP_FRAME_SIZE = 4096`,
    `ROCKETNET_XDP_SEND_QUEUE_SIZE = 16384`,
    `ROCKETNET_XDP_RECV_QUEUE_SIZE = 32768`,
    `ROCKETNET_XDP_FILL_FRAMES = 32768`,
    `ROCKETNET_XDP_FILL_RING_SIZE = 65536`, and
    `ROCKETNET_XDP_UMEM_FRAMES = 98304`. The UMEM is 384 MiB raw /
    `402,653,184` bytes per queue. These constants are not runtime tuning
    controls; change only the supported queue/CPU configuration below.
    The send queue is the most packets one queue starts in one update: two
    queues at 60 Hz start up to 1,966,080 packets a second, twice 10 GbE line
    rate (958.6k a second at 1304 bytes on the wire), a static assert in
    `sdk/rocketnet_constants.h`. XDP sizes its own process batch (what its
    queues drain into each update) at no fewer than twice the send queue,
    32768 packets (`ROCKETNET_XDP_MIN_RECEIVE_PACKETS_PER_UPDATE`); every
    other transport keeps the shared 16 packets per client.
  - `num_xdp_threads` (`0` selects the default, 1; public range
    `1 .. 1024`) selects the planned AF_XDP queue count, one thread a queue,
    and must equal the NIC's combined queue count. `num_os_cpus`
    (`0` selects the transport default, `1 .. 256`) selects the first worker
    CPU: worker `q` runs on `num_os_cpus + q`. Total planned AF_XDP memory is
    checked against compile-time `-DROCKETNET_XDP_MEMORY_MIB` (default `8192` MiB).
  - GSO and io_uring have their own counts, `num_gso_workers` (default 1)
    and `num_io_uring_workers` (default 2), each at most 32, and stage sends
    per queue, so more workers add send room; sends past it, disconnect
    bursts included, spread across updates as the
    [send room](TRANSPORT-INTERNALS.md#staging-and-drain-bounds)
    reports.
- **Runtime memory budgets (`ROCKETNET_CLIENT_MEMORY_BUDGET_BYTES`, `ROCKETNET_SERVER_MEMORY_BUDGET_BYTES`)**:
  - Set `ROCKETNET_CLIENT_MEMORY_BUDGET_BYTES` or
    `ROCKETNET_SERVER_MEMORY_BUDGET_BYTES` to limit transport candidates. This
    is not a hard ceiling on the endpoint/message block allocated at create,
    and it does not change the value from `rocketnet_*_required_memory`.
  - At `rocketnet_client_connect` / `rocketnet_server_start`, each candidate in
    the transport list is checked against the remaining budget, including both
    user-space transport allocator bytes and AF_XDP `umem_bytes`
    (`total_bytes = allocator_bytes + umem_bytes`). Candidates that exceed the
    budget are skipped; if no candidate in the transport order fits, connect/start
    returns `ROCKETNET_STATUS_NO_TRANSPORT_FOUND`.

---

## Deterministic memory planning and regenerated sizing examples

### Platform, build, and allocator assumptions

Tables 1–4 and the plan receipts below are queries of this source on
**Linux x86-64** (64-bit LP64, GCC 15.2.0, Release, `sizeof(void*) = 8`,
4,096-byte page size). They change when the source changes: query your own
build.
Every cell of Tables 1 and 2 is a call of the compiled public planners
(`rocketnet_client_required_memory`, `rocketnet_server_required_memory`) for
the listed profile, client count and transport, in both the development and
the production library, with no environment overrides and an all-zero accepted
packet filter; the `"loopback"` queries were given a valid one-slot hub.
The query build used `ROCKETNET_DEV=OFF` and `ROCKETNET_BUILD_DEV_LIBRARIES=ON`,
producing both libraries from the same source. Message-region
breakdowns come from `rocketnet_message_connection_sizing` after resolving the
endpoint's send budget. The `ROCKETNET_DEV=1` cells for the `default` and
`light` rows at 1,000 clients are also what the `bin/load` commands in
[Commands to regenerate](#commands-to-regenerate-planner-and-transport-figures)
print. Table 4 is `bin/transport_memory --plan-only` and
`--session-plan-only` output from the same build.
These are source-specific plans, not measured process
memory or a performance qualification. The build used the default constants:

- `ROCKETNET_MAX_CLIENTS = 1000`
- `ROCKETNET_MAX_CHANNELS = 32`
- `ROCKETNET_MAX_MESSAGE_BYTES = 1048576` (1 MiB)
- `ROCKETNET_XDP_MEMORY_MIB = 8192`
- `ROCKETNET_ALLOCATION_ALIGNMENT = 64` (cache-line alignment)
- Per-allocation tracking header overhead: `rocketnet_allocation_overhead() = 87`
  bytes (`sizeof(rocketnet_allocation_header_t) = 24` bytes +
  `ROCKETNET_ALLOCATION_ALIGNMENT - 1 = 63` bytes worst-case alignment padding)
- Linux server transports compiled in: `xdp`, `gso`, `io_uring`, `portable`,
  `loopback` (`ROCKETNET_XDP=ON`, `ROCKETNET_IO_URING=ON`).

Two build variants are shown side by side:

1. **Development build (`ROCKETNET_DEV=1`, `-DROCKETNET_DEV=ON`)**: links
   `librocketnet_dev.a` (used by `bin/load` and `bin/transport_memory`), which
   includes the network simulator and packet-capture (`pcap`) fields inside the
   endpoint and transport structs, and permits both an all-zero filter and
   the published test `packet_filter` (`b7:59:01:00`), plus development keys.
2. **Production build (`ROCKETNET_DEV=0`, `-DROCKETNET_DEV=OFF`)**: links
   `librocketnet.a`, which compiles out the simulator and `pcap` fields (saving
   `784,908` bytes on an auto-transport client and `6,389,276` bytes on a
   1,000-client auto-transport server in this build)
   while keeping `rocketnet_message_connection_bytes` identical.
   **Important production planning rule**: `rocketnet_client_required_memory`
   and `rocketnet_server_required_memory` run the same configuration validation
   as `rocketnet_client_create` and `rocketnet_server_create`. In a production
   build (`ROCKETNET_DEV=0`), leaving `config.packet_filter` at its default
   published test value (`b7:59:01:00`) without setting
   `ROCKETNET_CLIENT_PACKET_FILTER` / `ROCKETNET_SERVER_PACKET_FILTER` causes
   `rocketnet_*_required_memory` to reject the test filter and return `0`.
   An all-zero filter is accepted by this validation; it is not the default
   test filter. Always populate the application's
   non-test four-byte `packet_filter` before calling `rocketnet_*_required_memory`
   in `ROCKETNET_DEV=0` builds.

### Commands to regenerate planner and transport figures

```sh
# 1. Stock 1,000-client default and light plans via bin/load (ROCKETNET_DEV=1):
LOAD_PLAN_ONLY=1 LOAD_PROFILE=default ./bin/load 1000 1
LOAD_PLAN_ONLY=1 LOAD_PROFILE=light ./bin/load 1000 1

# 2. Pinned server transport plans via bin/load:
for t in portable gso io_uring xdp; do
  ROCKETNET_SERVER_TRANSPORT="$t" LOAD_PLAN_ONLY=1 LOAD_PROFILE=default ./bin/load 1000 1
  ROCKETNET_SERVER_TRANSPORT="$t" LOAD_PLAN_ONLY=1 LOAD_PROFILE=light ./bin/load 1000 1
done

# 3. Transport-layer allocator vs. OS/kernel resource breakdown via bin/transport_memory:
#    The second positional value is retained for command compatibility but the
#    plan always uses ROCKETNET_MAX_PACKET_PAYLOAD_BYTES. Queue count 0 is the
#    transport's default.
for t in portable gso io_uring xdp loopback; do
  ./bin/transport_memory 1000 1416 "$t" 0 --plan-only
done
./bin/transport_memory 1000 1416 xdp 7 --plan-only
# Socket buffer and XDP ring requests:
./bin/transport_memory 1000 1416 gso 0 --session-plan-only
```

`bin/load` still performs its brief local warm-up connection cycle before the
plan-only output. The public `required_memory` calls themselves start no
transport. A loopback query additionally requires a valid caller-owned hub in
the config; the command-line load tool does not supply one.

Sample `LOAD_PLAN_ONLY=1` receipts on Linux x86-64 (`ROCKETNET_DEV=1`):

```text
load_plan: clients=1000 profile=default client_public_required_bytes=22222418 server_public_required_bytes=20224798194 total_public_required_bytes=42447216194 public_memory_scope=lifecycle_allocator_peak transport_selection=unresolved_before_start term_status_code=0 reason=none verdict=PASS
load_plan: clients=1000 profile=light client_public_required_bytes=15195026 server_public_required_bytes=2147358194 total_public_required_bytes=17342384194 public_memory_scope=lifecycle_allocator_peak transport_selection=unresolved_before_start term_status_code=0 reason=none verdict=PASS
```

### Named configuration examples

In addition to `default` and `light`, the tables below include a concrete
custom game configuration (**`arena-64`**) sized for a 64-player, 60 Hz arena
match:

- `max_clients = 64`, `num_channels = 4` (`C = 4`)
- `max_message_bytes = 16384` (16 KiB), `receive_message_buffer_bytes = 65536`
  (64 KiB, `B`), `send_message_buffer_bytes = 65536` (64 KiB, `S`),
- `send_queue_messages = 256` (`Q`), `reliable_messages_in_flight = 64` (`W`),
  `packets_in_flight = 128` (`P`), `packets_per_update = 64`,
  `unreliable_messages_per_update = 64` (`Du`), `sequenced_messages_held = 8`
  (`Ru`), `max_message_receipts = 256` (`Rq`)
- `send_kbps = 1000`, `per_client_send_kbps = 1000`,
  `reliable_message_timeout_ms = 0` (the default, off; the reliable timeout
  enters no memory figure).

#### Table 1: Client `rocketnet_client_required_memory` (Linux x86-64)

`rocketnet_client_required_memory(&client_config)` equals:
`rn_client_header_bytes + rocketnet_message_client_required_memory(&mc, 0) + 87 + max_candidate_client_transport_bytes + 1,048,576` (the last term is the caller-allocator allowance). The message-object query includes the connection region and its surrounding object, connection state, and delivery views.

| Client profile | `transport` setting | Per-connection message region (`conn_bytes`) | `ROCKETNET_DEV=1` required bytes | `ROCKETNET_DEV=0` required bytes | Notes |
| --- | --- | ---: | ---: | ---: | --- |
| `default` | `NULL` / `"auto"` or `"gso"` | `7,993,408` (7.62 MiB) | `22,222,418` (21.19 MiB) | `21,437,510` (20.44 MiB) | Client `send_kbps = 1000`; automatic transport order `gso -> portable`, sized for `gso` |
| `default` | `"portable"` | `7,993,408` (7.62 MiB) | `22,073,154` (21.05 MiB) | `21,288,246` (20.30 MiB) | Single UDP socket, one client I/O worker |
| `default` | `"loopback"` | `7,993,408` (7.62 MiB) | `22,072,153` (21.05 MiB) | `21,287,245` (20.30 MiB) | Caller-owned hub and its datagram queues are additional; see the note below |
| `light` | `NULL` / `"auto"` or `"gso"` | `967,744` (0.92 MiB) | `15,195,026` (14.49 MiB) | `14,410,118` (13.74 MiB) | `max_message_bytes = 1415`, `send_kbps = 256` |
| `light` | `"portable"` | `967,744` (0.92 MiB) | `15,045,762` (14.35 MiB) | `14,260,854` (13.60 MiB) | Pinned portable UDP |
| `light` | `"loopback"` | `967,744` (0.92 MiB) | `15,044,761` (14.35 MiB) | `14,259,853` (13.60 MiB) | Caller-owned hub and its datagram queues are additional; see the note below |
| `arena-64` | `NULL` / `"auto"` or `"gso"` | `1,491,072` (1.42 MiB) | `15,718,162` (14.99 MiB) | `14,933,254` (14.24 MiB) | 4 channels, 16 KiB max msg, `W=64`, `P=128` |
| `arena-64` | `"portable"` | `1,491,072` (1.42 MiB) | `15,568,898` (14.85 MiB) | `14,783,990` (14.10 MiB) | Pinned portable UDP |

#### Table 2: Server `rocketnet_server_required_memory` across client counts and transports (Linux x86-64)

`rocketnet_server_required_memory(&server_config)` equals:
`rn_server_header_bytes + rocketnet_message_server_required_memory(&tc, &mc, 0) + 87 + max_candidate_server_transport_bytes + 1,048,576` (the last term is the caller-allocator allowance). The message-object query includes all connection regions and the surrounding object, per-connection state, and delivery views.

| Server profile | `max_clients` | `transport` setting | Per-client message region (`conn_bytes`) | `ROCKETNET_DEV=1` required bytes | `ROCKETNET_DEV=0` required bytes |
| --- | ---: | --- | ---: | ---: | ---: |
| `default` | `1` | `NULL` / `"auto"` or `"io_uring"` | `19,043,456` (18.16 MiB) | `689,626,832` (657.68 MiB) | `683,365,428` (651.71 MiB) |
| `default` | `2` | `NULL` / `"auto"` or `"io_uring"` | `19,043,456` (18.16 MiB) | `708,999,646` (676.15 MiB) | `702,738,114` (670.18 MiB) |
| `default` | `64` | `NULL` / `"auto"` or `"io_uring"` | `19,043,456` (18.16 MiB) | `1,910,112,962` (1.78 GiB) | `1,903,843,494` (1.77 GiB) |
| `default` | `64` | `"xdp"` | `19,043,456` (18.16 MiB) | `1,898,484,221` (1.77 GiB) | `1,892,214,753` (1.76 GiB) |
| `default` | `64` | `"gso"` | `19,043,456` (18.16 MiB) | `1,512,642,055` (1.41 GiB) | `1,506,372,523` (1.40 GiB) |
| `default` | `64` | `"portable"` | `19,043,456` (18.16 MiB) | `1,293,814,523` (1.20 GiB) | `1,287,545,023` (1.20 GiB) |
| `default` | `64` | `"loopback"` | `19,043,456` (18.16 MiB) | `1,284,670,611` (1.20 GiB) | `1,278,401,151` (1.19 GiB) |
| `default` | `100` | `NULL` / `"auto"` or `"io_uring"` | `19,043,456` (18.16 MiB) | `2,607,533,818` (2.43 GiB) | `2,601,259,742` (2.42 GiB) |
| `default` | `250` | `NULL` / `"auto"` or `"io_uring"` | `19,043,456` (18.16 MiB) | `5,517,539,182` (5.14 GiB) | `5,511,245,906` (5.13 GiB) |
| `default` | `500` | `NULL` / `"auto"` or `"io_uring"` | `19,043,456` (18.16 MiB) | `10,405,644,826` (9.69 GiB) | `10,399,319,550` (9.69 GiB) |
| `default` | `1000` | `NULL` / `"auto"` or `"io_uring"` | `19,043,456` (18.16 MiB) | `20,224,798,194` (18.84 GiB) | `20,218,408,918` (18.83 GiB) |
| `default` | `1000` | `"xdp"` | `19,043,456` (18.16 MiB) | `20,057,063,245` (18.68 GiB) | `20,050,673,969` (18.67 GiB) |
| `default` | `1000` | `"gso"` | `19,043,456` (18.16 MiB) | `19,671,355,319` (18.32 GiB) | `19,664,965,979` (18.31 GiB) |
| `default` | `1000` | `"portable"` | `19,043,456` (18.16 MiB) | `19,585,983,659` (18.24 GiB) | `19,579,594,351` (18.23 GiB) |
| `default` | `1000` | `"loopback"` | `19,043,456` (18.16 MiB) | `19,443,134,019` (18.11 GiB) | `19,436,744,751` (18.10 GiB) |
| `light` | `1` | `NULL` / `"auto"` or `"io_uring"` | `967,744` (0.92 MiB) | `671,549,392` (640.44 MiB) | `665,287,988` (634.47 MiB) |
| `light` | `64` | `NULL` / `"auto"` or `"io_uring"` | `967,744` (0.92 MiB) | `753,156,802` (718.27 MiB) | `746,887,334` (712.29 MiB) |
| `light` | `64` | `"xdp"` | `967,744` (0.92 MiB) | `741,528,061` (707.18 MiB) | `735,258,593` (701.20 MiB) |
| `light` | `64` | `"gso"` | `967,744` (0.92 MiB) | `355,685,895` (339.21 MiB) | `349,416,363` (333.23 MiB) |
| `light` | `64` | `"portable"` | `967,744` (0.92 MiB) | `136,858,363` (130.52 MiB) | `130,588,863` (124.54 MiB) |
| `light` | `64` | `"loopback"` | `967,744` (0.92 MiB) | `127,714,451` (121.80 MiB) | `121,444,991` (115.82 MiB) |
| `light` | `100` | `NULL` / `"auto"` or `"io_uring"` | `967,744` (0.92 MiB) | `799,789,818` (762.74 MiB) | `793,515,742` (756.76 MiB) |
| `light` | `250` | `NULL` / `"auto"` or `"io_uring"` | `967,744` (0.92 MiB) | `998,179,182` (951.94 MiB) | `991,885,906` (945.94 MiB) |
| `light` | `500` | `NULL` / `"auto"` or `"io_uring"` | `967,744` (0.92 MiB) | `1,366,924,826` (1.27 GiB) | `1,360,599,550` (1.27 GiB) |
| `light` | `1000` | `NULL` / `"auto"` or `"io_uring"` | `967,744` (0.92 MiB) | `2,147,358,194` (2.00 GiB) | `2,140,968,918` (1.99 GiB) |
| `light` | `1000` | `"xdp"` | `967,744` (0.92 MiB) | `1,979,623,245` (1.84 GiB) | `1,973,233,969` (1.84 GiB) |
| `light` | `1000` | `"gso"` | `967,744` (0.92 MiB) | `1,593,915,319` (1.48 GiB) | `1,587,525,979` (1.48 GiB) |
| `light` | `1000` | `"portable"` | `967,744` (0.92 MiB) | `1,508,543,659` (1.40 GiB) | `1,502,154,351` (1.40 GiB) |
| `light` | `1000` | `"loopback"` | `967,744` (0.92 MiB) | `1,365,694,019` (1.27 GiB) | `1,359,304,751` (1.27 GiB) |
| `arena-64` | `64` | `NULL` / `"auto"` or `"io_uring"` | `1,491,072` (1.42 MiB) | `786,637,506` (750.20 MiB) | `780,368,038` (744.22 MiB) |
| `arena-64` | `64` | `"gso"` | `1,491,072` (1.42 MiB) | `389,166,599` (371.14 MiB) | `382,897,067` (365.16 MiB) |
| `arena-64` | `64` | `"portable"` | `1,491,072` (1.42 MiB) | `170,339,067` (162.45 MiB) | `164,069,567` (156.47 MiB) |

For `loopback`, these are endpoint plans only: the separately allocated,
caller-owned hub and all its datagram queues must be budgeted once in addition.
The queries supplied a valid one-slot hub only to satisfy the planner's hub
identity check; no endpoint was started. Hub capacity does not enter either
endpoint query. An actual run needs a hub sized for its client count.

#### Table 3: Per-connection message region sub-allocation (`rocketnet_message_connection_sizing`)

Each connection's message region is sub-allocated from a single 64-byte-aligned
block (`sdk/rocketnet_allocator.cpp`, `docs/CHANNELS.md` section 14):

```text
connection_bytes = 88C + 112.125W + B + 88Q + S + 128P + 24(W + Du) + 1440Ru + 16Rq + SR
```

The `88Q` term is `72Q` send descriptors plus the `16Q` unreliable ring.
`Q`, `S`, and `SR` include the resend-delay payload reserve derived from
`send_kbps` / `per_client_send_kbps`. `S_eff` also holds two 4-byte words a
store chunk, its metadata word and its free-stack entry:

| Sub-region | Formula | `default` server (`C=32, W=1024, P=1024`) | `light` server (`C=8, W=256, P=256`) | `arena-64` (`C=4, W=64, P=128`) |
| --- | --- | ---: | ---: | ---: |
| Channel send state | `64 * C` | `2,048` | `512` | `256` |
| Channel receive state | `24 * C` | `768` | `192` | `96` |
| Sender per-number table | `32 * W` | `32,768` | `8,192` | `2,048` |
| Sender first-send times | `8 * W` | `8,192` | `2,048` | `512` |
| Receiver descriptors | `32 * W` | `32,768` | `8,192` | `2,048` |
| Received bitset | `W / 8` | `128` | `32` | `8` |
| Held-ordered index | `16 * 2 * W` | `32,768` | `8,192` | `2,048` |
| Receive ring | `B` | `1,048,576` | `142,400` | `65,536` |
| Send descriptors | `72 * Q_eff` | `1,480,032` | `45,216` | `159,120` |
| Send store (`128`/`512`/`1152` slabs) | `S_eff` | `12,279,808` | `348,224` | `869,440` |
| Sent-packet ring | `32 * P` | `32,768` | `8,192` | `4,096` |
| Sent-packet index | `16 * 2 * P` | `32,768` | `8,192` | `4,096` |
| Sent-packet references | `4 * 16 * P` | `65,536` | `16,384` | `8,192` |
| Delivered entries | `24 * (W + Du)` | `49,152` | `6,240` | `3,072` |
| Sequenced holds | `1,440 * Ru` | `23,040` | `144,000` | `11,520` |
| Receipt ring | `16 * Rq` | `4,096` | `2,048` | `4,096` |
| Resend FIFO | `8 * W` | `8,192` | `2,048` | `512` |
| Unreliable ring | `8 * 2 * Q_eff` | `328,896` | `10,048` | `35,360` |
| Send spans | `SR` | `3,581,120` | `207,296` | `318,848` |
| Alignment padding | to 64 B | `32` | `96` | `168` |
| **Total `connection_bytes`** | — | **`19,043,456` (18.16 MiB)** | **`967,744` (0.92 MiB)** | **`1,491,072` (1.42 MiB)** |

#### Sizing the send side for an outage

While the other side receives nothing, every message a game starts waits in
the send region: the reliable window, the sent-packet ring and the send budget
hold messages back but never refuse a start. To keep starting messages through
an outage shorter than the connection timeout T (the connect token's
`timeout_seconds`) without a refusal, size the send side for the send rate
times T: `Q` at least the messages started per second times T, and `S` (with
`SR` for messages above one 1,152-byte chunk) at least what those messages take
in their slab classes over T. The light profile is sized this way for its
workload: 10 messages per second over the default 10 s timeout is 100
messages, inside its `Q` of 128. Past that size a start is refused
(`start_message` returns `NULL` and the connection's `send_refused` counts it)
and the connection stays up; it is not ended. See
[surviving a network outage](USAGE.md#surviving-a-network-outage) for what
the connection keeps through an outage and
[refused sends](ERRORS-AND-LOGGING.md#a-game-policy-for-a-client-it-cannot-send-to)
for a game policy on refusals.

### Deterministic planner values vs. measured OS and process memory

`rocketnet_client_required_memory` and `rocketnet_server_required_memory`
report **deterministic worst-case user-space allocator peaks** routed through
`rocketnet_allocator_t`. They differ from Operating System and process memory
metrics (`VmRSS`, `RssAnon`, `/proc/meminfo`, `getrusage(RUSAGE_SELF)`) in two
directions:

1. **OS and kernel memory outside `rocketnet_allocator_t` (why total host memory can exceed `required_memory`)**:
   - **Worker thread stacks**: Each socket worker thread reserves an OS stack
     (`8 MiB = 8,388,608` bytes default on Linux x86-64, one per network
     thread at the defaults: 2 threads = `16 MiB` for `portable`, 1 = `8 MiB`
     for `gso`, 2 = `16 MiB` for `io_uring`, 1 = `8 MiB` for `xdp`, 0 for
     `loopback`).
   - **AF_XDP UMEM and kernel rings**: Each AF_XDP queue allocates a 98,304-frame,
     page-aligned `402,653,184`-byte (384 MiB raw) UMEM buffer via `mmap` plus
     kernel RX/TX rings (`786,432` bytes of descriptors per queue;
     `2,818,572,288` bytes UMEM + `5,505,024` bytes of RX and TX descriptors
     across 7 queues, `num_xdp_threads = 7`, and a seventh of each at the
     default of 1; the fill and completion rings are not in that figure). UMEM is counted against `ROCKETNET_SERVER_MEMORY_BUDGET_BYTES`
     and `-DROCKETNET_XDP_MEMORY_MIB`, but is not allocated from
     `rocketnet_allocator_t`.
   - **`io_uring` mapped rings**: Each queue has its own worker thread,
     socket and ring (submission and completion), plus a provided-buffer
     ring. The planner's `mapped_ring_bytes` counts, for each queue, the
     provided-buffer ring in whole host pages and a bound on the submission
     and completion ring mappings (`rocketnet_server_uring_mapped_bytes`):
     `991,232` bytes at the default two queues and `1,982,464` at four, with
     4 KiB pages.
   - **Kernel socket buffers (`SO_SNDBUF` / `SO_RCVBUF`)**: UDP send requests
     are `1,000,000` bytes per socket. Receive requests scale with runtime
     `max_clients` on every transport, each GSO and io_uring queue socket
     included (25,000,000 per socket at 1000 clients). The kernel may grant less. XDP uses UMEM and its
     rings instead of these UDP socket queues. See [Receive buffer sizing](MEMORY.md#receive-buffer-sizing).
2. **Demand-paged physical RSS vs. reserved virtual arenas (why measured RSS at startup or low load can be much lower than `required_memory`)**:
   - `rocketnet_server_create` and `rocketnet_client_create` allocate the
     endpoint's message block; `rocketnet_server_start` and
     `rocketnet_client_connect` allocate the transport. No `malloc` occurs on
     the hot path after that. Large uncarved regions and the receive batch
     (`rocketnet_message_batch_t`, about 1.5 MiB: 65,536 entries of four
     4-byte fields and one pointer) stay untouched until messages actually
     occupy those slots.
   - The Linux kernel only faults physical pages into Resident Set Size (RSS)
     when bytes are written. An earlier recorded Linux x86-64 run of
     `./bin/transport_memory 1000 1416 portable` allocated
     `live_allocator_bytes = 495,178,594` (472.24 MiB) of transport arenas at
     startup, while `process_peak_rss_bytes` after `create` and `start` is only
     `53,215,232` bytes (50.75 MiB) until clients connect and fill their
     message rings. Capacity planning must size host RAM for the
     touched working set under peak load plus the OS/kernel resources below,
     never from idle startup RSS.

#### Table 4: Server transport-layer allocator and OS breakdown at `max_clients = 1000` (`./bin/transport_memory`, Linux x86-64, `ROCKETNET_DEV=1`)

The values below are `./bin/transport_memory 1000 1416 <transport> <queues> --plan-only`
at each transport's default count (queue argument 0), plus an `xdp` row at 7
queues; the socket column is the `--session-plan-only` request. The second
positional value is ignored by this inventory tool; every row plans the fixed
`ROCKETNET_MAX_PACKET_PAYLOAD_BYTES` payload ceiling. Every UDP receive request
comes from the client-count formula above. Run both options with the intended
configuration for your own figures.

| Server transport | `allocator_bytes` | `umem_bytes` (page-aligned) | `os_bytes` (UMEM + stacks + mapped rings) | Worker threads (`stack_bytes`) | Sockets (`SO_SNDBUF` + `SO_RCVBUF` request) |
| --- | ---: | ---: | ---: | ---: | ---: |
| `portable` | `530,923,668` (506.33 MiB) | `0` | `16,777,216` (16.00 MiB) | `2` (`16,777,216`) | `1` (`1,000,000` + `25,000,000`) |
| `gso` (1 worker) | `616,295,328` (587.75 MiB) | `0` | `8,388,608` (8.00 MiB) | `1` (`8,388,608`) | `1` (`1,000,000` + `25,000,000`) |
| `io_uring` (2 workers) | `1,169,738,203` (1.09 GiB) | `0` | `17,768,448` (16.95 MiB) | `2` (`16,777,216`) | `2` (`2,000,000` + `50,000,000`) |
| `xdp` (1 queue) | `1,002,003,254` (955.58 MiB) | `402,653,184` (384.00 MiB) | `411,041,792` (392.00 MiB) | `1` (`8,388,608`) | `1` (`786,432` XDP rings) |
| `xdp` (7 queues) | `4,671,870,704` (4.35 GiB) | `2,818,572,288` (2.63 GiB) | `2,877,292,544` (2.68 GiB) | `7` (`58,720,256`) | `7` (`5,505,024` XDP rings) |
| `loopback` | `388,074,028` (370.10 MiB) | `0` | `0` | `0` (`0`) | `0` |
