# RocketNet v1.1.30

Changes compared with the `v1.1.29` tag. A point release: no configuration,
packet layout or protocol version change, and v1.1.30 endpoints interoperate
with every v1.x endpoint.

- Send reserve: the gso, io_uring, xdp, iocp and rio server transports keep
  part of their send room for the library's own traffic (connection replies,
  keep-alives and disconnects), so game messages cannot starve it. A message
  that would use the kept part waits in its queue and goes out in a later
  call. The part kept is never more than half the room; the portable and
  loopback transports keep nothing back.
- Counters: one new counter, `SEND_RESERVE_REFUSED` (id 277), counts the sends
  held back this way. Ids 0-276 are unchanged.

- Tag: `v1.1.30`, on the merged `main` commit.
- Release: [GitHub release](https://github.com/mas-bandwidth/rocketnet/releases/tag/v1.1.30).
