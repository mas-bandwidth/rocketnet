# RocketNet v1.1.45

Changes compared with the `v1.1.44` tag. A point release: no packet layout or
protocol version change, and v1.1.45 endpoints interoperate with every v1.x
endpoint.

- Windows RIO server transport, rebuilt as measured: `num_xdp_queues` now
  sets the number of lanes, each a socket on the server's port with its own
  worker thread (zero or 1 is one socket bound exclusively; more than one
  needs a specific bind address and shares the port, see the comment on
  `num_xdp_queues` in `sdk/rocketnet.h`). Workers prefilter on their own
  thread, the send phase keeps a floor, and the lane layout has a TLA+
  model bound to the code by the trace check.
- Counters: two new rows, appended. 282 `RECEIVE_DROPPED_STOPPED` and 283
  `RIO_PIN_REFUSALS`; the contract now holds 284 counters and no released id
  moved.

- Tag: `v1.1.45`, on the merged `main` commit.
- Release: [GitHub release](https://github.com/mas-bandwidth/rocketnet/releases/tag/v1.1.45).
