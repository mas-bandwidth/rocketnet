# RocketNet v1.1.24

Changes compared with the `v1.1.23` tag. A point release: no configuration,
packet layout or protocol version change, and v1.1.24 endpoints interoperate
with every v1.x endpoint.

- Counters: the counter set grows from 224 to 277 rows. Ids 0-223 are
  unchanged; rows 224-276 are new and name where a transport counts a receive
  or send refusal (the gso, uring, IOCP and RIO transports) and the shared
  receive and send rows. Some of the new rows count the same event as a shared
  row, so a sum over every refusal row is not a count of distinct drops;
  `standard/TRANSPORT.md` lists each alias.
- Receive prefilter: the shared receive prefilter header used by the socket
  transports.

- Tag: `v1.1.24`, on the merged `main` commit.
- Release: [GitHub release](https://github.com/mas-bandwidth/rocketnet/releases/tag/v1.1.24).
