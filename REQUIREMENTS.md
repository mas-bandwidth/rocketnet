# RocketNet Requirements

The hard set, locked with Glenn 2026-08-26. The first customer is Dean Hall
(RocketWerkz); "Dean" below is him. Everything here is a requirement, not a
preference.

RocketNet is a merger of flow's good transport bits and zero copy API with a
netcode-like connection protocol and security — connect tokens and all
crypto, which must not exclude player hosted servers. Think of it as pulling
the 1000 player transport goodness out of the space game and giving it to
Dean as a commercial product, meeting Dean's stated needs in the immediate
term — and once done, re-integrating it back into the space game as its
foundation. It serves both the needs of the space game and the needs of Dean
Hall. Two-for-one.

* Replacement for RakNet that isn't IO bound. On Linux this means AF_XDP plus
  non-root alternatives; on Windows completion ports plus whatever else gets
  past IO bounds; on mac the equivalent, to be surveyed.

* The zero copy machinery from flow for sending and receiving packets
  quickly.

* A C# interface, via a simple C API on top of the library underneath. The
  library stays the loose C-like C++ we do — no rewrite in C; the nice C API
  rides on top for Dean to call from C#.

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

* Crypto is sodium, ruled 2026-08-26 after the hydrogen evaluation: gutted
  to what the protocol needs, vendored as the two-file flatten netcode and
  yojimbo already carry, extended with Ed25519 signatures and sealed boxes
  for the public key token design. Hydrogen was feasible and loses on crypto
  pedigree, backend interop, per-packet overhead (36 vs 16 bytes), and the
  missing associated data parameter. XDP crypto stays a nice to have,
  reachable later via a proton style kernel module for either library.
  Interim, so construction moves quickly: brew libsodium as is on the dev
  box; the gutted two-file vendor lands later, before anything ships.

* Scale: 1000 players or above on a single server.

* Network identity is not tied to an IPv4/IPv6 address struct. An endpoint
  is a string with some maximum size, so transports are free to name
  whatever they route by — this way new and interesting transports can be
  created. The data passed in to the client on connect is a string that may
  not conform to an IP address — more like a base64 token or something. TBD.

* Excluded: the greater network next 2.0 visions — relay networks, network
  acceleration, DDoS protection. Dean doesn't want them. If we ever want
  them later, they are brought in as a new transport.

## The shape this adds up to

A big fat connect token connect server, netcode style, with zero copy send
and receive that is not IO bound on any platform. Once that stands,
reliable-ordered packets and most recent packets are added on top.

## Order of operations

1. The minimal secure connect: netcode-style connect tokens on public key
   crypto, exchanging signed and encrypted packets. Crypto per the sodium
   ruling above.

2. A few different platform transports, implemented to learn what the shape
   is. Make it easy to add new transports.

3. With the public key, non IO bound, zero copy netcode standing:
   reliability, flows, broadcast and all that, at the end.
