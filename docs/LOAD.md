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
one `ROCKETNET_MAX_PACKET_PAYLOAD_BYTES` payload (one 1,500-byte MTU packet) per
100 ms in each direction at 10 Hz. It does not change public defaults; the
`default` profile remains the stock acceptance.

## Low-bandwidth population ladder

The requested ladder is 10, 100, 250, 500 and 1,000 clients. At each rung,
offer one 32-byte application message per client per second in each direction
with the fixed-rate option below. For the stock 1,000-client acceptance, run
the `default` profile so the result covers SDK-default sizing. The `light`
profile is a separate, additional bounded-memory workload; it cannot substitute
for the default-profile acceptance. The current `bin/load` harness is capped at
`ROCKETNET_MAX_CLIENTS` (1,000). Future custom 2,000, 3,000, 4,000, 5,000,
6,000, 7,000, 8,000, 9,000 and 10,000-client rungs require an explicitly
extended harness/build and capacity review.

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
nonzero exit. The `public_memory_scope=create_only` and
`transport_selection=unresolved_before_start` labels matter: required-memory
figures include public create allocations, while connect/start may allocate
more transport memory and native resources. The planner cannot certify host
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
whole run, not just the create-only plan. At `830c5437` the plan reports
`total_public_required_bytes=18214470336` (16.96 GiB: 13.32 GB server, 4.90 MB
per client), and the xdp server's start adds about 1.87 GB of allocator memory
and 1.88 GB of UMEM on top. A fixed 16G guest is OOM-killed (exit 137,
anon-rss 15.9 GB) and a 24G guest is killed at the shutdown wipe. The step now
runs `LOAD_PLAN_ONLY=1 ./bin/load 1000 60` with the cell's environment first
and sizes the guest from the plan plus a measured run reserve: `need = plan + 1000 * reserve_per_client_bytes`, and the guest is `need * 5 / 4 + 1 GiB` (the
1 GiB is the guest kernel's own), rounded up to whole GiB. It prints `LOAD cell=xdp FAIL runner has ... MiB available` when the runner's `MemAvailable` is
below that. The guest's load exit status, peak RSS, `/proc/meminfo` totals and
`dmesg | tail -n 50` go into `vm.log` on every run, so an early end names its
cause, and the step prints `guest_peak_rss_kib` beside `need_bytes`, with a
WARN line when the peak passes the need.

The reserve was measured at `a40897f3` in an unconstrained 48G guest on a
121 GB runner: peak RSS 27397952 kB at 1,000 clients and 13703716 kB at 500,
against plans of 18214470336 and 9108290368 bytes. That is 9841033 and 9848630
bytes of reserve per client; the step uses the larger. The peak is 1.54 times
the plan at both counts, so an earlier `plan * 3 / 2 + 2 GiB` sizing sat below
the peak in its ratio term and passed only on its 2 GiB constant (28G guest,
27.4 GiB `MemTotal`, 1.3 GiB over the 26.1 GiB peak). For the default profile
the need is 28063100336 bytes (26.1 GiB) and the guest is 34G, about 7.9 GiB
above the measured peak. A profile change, or a WARN line, means the reserve
must be re-measured.

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
955,710,336 bytes (955,710 bytes per slot on average), and
`rocketnet_client_required_memory` for the client profile is 3,063,168 bytes.
These are configuration calculations and measured create-block sizes, not
measured wire usage. The summary reports each endpoint's
`rocketnet_*_required_memory` public block size. Those figures exclude the
transport allocations made at connect/start, native resources and process
overhead, so they are not whole-session memory estimates. The harness's own
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
