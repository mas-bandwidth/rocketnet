# RocketNet v1.1.20

Changes compared with the `v1.1.19` tag. A point release: no configuration,
export, counter, packet layout or protocol version change, and v1.1.20
endpoints interoperate with every v1.x endpoint.

- Linux gso transport: the send bank is 16384 packets per queue and the
  receive queue 32768, so a gso server is no longer capped by the 4096 send size.

- Tag: `v1.1.20`, on the merged `main` commit.
- Release: [GitHub release](https://github.com/mas-bandwidth/rocketnet/releases/tag/v1.1.20).
