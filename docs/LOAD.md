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
