# RocketNet v1.1.21

Changes compared with the `v1.1.20` tag. A point release: no configuration,
export, counter, packet layout or protocol version change, and v1.1.21
endpoints interoperate with every v1.x endpoint.

- Linux XDP transport: the send queue is 16384 packets per queue (it was 4096,
  which limited two XDP network threads to 491k packets a second), with the XDP
  receive staging and memory cap raised to match.

- Tag: `v1.1.21`, on the merged `main` commit.
- Release: [GitHub release](https://github.com/mas-bandwidth/rocketnet/releases/tag/v1.1.21).
