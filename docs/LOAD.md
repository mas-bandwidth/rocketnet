# Many-client load harness

`bin/load` is the many-client load harness for up to
`ROCKETNET_MAX_CLIENTS` clients. It uses the public client/server objects and
the ordinary five-step frame order. Both profiles update at 10 Hz and offer at
most one packet per client per update. `LOAD_PROFILE=default` (the default)
keeps the SDK's default message and transport sizing. `LOAD_PROFILE=light`
uses the same traffic and checks with message and transport capacities sized
for this harness's 32-byte payloads, eight reliability cases and one-fragment
messages. It does not change public defaults or make a general memory promise
for other applications.

The light profile sets eight channels, a 64-message in-flight reliability
window, 4096-byte receive buffers, a 1415-byte maximum message, 12288-byte send
buffers, 16-message send queues, 16 packets in flight, and one 1416-byte
transport fragment slot. Its 64-kbps per-client send budget gives 25 times the
2.56-kbps payload-only rate in each direction; that ratio is a configuration
calculation, not measured wire usage. The summary reports each endpoint's
`rocketnet_*_required_memory` public block size. Those figures exclude the
transport allocations made at connect/start, native resources and process
overhead, so they are not whole-session memory estimates.

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
1,000-client case, throughput measurements, profiler samples and long soak are
planned hardening work and require an allocated bench plus a complete raw
receipt. They are not performance claims of this release.
