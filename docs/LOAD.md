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
one 1,415-byte message (`ROCKETNET_MAX_PACKET_PAYLOAD_BYTES - 1`, which fits one
1,500-byte MTU packet) per 100 ms in each direction at 10 Hz. It does not
change public defaults; the `default` profile remains the stock acceptance.

## Low-bandwidth population ladder

The requested ladder is 10, 100, 250, 500 and 1,000 clients. At each rung,
offer one 32-byte application message per client per second in each direction
with the fixed-rate option below. For the stock 1,000-client acceptance, run
the `default` profile so the result covers SDK-default sizing. The `light`
profile is a separate, additional bounded-memory workload; it cannot substitute
for the default-profile acceptance. The current `bin/load` harness is capped
at `ROCKETNET_MAX_CLIENTS` (1,000) and records no ladder results (below). The
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
count or validated-above-1000 guarantee is available.

For every rung, require each client's maximum wire rate to stay strictly below
100 kbps in both directions, and compare aggregate traffic in each direction
with that direction's available link rate. Record the exact source head,
population, profile, transports and receipt. Report the highest population that
passed all functional and rate criteria, the first population with a functional
failure, and any higher population blocked by a capacity guard as `UNRUN`.
A capacity refusal is not a functional failure, and an unrun rung has no pass
result. Product documentation records no current ladder results.

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
as the intended run to get a `load_plan:` receipt without creating endpoints
or binding a socket. It reports `client_public_required_bytes`,
`server_public_required_bytes` and checked
`total_public_required_bytes = server + clients * client`. A zero required
size, arithmetic overflow or failed SDK termination returns a FAIL receipt and
nonzero exit. Required-memory figures bound allocator memory over the
lifecycle for the largest planned built-in transport in the configured ladder,
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
default-profile clients in one `bin/load` process. That guest must hold the
whole run, including operating-system-owned memory outside the lifecycle
allocator plan. Historical create-only measurements reported
`total_public_required_bytes=18214470336` (16.96 GiB: 13.32 GB server, 4.90 MB
per client), with the xdp server's start adding about 1.87 GB of allocator
memory and 1.88 GB of UMEM. The current lifecycle plan includes the allocator
portion; do not add it a second time. In those historical runs a fixed 16G
guest was OOM-killed (exit 137,
anon-rss 15.9 GB) and a 24G guest was killed at the shutdown wipe. The step
runs `LOAD_PLAN_ONLY=1 ./bin/load 1000 60` with the cell's environment first
and sizes the guest from the plan plus conservative operational headroom: `need = plan + 1000 * reserve_per_client_bytes`, and the guest is `need * 5 / 4 + 1 GiB` (the
1 GiB is the guest kernel's own), rounded up to whole GiB. It prints `LOAD cell=xdp FAIL runner has ... MiB available` when the runner's `MemAvailable` is
below that. The guest's load exit status, peak RSS, `/proc/meminfo` totals and
`dmesg | tail -n 50` go into `vm.log` on every run, so an early end names its
cause, and the step prints `guest_peak_rss_kib` beside `need_bytes`, with a
WARN line when the peak passes the need.

The historical reserve was measured against the create-only plan in an
unconstrained 48G guest on a
121 GB runner: peak RSS 27397952 kB at 1,000 clients and 13703716 kB at 500,
against plans of 18214470336 and 9108290368 bytes. That is 9841033 and 9848630
bytes per client; the step retains the larger as conservative operational
headroom. It is not a measured residual above the current lifecycle plan:
part of that historical difference is transport allocator memory now counted
in the plan. The peak is 1.54 times
the plan at both counts, so an earlier `plan * 3 / 2 + 2 GiB` sizing sat below
the peak in its ratio term and passed only on its 2 GiB constant (28G guest,
27.4 GiB `MemTotal`, 1.3 GiB over the 26.1 GiB peak). For that historical default-profile plan, the need was 28063100336 bytes
(26.1 GiB) and the guest was 34G, about 7.9 GiB above the measured peak.
Current guest sizing uses the current plan and therefore differs. Measure
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
refuses an offered message.

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
line rate, so a scaling ladder must compare its aggregate measured traffic
with the actual host link capacity separately. The fixed-rate workload has
`timing_baseline=fixed_rate_v1`; compare its frame timings only with runs that
share rate, population, transport and measurement scope.

The light profile is the SDK's public light-client geometry: eight channels, a
256-message in-flight reliability window, a 142,400-byte receive buffer, a
1415-byte (`ROCKETNET_MAX_PACKET_PAYLOAD_BYTES - 1`) maximum message, a
141,500-byte send buffer, a 128-message send queue, 256 packets in flight, 4
packets and 4 unreliable messages per update, 100 sequenced messages held, 128
receipts, and one reassembly slot (`max_fragmented_messages` is 1). Its
256-kbps per-client send budget is 2.13 times the roughly 120-kbps wire rate
of one 1,500-byte packet per 100 ms in each direction (10 * 1,500 * 8 =
120,000 bits/s). The reliable timeout stays at its 10,000 ms default. At
`max_clients` 1000 the server's `rocketnet_server_required_memory` is
3,238,578,316 bytes, and `rocketnet_client_required_memory` for the client
profile is 22,890,053 bytes (Linux x86-64, development build, io_uring and
XDP built; each is the lifecycle peak the caller's allocator sees, for the
largest transport the ladder can grant). Re-measured 2026-10-27 on hetzner2
at `ad91bdfa`, RelWithDebInfo, clang++ 18.1.3, `ROCKETNET_DEV=ON`. The previous
development figures were 3,231,507,666 and 22,896,245. Generation, owner epoch,
enqueue ticket and the hole bound of 64 stayed. The non-development column was
not re-measured: `tests/test_unit_light_profile.cpp` is not linked when
`ROCKETNET_DEV=OFF`. On Windows x64 with MSVC, the
server/client figures are 1,591,206,044 / 22,968,879 bytes in development builds
and 1,584,809,680 / 22,183,075 bytes with `ROCKETNET_DEV=OFF`.
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
client, term; `load_census.h`): no thread or handle may survive post-init to
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
- receipt and drop counters reconcile with attempted sends;
- a refused `start_message` or `finish_message` is counted and fails the
  corresponding offered-load criterion.

