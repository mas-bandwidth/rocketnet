# RocketNet Requirements

The hard set, locked with Glenn 2026-08-26. The first customer is Dean Hall
(RocketWerkz); "Dean" below is him. Everything here is a requirement, not a
preference.

* Replacement for RakNet that isn't IO bound. On Linux this means AF_XDP plus
  non-root alternatives; on Windows completion ports plus whatever else gets
  past IO bounds; on mac the equivalent, to be surveyed.

* The zero copy machinery from flow for sending and receiving packets
  quickly.

* A C# interface, via a simple C API on top of the C++ library underneath.

* Everything is byte buffers, no structure. To Dean these are "packets" he
  can send. RocketNet does not own serialization.

* Super fast, reliable, maintained, bug free.

* Flows, on the server, for Dean's chunks (squares on a top-down map; players
  subscribe to chunks near their player): create flow, destroy flow, client
  subscribe/unsubscribe on the server, and packets sent to a flow by id reach
  every subscriber. That covers his broadcast case.

* Three packet disciplines, all byte buffers to the caller: reliable-ordered
  packets, unreliable packets, and most recent state only packets.

* Player hosted servers are not excluded by the security model. Knowing
  Dean, he will want them.

* Connection is netcode style: connect with connect tokens — but the tokens
  must not exclude player hosted servers by requiring a secret shared
  between the backend and the servers.

* Crypto is hydrogen, not sodium, if possible.

## The shape this adds up to

A big fat connect token connect server, netcode style, with zero copy send
and receive that is not IO bound on any platform. Once that stands,
reliable-ordered packets and most recent packets are added on top.

## Order of operations

1. The minimal secure connect: netcode-style connect tokens on public key
   crypto, exchanging signed and encrypted packets with hydrogen. First we
   work out whether hydrogen is possible for this at all — we don't know
   yet.

2. A few different platform transports, implemented to learn what the shape
   is. Make it easy to add new transports.

3. With the public key, non IO bound, zero copy netcode standing:
   reliability, flows, broadcast and all that, at the end.
