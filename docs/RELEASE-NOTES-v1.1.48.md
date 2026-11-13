# RocketNet v1.1.48

Changes compared with the `v1.1.47` tag. A point release: no packet layout or
protocol version change, and v1.1.48 endpoints interoperate with every v1.x
endpoint.

- Linux XDP server transport: each pin an XDP thread asks for and the system
  refuses is counted on its queue and reported in a public counter.
- Counters: one new row, appended. 286 `XDP_PIN_REFUSALS`; the contract now
  holds 287 counters and no released id moved.

- Tag: `v1.1.48`, on the merged `main` commit.
- Release: [GitHub release](https://github.com/mas-bandwidth/rocketnet/releases/tag/v1.1.48).
