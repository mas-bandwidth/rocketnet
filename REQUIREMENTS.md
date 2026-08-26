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

  The C API is for C to call: an `extern "C"` interface with real,
  non-mangled symbols, consumable from many languages without C++ nonsense.
  No C++ types cross that surface. C# is the first consumer, not the only
  one it is shaped for.

* Development works out of the box, with no backend, no configuration and no
  copying keys between machines. Run the server, point the client at its
  address, connected. This is achieved with published test keys — both the
  backend signing keypair and the server key exchange keypair are fixed values
  printed in the standard and embedded in the library, used by default, so the
  developer configures nothing and the default path is the working path. The
  security comes from the keys being public rather than from a flag being off:
  a deployment that ships with them has an authorization system anyone can
  forge, which fails loudly, where a mode flag that disables checks fails
  silently. Nothing else is relaxed; the development path runs the same
  handshake, sealing and replay protection that ships.

* Example backend code, in Go and C#, for creating connect tokens. That is
  the backend integration story: a customer runs their own backend, and
  minting a token is the only thing it has to do for us. Go needs no
  dependency for the signing half — Ed25519 is in its standard library — and
  sealed boxes are in `nacl/box`.

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

* No security property weaker than netcode 1.02. That is the absolute
  minimum baseline, and a change that buys something new at the cost of an
  existing guarantee is a defect.

* The connect token fits in a single 1500 MTU packet. Because of that it
  names one server, at up to two addresses — an IPv4 one and an IPv6 one,
  each optional. netcode's list of up to 32 candidate servers per token was
  a mistake (Glenn's own, named as such); failover is a fresh token from the
  backend, which has better information than any stale candidate list.

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

* IPv6, designed in properly from day one: a server listens on both IPv4
  and IPv6 addresses, and clients connect over either family. A reasonable
  upgrade in 2026 — netcode ended up being patched to support this;
  RocketNet does not repeat that.

* Multiple servers per physical box is common and supported. A server is
  addressed by address and port; nothing in the library may assume it owns
  the machine. Transports declare their sharing model: socket transports
  run many servers per box, one port each. AF_XDP is a single server per
  box design — that is a property of that transport, the end. A customer
  who wants that changed is asking for paid customization work. (Dean is
  being advised onto GameLift, which has no XDP, so the many-per-box case
  there rides the socket transports anyway.)

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