Short 10- and 100-client cases may be used as integration gates. The
1,000-client case, throughput measurements, profiler samples and long soak
remain hardening work and require an allocated bench plus a complete raw
receipt. The harness emits call timings, but they are not a performance claim
until the required Release runs are reviewed.

## Choosing configuration for concrete game workloads

Neither `rocketnet_*_default_config` nor `rocketnet_*_light_config` is a
universal preset suitable for every game:

- **`rocketnet_client_default_config` / `rocketnet_server_default_config`**
  reserves general-purpose headroom across every dimension: 32 channels, a
  1 MiB (`1,048,576`-byte) maximum message and receive
  ring, a 4 MiB (`4,194,304`-byte) send buffer, 8 concurrent fragment
  reassembly slots per connection, a 1,024-message reliable window, 1,024
  packets in flight, 256 packets and 1,024 unreliable messages per update,
  a 1,000-kbps client send budget, and a 10,000-kbps server per-client send
  budget. The server budget produces an `18,383,616`-byte (17.53 MiB) message
  region per connection and a `20,908,851,166`-byte
  (19.47 GiB, `ROCKETNET_DEV=1`) worst-case server allocator footprint at
  `max_clients = 1000`. A game with small messages, fewer channels, or smaller
  match sizes pays for unused per-client arena space if it leaves every field
  at `0` (default).
- **`rocketnet_client_light_config` / `rocketnet_server_light_config`** is
  sized specifically for a 10 Hz (100 ms) update cadence carrying at most one
  unfragmented 1,415-byte message (`ROCKETNET_MAX_PACKET_PAYLOAD_BYTES - 1`)
  every 100 ms in each direction across 8 channels, with `max_fragmented_messages = 1`,
  `packets_per_update = 4`, `unreliable_messages_per_update = 4`,
  `send_queue_messages = 128`, `reliable_messages_in_flight = 256`, and a
  256-kbps server `per_client_send_kbps` budget. That cuts the server's
  per-client message region to `947,776` bytes (0.90 MiB), but any game that
  sends messages larger than 1,415 bytes, emits more than 4 unreliable messages
  or 4 packets per `update` tick, or bursts above 256 kbps per client will hit
  a refused send, `ROCKETNET_CLIENT_STATE_MESSAGE_MEMORY_EXHAUSTED` /
  `ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED`, or
  `messages_dropped_over_budget`.

Size a game's configuration by mapping its concrete simulation and traffic
requirements to each configuration group:

### 1. Client capacity (`max_clients`, `ROCKETNET_MAX_CLIENTS`)

- **Runtime `max_clients`** (`rocketnet_server_config_t.max_clients`,
  environment `ROCKETNET_SERVER_MAX_CLIENTS`, range `1 .. ROCKETNET_MAX_CLIENTS`,
  where `0` resolves to `min(ROCKETNET_MAX_CLIENTS, 1000)`): scales both the
  message-layer connection array (`max_clients * connection_bytes`) and
  per-client transport state (session slots, challenge/encryption/replay
  tables, and fragment reassembly pools).
- **Tradeoff**: Set `max_clients` to the actual maximum player count of the
  server instance (for example `16` for a squad server, `64` for an arena
  match, or `100` for a survival match). Lowering `max_clients` from `1000` to
  `64` on the `default` profile reduces `rocketnet_server_required_memory` from
  `20,908,851,166` bytes (19.47 GiB) to `3,093,203,134` bytes (2.88 GiB) with
  unpinned `transport = NULL` (`"auto"`), or to `1,493,745,440` bytes
  (1.39 GiB) with `transport = "gso"`.
- **Compile-time `ROCKETNET_MAX_CLIENTS`**: Fixed-size arrays inside endpoint
  and transport headers are dimensioned by `ROCKETNET_MAX_CLIENTS` (default
  `1000`). If a build never hosts more than `N` clients, compiling with
  `-DROCKETNET_MAX_CLIENTS=N` shrinks those static header tables as well.
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
  `1 .. ROCKETNET_MAX_CHANNELS`, default `32`, light `8`): each channel owns
  independent ordered (`ROCKETNET_RELIABILITY_RELIABLE_ORDERED*`) and sequenced
  (`ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED*`) state.
