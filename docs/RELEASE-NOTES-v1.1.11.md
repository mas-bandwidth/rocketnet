# RocketNet v1.1.11

Changes compared with the `v1.1.10` tag. A point release: no configuration,
export, counter, packet layout or protocol version change, and v1.1.11
endpoints interoperate with every v1.x endpoint.

- Server: the connection request checks compare the zero pad and the server
  public key (both public values) with a plain memcmp, so the main thread does
  less work per request: a no-cookie request goes from 518 to 395 ns median on
  one core. Replies, counters and check order are unchanged.

- Tag: `v1.1.11`, on the merged `main` commit.
- Release: [GitHub release](https://github.com/mas-bandwidth/rocketnet/releases/tag/v1.1.11).
