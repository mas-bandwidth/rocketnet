# RocketNet v1.1.43

Changes compared with the `v1.1.42` tag. A point release: no configuration,
export, counter, packet layout or protocol version change, and v1.1.43
endpoints interoperate with every v1.x endpoint.

- Tests: the POSIX create/destroy stress case runs each GSO server with one
  queue instead of the default four, so a cycle maps a quarter of the buffers
  and still starts and stops a worker and the send thread. Tests only;
  behaviour is unchanged.

- Tag: `v1.1.43`, on the merged `main` commit.
- Release: [GitHub release](https://github.com/mas-bandwidth/rocketnet/releases/tag/v1.1.43).
