# RocketNet v1.1.15

Changes compared with the `v1.1.14` tag. A point release: no configuration,
export, counter, packet layout or protocol version change, and v1.1.15
endpoints interoperate with every v1.x endpoint.

- Windows IOCP transport: a datagram shorter than the smallest server-bound
  packet is dropped and counted as too short on the network thread, as the
  gso, io_uring, RIO and XDP transports already do, and never takes a
  receive slot.

- Tag: `v1.1.15`, on the merged `main` commit.
- Release: [GitHub release](https://github.com/mas-bandwidth/rocketnet/releases/tag/v1.1.15).