- **Tradeoff**: Head-of-line blocking on `RELIABLE_ORDERED` is scoped to a
  single channel. Separate unrelated streams onto distinct channels (for
  example: channel 0 for session control and RPCs, channel 1 for high-rate
  entity state, channel 2 for combat/inventory events, channel 3 for chat or
  telemetry) so packet loss on one ordered stream never stalls another. Per-channel
  state is only `56 * C` bytes per connection (`32 * C` send + `24 * C`
  receive), so channel count has negligible memory cost compared to
  arenas and windows, but `client` and `server` must configure the same
  `num_channels` or the connection ends in
  `ROCKETNET_CLIENT_STATE_CONFIG_MISMATCH` /
  `ROCKETNET_DISCONNECT_REASON_CONFIG_MISMATCH`.

### 3. Message size, arenas, and fragment reassembly (`max_message_bytes`, `receive_message_buffer_bytes`, `send_message_buffer_bytes`, `max_fragmented_messages`, `ROCKETNET_REASSEMBLY_MEMORY_MIB`)

- **Single-packet vs. fragmented messages**: `max_message_bytes` is the public
  ceiling for every reliability mode, from
  `ROCKETNET_MAX_PACKET_PAYLOAD_BYTES - 1` (`1415`) through
  `ROCKETNET_MAX_MESSAGE_BYTES` (`1,048,576`). Exact one-packet limits depend
  on the reliability form's headers; larger messages use the message layer's
  bounded reliable-fragment path. This public setting does not resize the
  transport layer's separate fixed-payload reassembly arena.
- **Validation invariants**:
  - `1415 <= max_message_bytes <= ROCKETNET_MAX_MESSAGE_BYTES` (`1,048,576`).
  - `receive_message_buffer_bytes` is a multiple of 16 in `4096 .. 1048576`,
    and `max_message_bytes <= receive_message_buffer_bytes`.
  - `12288 <= send_message_buffer_bytes <= 268435456`.
  - The send buffer's large-slab share must hold all 1,152-byte fragments of
    one `max_message_bytes` block, and that fragment count must not exceed
    `reliable_messages_in_flight`.
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
  separate `reliable_message_timeout_ms` default is 10,000 ms.
- **Transport fragment reassembly (`max_fragmented_messages`, compile-time `ROCKETNET_REASSEMBLY_MEMORY_MIB`)**:
  `max_fragmented_messages` (`1 .. 256`, where `0` selects the default `8`
  slots and `light` sets `1`) bounds how many multi-packet messages a single
  remote endpoint can have partially reassembled at the same time. Reassembly
  buffers live in the transport layer. Their public-plan payload ceiling is
  the transport's fixed `ROCKETNET_TRANSPORT_MAX_PAYLOAD_BYTES`, not the
  message-layer `max_message_bytes`; lowering only `max_message_bytes` does not
  reduce this transport allocation. At `max_clients = 1000`, 8 slots reserve
  `296,688,452` bytes (282.94 MiB), while 1 slot reserves `56,976,952` bytes
  (54.34 MiB). Total server reassembly memory is checked at
  `rocketnet_server_create` against `-DROCKETNET_REASSEMBLY_MEMORY_MIB`
  (default `512` MiB); exceeding that cap refuses creation with
  `ROCKETNET_STATUS_MEMORY_BUDGET_EXCEEDED`.

### 4. Queues, reliable windows, per-update caps, sequenced holds, and receipts (`send_queue_messages`, `reliable_messages_in_flight`, `packets_in_flight`, `packets_per_update`, `unreliable_messages_per_update`, `sequenced_messages_held`, `max_message_receipts`)

| Field | Symbol | Range | Default | Light | Sizing rule and tradeoff |
| --- | --- | --- | --- | --- | --- |
| `send_queue_messages` | `Q` | `16 .. 1048576` | `1024` | `128` | Maximum messages queued for transmission before/during send pacing (`64` bytes per descriptor plus resend-delay reserve). Increase if a frame queues many small messages before `update`. |
| `reliable_messages_in_flight` | `W` | power of two, `64 .. 1048576` | `1024` | `256` | Sliding window of unacknowledged reliable messages (`120.125 * W` bytes plus delivered tracking). Size `W >= peak_reliable_msg_rate * RTT_sec * 2` so normal RTT never blocks reliable sends. |
| `packets_in_flight` | `P` | `64 .. 1048576`, and `P >= W` | `1024` | `256` | Sent-packet tracking ring (`128 * P` bytes). If `P < W`, create returns `ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE`. For high tick rates with unreliable traffic alongside reliable messages, set `P > W` (e.g. `2 * W`). |
| `packets_per_update` | — | `1 .. 1024` | `256` | `4` | Maximum UDP packets emitted per connection in a single `update` call. Must cover peak per-tick packet bursts (including fragments and retransmits). |
| `unreliable_messages_per_update` | `Du` | `1 .. 65536` | `1024` | `4` | Maximum unreliable/sequenced messages accepted and delivered per connection per `update` (`24 * (W + Du)` bytes in delivered table). Must exceed the peak unreliable messages sent or received in one frame. |
| `sequenced_messages_held` | `Ru` | `0 .. 4096` | `16` | `100` | Slots holding the latest sequenced message per channel (`1,440 * Ru` bytes). Needs at least as many slots as active sequenced channels (`Ru >= active_sequenced_channels`). |
| `max_message_receipts` | `Rq` | `16 .. 65536` | `256` | `128` | Receipt ring capacity (`16 * Rq` bytes) for `*WITH_RECEIPT` modes drained via the receipt APIs. Size to the maximum receipt events that can complete between two drains. |

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
     `connection_bytes` from `18,383,616` bytes (17.53 MiB) to `947,776`
     bytes (0.90 MiB). (When calling the internal layer helper
     `rocketnet_message_connection_bytes` or `rocketnet_message_connection_sizing`
     directly, set both `client_send_kbps` and `server_send_kbps` on
     `rocketnet_message_config_t` to that endpoint's resolved kbps so it matches
     `rn_message_object_required_bytes`.)
