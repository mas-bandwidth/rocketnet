# RocketNet

Low-level network transport. It's fucking fast!

RocketNet moves bytes between a game server and its clients at the highest IO
rate each platform allows: connection, reliability, fragmentation and broadcast,
and nothing above that line. What a packet means is never this library's
business.

## The model

You send **packets**. There are exactly three kinds, always available, chosen
per packet at send time, with nothing to configure:

- **Reliable** — delivered, in order.
- **Latest** — reliable, most recent only. Newer supersedes older on both the
  send and receive side.
- **Unreliable** — sent once, may be lost.

On the wire, packets are split into **transport packets** sized from what the
live transport reports, and the transport underneath is swappable: plain UDP
sockets, sendmmsg + GSO, io_uring, AF_XDP kernel bypass, IOCP and RIO on
Windows, kqueue on macOS — one config value, identical contract, identical test
suite over every one.

Sends are zero copy: start a packet, write your bytes directly into transport
memory, send it. A server addresses any send to a single **destination** — one
client, or one **flow**, a server-defined set of subscribed clients that
receives the same written-once bytes. Clients just receive packets.

Security is the netcode.io connect-token model, carried over whole.

## Status

Interface first. [rocketnet.h](rocketnet.h) is the entire public contract and
is under active design review; the implementation is catching up behind it. Not
usable yet.

## Documentation

- [rocketnet.h](rocketnet.h) — the contract, fully commented.
- [BUILDING.md](BUILDING.md) — how to build and test.
- [CLAUDE.md](CLAUDE.md) — contributor guide.

## License

AGPL-3.0. See [LICENSE](LICENSE).
