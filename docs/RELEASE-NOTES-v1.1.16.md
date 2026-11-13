# RocketNet v1.1.16

Changes compared with the `v1.1.15` tag. A point release: no configuration,
export, counter, packet layout or protocol version change, and v1.1.16
endpoints interoperate with every v1.x endpoint.

- Windows RIO transport: a failed create with nothing posted, when closesocket refuses, frees the transport and abandons only the socket handle.

- Tag: `v1.1.16`, on the merged `main` commit.
- Release: [GitHub release](https://github.com/mas-bandwidth/rocketnet/releases/tag/v1.1.16).