- **`reliable_message_timeout_ms`** (range `1000 .. 600000`, default `10000`): disconnects
  a connection with `ROCKETNET_DISCONNECT_REASON_RELIABLE_TIMED_OUT` when a
  reliable message remains unacknowledged past this deadline.

### 6. Transports, queues, CPUs, and memory budgets (`transport`, `num_xdp_queues`, `num_os_cpus`, `ROCKETNET_*_MEMORY_BUDGET_BYTES`, `ROCKETNET_XDP_MEMORY_MIB`)

- **Transport selection (`transport`)**:
  - Leaving `transport = NULL` or `"auto"` selects the platform ladder (`xdp -> gso -> io_uring -> portable`
    on Linux server; `gso -> portable` on Linux client; `msgx -> portable` on
    macOS; production Windows servers prefer `iocp -> rio -> portable`, while
    development Windows servers prefer `portable -> iocp -> rio`).
  - Because `rocketnet_server_required_memory` and `rocketnet_client_required_memory`
    are called **before** `rocketnet_server_start` / `rocketnet_client_connect`
    probes the host, an unpinned ladder must size the caller allocator for the
    largest compiled-in candidate in that ladder (`xdp` on a Linux server build
    with `ROCKETNET_XDP=ON`).
  - If a deployment will run on `gso` or `portable` (for example in a container
    or VM without AF_XDP), pin `config.transport = "gso"` or `"portable"` (or
    set `ROCKETNET_SERVER_TRANSPORT=gso`) **before** calling
    `rocketnet_server_required_memory` and `rocketnet_server_create`. At
    `max_clients = 64` (`default` profile, `ROCKETNET_DEV=1`), pinning
    `transport = "gso"` reduces `rocketnet_server_required_memory` from
    `3,093,203,134` bytes (2.88 GiB) to `1,493,745,440` bytes (1.39 GiB), and
    pinning `transport = "portable"` reduces it to `1,319,195,936` bytes
    (1.23 GiB).
- **AF_XDP queues, CPU count, and compile-time cap (`num_xdp_queues`, `num_os_cpus`, `ROCKETNET_XDP_MEMORY_MIB`)**:
  - Preserve the pinned AF_XDP geometry: `ROCKETNET_XDP_FRAME_SIZE = 4096`,
    `ROCKETNET_XDP_SEND_QUEUE_SIZE = 4096`,
    `ROCKETNET_XDP_RECV_QUEUE_SIZE = 16384`,
    `ROCKETNET_XDP_FILL_FRAMES = 16384`,
    `ROCKETNET_XDP_FILL_RING_SIZE = 32768`, and
    `ROCKETNET_XDP_UMEM_FRAMES = 49152`. The UMEM is 192 MiB raw /
    `201,326,592` bytes per queue. These constants are not runtime tuning
    controls; change only the supported queue/CPU configuration below.
  - `num_xdp_queues` (`0` selects the transport default, `1 .. 1024`) and
    `num_os_cpus` (`0` selects the transport default, `1 .. 256`) control the planned AF_XDP queue
    count and per-CPU BPF map sizing. Total planned AF_XDP memory is checked
    against compile-time `-DROCKETNET_XDP_MEMORY_MIB` (default `4096` MiB).
- **Runtime memory budgets (`ROCKETNET_CLIENT_MEMORY_BUDGET_BYTES`, `ROCKETNET_SERVER_MEMORY_BUDGET_BYTES`)**:
  - Set `ROCKETNET_CLIENT_MEMORY_BUDGET_BYTES` or
    `ROCKETNET_SERVER_MEMORY_BUDGET_BYTES` to limit transport candidates. This
    is not a hard ceiling on the endpoint/message block allocated at create,
    and it does not change the value from `rocketnet_*_required_memory`.
  - At `rocketnet_client_connect` / `rocketnet_server_start`, each candidate in
    the transport ladder is checked against the remaining budget, including both
    user-space transport allocator bytes and AF_XDP `umem_bytes`
    (`total_bytes = allocator_bytes + umem_bytes`). Candidates that exceed the
    budget are skipped; if no candidate in the ladder fits, connect/start
    returns `ROCKETNET_STATUS_TRANSPORT_LADDER_EXHAUSTED`.

---

## Deterministic memory planning and regenerated sizing examples

### Platform, build, and allocator assumptions

All deterministic byte figures below were generated on **Linux x86-64**
(64-bit LP64, `sizeof(void*) = 8`, 4,096-byte page size) with the default
compile-time constants:

