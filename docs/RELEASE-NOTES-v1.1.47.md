# RocketNet v1.1.47

Changes compared with the `v1.1.46` tag. A point release: no packet layout or
protocol version change, and v1.1.47 endpoints interoperate with every v1.x
endpoint.

- Linux gso server transport, rebuilt as measured: queue workers place
  themselves off cpu 0, and a worker whose pin the system refuses keeps a
  mask that leaves cpu 0 out. Its trace check binds the queue handoff to the
  model.
- Counters: one new row, appended. 285 `GSO_PIN_REFUSALS`, the pins the gso
  queue workers asked for and the system refused; the contract now holds
  286 counters and no released id moved.

- Tag: `v1.1.47`, on the merged `main` commit.
- Release: [GitHub release](https://github.com/mas-bandwidth/rocketnet/releases/tag/v1.1.47).
