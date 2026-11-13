# RocketNet v1.1.50

Changes compared with the `v1.1.48` tag. A point release: no packet layout or
protocol version change, and v1.1.50 endpoints interoperate with every v1.x
endpoint. The `v1.1.49` label was carried on `main` but never released; its
changes are in this release.

- Linux io_uring server transport: rebuilt. Each pin an io_uring thread asks
  for and the system refuses is counted, and the datagrams the kernel dropped
  at its sockets are reported, each in a public counter.
- macOS server transport: rebuilt. Datagrams queued at a connected client
  socket that the server transport retired, and never handed to the game,
  are counted when the socket is closed and reported in a public counter.
- Counters: three new rows, appended. 287 `URING_PIN_REFUSALS`, 288
  `URING_RECEIVE_DROPPED_SOCKET` and 289 `RECEIVE_DROPPED_RETIRED`; the
  contract now holds 290 counters and no released id moved.
- AddressSanitizer builds only: poisoned red zones between carved arrays, so
  a write one past an array is reported at the write. Builds without
  sanitizers are unchanged.
- Documentation: [Performance](PERFORMANCE.md), the server transport load
  measurements, linked from the README.
- Tests and tools:
  - the trace check reruns a plant that TLC reports without printing its
    behaviour, up to five times;
  - the load test reconcile names stray datagrams that arrive with no port;
  - its CI lint fix;
  - the CI split: Linux Debug unit runs in three shards, and the Linux
    Release source checks run as their own job.

- Tag: `v1.1.50`, on the merged `main` commit.
- Release: [GitHub release](https://github.com/mas-bandwidth/rocketnet/releases/tag/v1.1.50).