- `ROCKETNET_MAX_CLIENTS = 1000`
- `ROCKETNET_MAX_CHANNELS = 32`
- `ROCKETNET_MAX_MESSAGE_BYTES = 1048576` (1 MiB)
- `ROCKETNET_REASSEMBLY_MEMORY_MIB = 512`
- `ROCKETNET_XDP_MEMORY_MIB = 4096`
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
   endpoint and transport structs, and permits the all-zero / published test
   four-byte `packet_filter` (`00:01:02:03`) and development keys.
2. **Production build (`ROCKETNET_DEV=0`, `-DROCKETNET_DEV=OFF`)**: links
   `librocketnet.a`, which compiles out the simulator and `pcap` fields (saving
   `785,804` bytes on a client and `6,396,372` bytes on a 1,000-client server)
   while keeping `rocketnet_message_connection_bytes` identical.
   **Important production planning rule**: `rocketnet_client_required_memory`
   and `rocketnet_server_required_memory` run the same configuration validation
   as `rocketnet_client_create` and `rocketnet_server_create`. In a production
   build (`ROCKETNET_DEV=0`), leaving `config.packet_filter` at all zeros (or
   set to `00:01:02:03`) without setting `ROCKETNET_CLIENT_PACKET_FILTER` /
   `ROCKETNET_SERVER_PACKET_FILTER` causes `rocketnet_*_required_memory` to
   reject the test filter and return `0`. Always populate the application's
   non-test four-byte `packet_filter` before calling `rocketnet_*_required_memory`
   in `ROCKETNET_DEV=0` builds.

### Commands to regenerate planner and transport figures

```sh
# 1. Stock 1,000-client default and light plans via bin/load (ROCKETNET_DEV=1):
LOAD_PLAN_ONLY=1 LOAD_PROFILE=default ./bin/load 1000 1
LOAD_PLAN_ONLY=1 LOAD_PROFILE=light ./bin/load 1000 1

# 2. Pinned server transport plans via bin/load:
for t in portable gso io_uring xdp loopback; do
  ROCKETNET_SERVER_TRANSPORT="$t" LOAD_PLAN_ONLY=1 LOAD_PROFILE=default ./bin/load 1000 1
  ROCKETNET_SERVER_TRANSPORT="$t" LOAD_PLAN_ONLY=1 LOAD_PROFILE=light ./bin/load 1000 1
done

# 3. Transport-layer allocator vs. OS/kernel resource breakdown via bin/transport_memory:
#    The second positional value is retained for command compatibility but the
#    plan always uses ROCKETNET_TRANSPORT_MAX_PAYLOAD_BYTES.
./bin/transport_memory 1000 1416 0 portable
./bin/transport_memory 1000 1415 1 portable
```

Sample `LOAD_PLAN_ONLY=1` receipts on Linux x86-64 (`ROCKETNET_DEV=1`):

```text
load_plan: clients=1000 profile=default client_public_required_bytes=29992800 server_public_required_bytes=20908851166 total_public_required_bytes=50901651166 public_memory_scope=lifecycle_allocator_peak transport_selection=unresolved_before_start term_status_code=0 reason=none verdict=PASS
load_plan: clients=1000 profile=light client_public_required_bytes=22890053 server_public_required_bytes=3238578316 total_public_required_bytes=26128631316 public_memory_scope=lifecycle_allocator_peak transport_selection=unresolved_before_start term_status_code=0 reason=none verdict=PASS
```

### Named configuration examples

In addition to `default` and `light`, the tables below include a concrete
custom game configuration (**`arena-64`**) sized for a 64-player, 60 Hz arena
match:

- `max_clients = 64`, `num_channels = 4` (`C = 4`)
- `max_message_bytes = 16384` (16 KiB), `receive_message_buffer_bytes = 65536`
  (64 KiB, `B`), `send_message_buffer_bytes = 65536` (64 KiB, `S`),
  `max_fragmented_messages = 2`
- `send_queue_messages = 256` (`Q`), `reliable_messages_in_flight = 64` (`W`),
  `packets_in_flight = 128` (`P`), `packets_per_update = 64`,
  `unreliable_messages_per_update = 64` (`Du`), `sequenced_messages_held = 8`
  (`Ru`), `max_message_receipts = 256` (`Rq`)
- `send_kbps = 1000`, `per_client_send_kbps = 1000`,
  `reliable_message_timeout_ms = 10000`.

#### Table 1: Client `rocketnet_client_required_memory` (Linux x86-64)

`rocketnet_client_required_memory(&client_config)` equals:
`rn_client_header_bytes + rocketnet_message_connection_bytes(&mc) + 87 + max_candidate_client_transport_bytes`.

