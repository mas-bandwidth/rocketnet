# RocketNet v1.1.12

Changes compared with the `v1.1.11` tag. A point release: no configuration,
export, counter, packet layout or protocol version change, and v1.1.12
endpoints interoperate with every v1.x endpoint.

- Server: the message layer's ack pass does less main-thread work when a
  connection has nothing in flight, and reads the transport round trip time
  only until the connection has its own sample. Server update goes from 2.56
  to 2.07 ms of main-thread time per frame with 1000 clients (EPYC).

- Tag: `v1.1.12`, on the merged `main` commit.
- Release: [GitHub release](https://github.com/mas-bandwidth/rocketnet/releases/tag/v1.1.12).
