# Many-client load harness

`bin/load` is the many-client load harness for up to
`ROCKETNET_MAX_CLIENTS` clients. It uses the public client/server objects and
the ordinary five-step frame order. The light profile updates at 10 Hz and
offers at most one packet per client per update; heavier profiles must state
their offered load explicitly.

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