| Client profile | `transport` setting | Per-connection message region (`conn_bytes`) | `ROCKETNET_DEV=1` required bytes | `ROCKETNET_DEV=0` required bytes | Notes |
| --- | --- | ---: | ---: | ---: | --- |
| `default` | `NULL` / `"auto"` or `"gso"` | `7,852,032` (7.49 MiB) | `29,992,800` (28.60 MiB) | `29,206,996` (27.85 MiB) | Client `send_kbps = 1000`; auto ladder `gso -> portable`, sized for `gso` (`20,022,665` dev transport bytes) |
| `default` | `"portable"` | `7,852,032` (7.49 MiB) | `29,842,576` (28.46 MiB) | `29,056,772` (27.71 MiB) | Single UDP socket, 2 worker threads |
| `default` | `"loopback"` | `7,852,032` (7.49 MiB) | `32,864,566` (31.34 MiB) | `32,078,762` (30.59 MiB) | In-process queue rings (`4096` packets each way) |
| `light` | `NULL` / `"auto"` or `"gso"` | `947,776` (0.90 MiB) | `22,890,053` (21.83 MiB) | `22,110,441` (21.09 MiB) | `max_message_bytes = 1415`, `max_fragmented_messages = 1`, `send_kbps = 256` |
| `light` | `"portable"` | `947,776` (0.90 MiB) | `22,746,021` (21.69 MiB) | `21,960,217` (20.94 MiB) | Pinned portable UDP |
| `light` | `"loopback"` | `947,776` (0.90 MiB) | `25,768,011` (24.57 MiB) | `24,982,207` (23.82 MiB) | In-process loopback |
| `arena-64` | `NULL` / `"auto"` or `"gso"` | `1,426,240` (1.36 MiB) | `23,408,754` (22.32 MiB) | `22,622,950` (21.57 MiB) | 4 channels, 16 KiB max msg, 2 fragment slots, `W=64`, `P=128` |
| `arena-64` | `"portable"` | `1,426,240` (1.36 MiB) | `23,258,530` (22.18 MiB) | `22,472,726` (21.43 MiB) | Pinned portable UDP |

#### Table 2: Server `rocketnet_server_required_memory` across client counts and transports (Linux x86-64)

`rocketnet_server_required_memory(&server_config)` equals:
`rn_server_header_bytes + max_clients * rocketnet_message_connection_bytes(&mc) + 87 + max_candidate_server_transport_bytes`.

| Server profile | `max_clients` | `transport` setting | Per-client message region (`conn_bytes`) | `ROCKETNET_DEV=1` required bytes | `ROCKETNET_DEV=0` required bytes |
| --- | ---: | --- | ---: | ---: | ---: |
| `default` | `1` | `NULL` / `"auto"` (includes `xdp`) | `18,383,616` (17.53 MiB) | `1,894,073,074` (1.76 GiB) | `1,887,796,574` (1.76 GiB) |
| `default` | `2` | `NULL` / `"auto"` (includes `xdp`) | `18,383,616` (17.53 MiB) | `1,913,106,854` (1.78 GiB) | `1,906,830,226` (1.78 GiB) |
| `default` | `64` | `NULL` / `"auto"` (includes `xdp`) | `18,383,616` (17.53 MiB) | `3,093,203,134` (2.88 GiB) | `3,086,919,082` (2.87 GiB) |
| `default` | `64` | `"io_uring"` | `18,383,616` (17.53 MiB) | `2,469,323,734` (2.30 GiB) | `2,463,039,682` (2.29 GiB) |
| `default` | `64` | `"gso"` | `18,383,616` (17.53 MiB) | `1,493,745,440` (1.39 GiB) | `1,487,461,388` (1.39 GiB) |
| `default` | `64` | `"portable"` | `18,383,616` (17.53 MiB) | `1,319,195,936` (1.23 GiB) | `1,312,911,884` (1.22 GiB) |
| `default` | `64` | `"loopback"` | `18,383,616` (17.53 MiB) | `1,313,075,007` (1.22 GiB) | `1,306,790,955` (1.22 GiB) |
| `default` | `100` | `NULL` / `"auto"` (includes `xdp`) | `18,383,616` (17.53 MiB) | `3,778,420,430` (3.52 GiB) | `3,772,132,026` (3.51 GiB) |
| `default` | `250` | `NULL` / `"auto"` (includes `xdp`) | `18,383,616` (17.53 MiB) | `6,633,492,230` (6.18 GiB) | `6,627,185,842` (6.17 GiB) |
| `default` | `500` | `NULL` / `"auto"` (includes `xdp`) | `18,383,616` (17.53 MiB) | `11,391,945,230` (10.61 GiB) | `11,385,608,826` (10.60 GiB) |
| `default` | `1000` | `NULL` / `"auto"` or `"xdp"` | `18,383,616` (17.53 MiB) | `20,908,851,166` (19.47 GiB) | `20,902,454,794` (19.47 GiB) |
| `default` | `1000` | `"io_uring"` | `18,383,616` (17.53 MiB) | `20,284,971,766` (18.89 GiB) | `20,278,575,394` (18.89 GiB) |
| `default` | `1000` | `"gso"` | `18,383,616` (17.53 MiB) | `19,309,393,472` (17.98 GiB) | `19,302,997,100` (17.98 GiB) |
| `default` | `1000` | `"portable"` | `18,383,616` (17.53 MiB) | `19,268,549,696` (17.95 GiB) | `19,262,153,324` (17.94 GiB) |
| `default` | `1000` | `"loopback"` | `18,383,616` (17.53 MiB) | `19,128,723,039` (17.82 GiB) | `19,122,326,667` (17.81 GiB) |
| `light` | `1` | `NULL` / `"auto"` (includes `xdp`) | `947,776` (0.90 MiB) | `1,876,395,723` (1.75 GiB) | `1,870,119,223` (1.74 GiB) |
| `light` | `64` | `NULL` / `"auto"` or `"xdp"` | `947,776` (0.90 MiB) | `1,961,853,150` (1.83 GiB) | `1,955,569,098` (1.82 GiB) |
| `light` | `64` | `"io_uring"` | `947,776` (0.90 MiB) | `1,337,973,750` (1.25 GiB) | `1,331,689,698` (1.24 GiB) |
| `light` | `64` | `"gso"` | `947,776` (0.90 MiB) | `362,395,456` (345.61 MiB) | `356,111,404` (339.61 MiB) |
| `light` | `64` | `"portable"` | `947,776` (0.90 MiB) | `187,845,952` (179.14 MiB) | `181,561,900` (173.15 MiB) |
| `light` | `64` | `"loopback"` | `947,776` (0.90 MiB) | `181,725,023` (173.31 MiB) | `175,440,971` (167.31 MiB) |
| `light` | `100` | `NULL` / `"auto"` (includes `xdp`) | `947,776` (0.90 MiB) | `2,010,686,080` (1.87 GiB) | `2,004,397,676` (1.87 GiB) |
| `light` | `250` | `NULL` / `"auto"` (includes `xdp`) | `947,776` (0.90 MiB) | `2,214,156,355` (2.06 GiB) | `2,207,849,967` (2.06 GiB) |
| `light` | `500` | `NULL` / `"auto"` (includes `xdp`) | `947,776` (0.90 MiB) | `2,553,273,480` (2.38 GiB) | `2,546,937,076` (2.37 GiB) |
| `light` | `1000` | `NULL` / `"auto"` or `"xdp"` | `947,776` (0.90 MiB) | `3,238,578,316` (3.02 GiB) | `3,225,111,294` (3.00 GiB) |
| `light` | `1000` | `"io_uring"` | `947,776` (0.90 MiB) | `2,607,628,266` (2.43 GiB) | `2,601,231,894` (2.42 GiB) |
| `light` | `1000` | `"gso"` | `947,776` (0.90 MiB) | `1,632,049,972` (1.52 GiB) | `1,625,653,600` (1.51 GiB) |
| `light` | `1000` | `"portable"` | `947,776` (0.90 MiB) | `1,591,206,196` (1.48 GiB) | `1,584,809,824` (1.48 GiB) |
| `light` | `1000` | `"loopback"` | `947,776` (0.90 MiB) | `1,451,379,539` (1.35 GiB) | `1,444,983,167` (1.35 GiB) |
| `arena-64` | `64` | `NULL` / `"auto"` (includes `xdp`) | `1,426,240` (1.36 MiB) | `1,994,654,206` (1.86 GiB) | `1,988,370,154` (1.85 GiB) |
| `arena-64` | `64` | `"gso"` | `1,426,240` (1.36 MiB) | `395,196,512` (376.89 MiB) | `388,912,460` (370.90 MiB) |
| `arena-64` | `64` | `"portable"` | `1,426,240` (1.36 MiB) | `220,647,008` (210.43 MiB) | `214,362,956` (204.43 MiB) |

