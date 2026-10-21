# Many-client load harness

`bin/load` is the many-client load harness for up to
`ROCKETNET_MAX_CLIENTS` clients. It uses the public client/server objects and
the ordinary five-step frame order. Both profiles offer at most one packet
per client per update. The harness visits up to 100 clients every 10 ms, so
at 1000 clients each client updates at 10 Hz; for 1–100 clients each client
updates at 100 Hz, with intermediate rates determined by the number of slices
per round. `LOAD_PROFILE=default` (the default)
keeps the SDK's default message and transport sizing. `LOAD_PROFILE=light`
uses the same traffic and checks with message and transport capacities sized
for this harness's 32-byte payloads, eight reliability cases and one-fragment
messages. It does not change public defaults or make a general memory promise
for other applications.

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
planning and execution.

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
fall further when sends are refused. Fixed-rate runs report
`application_offers_accepted=FAIL` and fail the verdict if either direction
refuses an offered message.

The `wire_window=traffic` counters are snapshots before the traffic clock
starts and at the first drain transition. `wire_window_seconds` measures from
the traffic clock through the transition snapshot, including any last traffic
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

The light profile sets eight channels, a 64-message in-flight reliability
window, 4096-byte receive buffers, a 1415-byte maximum message, 12288-byte send
buffers, 16-message send queues, 16 packets in flight, and one 1416-byte
transport fragment slot. Its 64-kbps per-client send budget is 25 times the
2.56-kbps payload-only rate per direction at 1000 clients. At 1–100 clients,
the 100-Hz schedule is 25.6 kbps of payload, leaving 2.5 times that rate.
Intermediate populations follow the slice cadence. These ratios are
configuration calculations, not measured wire usage. The summary reports each endpoint's
`rocketnet_*_required_memory` public block size. Those figures exclude the
transport allocations made at connect/start, native resources and process
overhead, so they are not whole-session memory estimates.

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
