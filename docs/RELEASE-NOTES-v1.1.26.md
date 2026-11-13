# RocketNet v1.1.26

Changes compared with the `v1.1.25` tag. A point release: no configuration,
export, counter, packet layout or protocol version change, and v1.1.26
endpoints interoperate with every v1.x endpoint.

- Socket transports: shared send helpers, the send gather and the send queue
  bank used by the io_uring send path, each with its TLA+ layout model and a
  trace check bound to the code.

- Tag: `v1.1.26`, on the merged `main` commit.
- Release: [GitHub release](https://github.com/mas-bandwidth/rocketnet/releases/tag/v1.1.26).