#### Table 3: Per-connection message region sub-allocation (`rocketnet_message_connection_sizing`)

Each connection's message region is sub-allocated from a single 64-byte-aligned
block (`sdk/rocketnet_allocator.cpp`, `docs/CHANNELS.md` section 14):

```text
connection_bytes = 64C + 120.125W + B + 80Q + S + 128P + 24(W + Du) + 1440Ru + 16Rq + SR
```

where `Q`, `S`, and `SR` include the resend-delay payload reserve derived from
`send_kbps` / `per_client_send_kbps`:

| Sub-region | Formula | `default` server (`C=32, W=1024, P=1024`) | `light` server (`C=8, W=256, P=256`) | `arena-64` (`C=4, W=64, P=128`) |
| --- | --- | ---: | ---: | ---: |
| Channel send state | `32 * C` | `1,024` | `256` | `128` |
| Channel receive state | `24 * C` | `768` | `192` | `96` |
| Sender per-number table | `32 * W` | `32,768` | `8,192` | `2,048` |
| Sender first-send times | `8 * W` | `8,192` | `2,048` | `512` |
| Receiver descriptors | `32 * W` | `32,768` | `8,192` | `2,048` |
| Received bitset | `W / 8` | `128` | `32` | `8` |
| Held-ordered index | `16 * 2 * W` | `32,768` | `8,192` | `2,048` |
| Receive ring | `B` | `1,048,576` | `142,400` | `65,536` |
| Send descriptors | `64 * Q_eff` | `1,315,584` | `40,192` | `141,440` |
| Send store (`128`/`512`/`1152` slabs) | `S_eff` | `12,114,368` | `343,616` | `857,856` |
| Sent-packet ring | `32 * P` | `32,768` | `8,192` | `4,096` |
| Sent-packet index | `16 * 2 * P` | `32,768` | `8,192` | `4,096` |
| Sent-packet references | `4 * 16 * P` | `65,536` | `16,384` | `8,192` |
| Delivered entries | `24 * (W + Du)` | `49,152` | `6,240` | `3,072` |
| Sequenced holds | `1,440 * Ru` | `23,040` | `144,000` | `11,520` |
| Receipt ring | `16 * Rq` | `4,096` | `2,048` | `4,096` |
| Resend FIFO | `8 * W` | `8,192` | `2,048` | `512` |
| Send spans | `SR` | `3,581,120` | `207,296` | `318,848` |
| Alignment padding | to 64 B | `0` | `64` | `88` |
| **Total `connection_bytes`** | — | **`18,383,616` (17.53 MiB)** | **`947,776` (0.90 MiB)** | **`1,426,240` (1.36 MiB)** |

