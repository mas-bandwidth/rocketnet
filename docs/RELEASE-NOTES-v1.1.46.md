# RocketNet v1.1.46

Changes compared with the `v1.1.45` tag. A point release: no packet layout or
protocol version change, and v1.1.46 endpoints interoperate with every v1.x
endpoint.

- Windows IOCP server transport, rebuilt as measured: `num_xdp_queues` sets
  the number of completion workers (one socket each, as described on
  `num_xdp_queues` in `sdk/rocketnet.h`), sends go out on per-worker lanes,
  receive admission is bounded and counted, and the transport pins its own
  threads. Its endpoints, send lanes and receive admission have TLA+ models
  bound to the code by the trace check.
- Counters: one new row, appended. 284 `IOCP_PIN_REFUSALS`; the contract now
  holds 285 counters and no released id moved.

- Tag: `v1.1.46`, on the merged `main` commit.
- Release: [GitHub release](https://github.com/mas-bandwidth/rocketnet/releases/tag/v1.1.46).