### Deterministic planner values vs. measured OS and process memory

`rocketnet_client_required_memory` and `rocketnet_server_required_memory`
report **deterministic worst-case user-space allocator peaks** routed through
`rocketnet_allocator_t`. They differ from Operating System and process memory
metrics (`VmRSS`, `RssAnon`, `/proc/meminfo`, `getrusage(RUSAGE_SELF)`) in two
directions:

1. **OS and kernel memory outside `rocketnet_allocator_t` (why total host memory can exceed `required_memory`)**:
   - **Worker thread stacks**: Each socket worker thread reserves an OS stack
     (`8 MiB = 8,388,608` bytes default on Linux x86-64: 2 threads = `16 MiB`
     for `portable`, 5 threads = `40 MiB` for `gso`, 7 threads = `56 MiB` for
     default 7-queue `xdp`, 0 threads for `io_uring` and `loopback`).
   - **AF_XDP UMEM and kernel rings**: Each AF_XDP queue allocates a 49,152-frame,
     page-aligned `201,326,592`-byte (192 MiB raw) UMEM buffer via `mmap` plus
     kernel fill/completion/RX/TX rings (`327,680` bytes per queue;
     `1,409,286,144` bytes UMEM + `2,293,760` bytes rings across 7 default
     queues). UMEM is counted against `ROCKETNET_SERVER_MEMORY_BUDGET_BYTES`
     and `-DROCKETNET_XDP_MEMORY_MIB`, but is not allocated from
     `rocketnet_allocator_t`.
   - **`io_uring` kernel-mapped submission/completion/buffer rings**: `262,144`
     bytes across the 4 server rings (`mapped_ring_bytes`).
   - **Kernel socket buffers (`SO_SNDBUF` / `SO_RCVBUF`)**: Each UDP socket
     requests `1,000,000` bytes send and `1,000,000` bytes receive buffer in
     the kernel (`1` socket on `portable`, `4` sockets on `gso` and `io_uring`,
     `7` sockets on default `xdp`).
2. **Demand-paged physical RSS vs. reserved virtual arenas (why measured RSS at startup or low load can be much lower than `required_memory`)**:
   - `rocketnet_server_create` and `rocketnet_client_create` allocate the full
     connection and transport arenas upfront so no runtime `malloc` occurs on
     the hot path, and deliberately leave large uncarved regions and the 512 KiB
     `rocketnet_message_batch_t` arrays untouched until messages actually occupy
     those slots.
   - The Linux kernel only faults physical pages into Resident Set Size (RSS)
     when bytes are written. For example, running
     `./bin/transport_memory 1000 1416 0 portable` on Linux x86-64 allocates
     `live_allocator_bytes = 874,407,209` (833.90 MiB) of transport arenas at
     startup, while `process_peak_rss_bytes` after `create` and `start` is only
     `38,871,040` bytes (37.07 MiB) until clients connect and fill their
     reassembly and message rings. Capacity planning must size host RAM for the
     touched working set under peak load plus the OS/kernel resources below,
     never from idle startup RSS.

#### Table 4: Server transport-layer allocator and OS breakdown at `max_clients = 1000` (`./bin/transport_memory`, Linux x86-64, `ROCKETNET_DEV=1`)

Values below come directly from `./bin/transport_memory 1000 1416 0 <transport>`
(which uses the default `8` fragment reassembly slots, `reassembly_bytes = 296,688,452`)
and `./bin/transport_memory 1000 1415 1 <transport>` (`light` reassembly: `1`
slot, `reassembly_bytes = 56,976,952`). The second positional value is ignored
by this inventory tool; both rows plan the fixed
`ROCKETNET_TRANSPORT_MAX_PAYLOAD_BYTES` payload ceiling:

| Server transport | Transport `allocator_bytes` (`8` frag slots) | Transport `allocator_bytes` (`1` frag slot) | `umem_bytes` (page-aligned) | `os_bytes` (UMEM + stacks + mapped rings) | Worker threads (`stack_bytes`) | Sockets (`SO_SNDBUF` + `SO_RCVBUF` req) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `portable` | `874,407,209` (833.90 MiB) | `634,695,709` (605.29 MiB) | `0` | `16,777,216` (16.00 MiB) | `2` (`16,777,216`) | `1` (`1,000,000` + `1,000,000`) |
| `gso` | `915,251,001` (872.85 MiB) | `675,539,501` (644.24 MiB) | `0` | `41,943,040` (40.00 MiB) | `5` (`41,943,040`) | `4` (`4,000,000` + `4,000,000`) |
| `io_uring` | `1,890,829,407` (1.80 GiB) | `1,651,117,907` (1.54 GiB) | `0` | `262,144` (256.00 KiB) | `0` (`0`) | `4` (`4,000,000` + `4,000,000`) |
| `xdp` (7 queues) | `2,514,708,679` (2.34 GiB) | `2,274,997,179` (2.12 GiB) | `1,409,286,144` (1.31 GiB) | `1,468,006,400` (1.37 GiB) | `7` (`58,720,256`) | `7` (`2,293,760` XDP rings) |
| `loopback` | `734,580,552` (700.55 MiB) | `494,869,052` (471.94 MiB) | `0` | `0` | `0` (`0`) | `0` |
