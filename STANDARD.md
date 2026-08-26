# RocketNet 1.0 — connection standard (draft)

**RocketNet** is a protocol for creating secure client/server connections over
UDP. This document is the security and connection standard, so people can write
their own conforming implementations. It is the packet-level connection layer
only; reliability, flows and broadcast ride above it in payload packets and are
specified separately.

The shape is a connect token minted by a game backend, an address-validated
handshake, sealed packets, and per-client slots. Two properties run through
everything below.

**There is no secret shared between the backend and the dedicated servers.** The
backend signs connect tokens with a private key that never leaves the backend;
it seals the session keys to the target server's own public key. A dedicated
server holds only the backend's public key and its own keypair. A compromised
server cannot mint tokens, and leaks only its own sessions. This is what lets a
title run player-hosted servers on hardware the backend operator does not
control.

**Every connection derives fresh keys, and no key or nonce ever outlives one
connection.** The client contributes a random salt to each attempt, the server
contributes a random session nonce to each mapping, and both sides derive the
working keys from the token keys and those values. Token keys never seal a
packet directly, so no reconnect, restart or replay can bring a key back.

Crypto is libsodium. Nothing here needs a primitive outside a stock build:

* Ed25519 signatures (`crypto_sign`) — backend token authenticity.
* Sealed boxes (`crypto_box_seal`, X25519) — delivering the session keys to the
  server confidentially through the untrusted client.
* BLAKE2b keyed hashing (`crypto_generichash`) — the address validation cookie,
  and session key derivation.
* X25519 (`crypto_scalarmult`) — the ephemeral exchange giving forward secrecy.
* SipHash-2-4 (`crypto_shorthash`) — keying the server's client lookup tables.
* ChaCha20-Poly1305 IETF AEAD (`crypto_aead_chacha20poly1305_ietf`) — every
  sealed packet.

## Architecture

Three components:

1. **The backend** — whatever service authenticates players and decides who
   plays where: a web server, a matchmaker, a game's own service. It holds the
   long-term signing keypair.
2. **Dedicated servers** — instances of the server side of the game, in data
   centers, in the cloud, or on community-hosted machines. Each holds its own
   key exchange keypair and the backend's public key.
3. **Clients** — endpoints running the protocol that want to connect.

The sequence of operations for a client connect:

1. A client authenticates with the backend.
2. The authenticated client requests to play via the backend.
3. The backend generates a _connect token_ and returns it to the client over a
   secure channel, for example HTTPS.
4. The client uses the connect token to establish a connection with the
   dedicated server over UDP.
5. The dedicated server verifies that only clients with a valid connect token
   can connect.
6. Once connected, the client and server exchange sealed UDP packets.

## Keys

Every party holds only keys whose exposure it can afford:

* The **backend** holds an Ed25519 signing keypair (`crypto_sign_keypair`:
  32-byte public key, 64-byte secret key). The secret half never leaves the
  backend. The public half is distributed to every server and every client as
  configuration.

* Each **server** holds an X25519 key exchange keypair (`crypto_box_keypair`:
  32-byte public key, 32-byte secret key). The secret half never leaves the
  server, and its exposure compromises only sessions on that one server. Each
  server also generates a random _cookie key_ at start, which never leaves it
  and is never stored.

* Each **client** holds the backend's public key as configuration, and the
  per-session keys handed to it inside a connect token.

* **Both** hold, per connection attempt, an ephemeral X25519 keypair whose
  secret half is erased as required under _Per-connection keys_.

* A **server** additionally holds a per-run secret keying its client lookup
  tables, described under _No lookup may be linear in the number of clients_.

There is no key in the system that is both shared and secret. That property
is load-bearing everywhere below.

**The one channel that must still be trusted** is server provisioning: the
backend must learn each server's X25519 public key, bound to that server's
addresses. That registration MUST be integrity-protected and authenticated to
the backend. If an attacker can register their own public key against a
legitimate server's address, the backend will seal real players' session keys to
the attacker. How registration happens is deployment-specific and outside this
standard; that it must be authenticated is not. (This is the trust anchor of
the whole design, and it is stated rather than assumed.)

## General Conventions

The key words MUST, MUST NOT, SHOULD, SHOULD NOT and MAY in this document are
to be interpreted as in RFC 2119: MUST is an absolute requirement of
conformance, SHOULD is a strong default an implementation departs from only
with understood consequences, MAY is genuinely optional.

RocketNet is a binary protocol. All data is written in little-endian byte order
unless otherwise specified, including sequence numbers converted to nonce byte
arrays and associated data passed to the AEAD primitive.

**That includes port numbers and the 16-bit groups of an IPv6 address, which is
the opposite of what socket APIs hand you.** `sockaddr` structures carry ports
and address words in network byte order; an implementation that copies them to
the wire unconverted produces buffers whose signatures verify and whose
addresses never match. Convert at the boundary, in both directions.

**Every structure in this document is packed: fields are laid end to end at the
offsets their listed sizes imply, with no alignment padding anywhere.** A
compiler-laid-out struct is not the wire format. The stated total sizes are
exact, and where they exceed the sum of the listed fields, the remainder is
explicit zero padding called out in the layout.

**Timestamps are unix time in seconds**: seconds since the unix epoch as an
unsigned 64-bit count. Sub-second precision is never carried on the wire.

Byte counts from libsodium, named so an implementation checks them against its
own headers rather than trusting this document:

    crypto_sign_BYTES                            64   // Ed25519 signature
    crypto_sign_PUBLICKEYBYTES                   32
    crypto_sign_SECRETKEYBYTES                   64
    crypto_box_PUBLICKEYBYTES                    32   // X25519
    crypto_box_SECRETKEYBYTES                    32
    crypto_box_SEALBYTES                         48   // 32 ephemeral pk + 16 mac
    crypto_generichash_KEYBYTES                  32
    crypto_aead_chacha20poly1305_ietf_KEYBYTES   32
    crypto_aead_chacha20poly1305_ietf_NPUBBYTES  12
    crypto_aead_chacha20poly1305_ietf_ABYTES     16   // packet tag

Where this document says a block is _sealed to the server_, it means
`crypto_box_seal` with that server's X25519 public key: the output is
`crypto_box_SEALBYTES` (48) longer than its plaintext, carries its own ephemeral
public key, and can be opened only with the server's secret key.

**A sealed box is anonymous — it authenticates nothing about who sealed it.**
The server's public key is public, so anyone can seal a block to it.
Authenticity of a connect token comes only from the backend's Ed25519 signature,
and a server MUST verify that signature before it trusts anything the sealed
block contains. This is stated here, in the processing order, and in the
conformance section, because it is the one way this design fails quietly: an implementation that treats a successful open as authentication
lets any party mint tokens for that server.

## Addresses and dual stack

A connect token names **one server**, reachable at up to two addresses: an IPv4
address and an IPv6 address, each optional, at least one present. (A matchmaker
assigns a server, so the only real multiplicity is address family; a list of
candidate servers inside a token would be stale the moment the matchmaker knew
better, and failover is a fresh token instead.)

The two addresses are two ways to reach one machine, **not two candidates to try
in turn**. The client selects the address whose family it can reach and uses
that address for the whole connection. If that address does not answer, the
client does not switch to the other one; it reports failure and the correct
response is to request a fresh connect token from the backend, which has better
information about server health than any stale token does.

*This is deliberate. Because both addresses belong to one server, they share one used-token
history, and that history binds a token to the source address that first
presented it. A client that failed over between families would present the same
token from a different source address and be refused — locking itself out of
both paths. Failover between families is therefore not a client behavior; it is
the backend's job, exactly as failover between servers is.*

RocketNet is dual stack from day one, not as a later addition:

* A server MAY advertise both an IPv4 and an IPv6 address, and it accepts a
  connection request arriving at **either** of them.
* Address normalization is a law of this protocol, not an implementation
  convenience: above the platform socket layer, an IPv4-mapped IPv6 address
  (`::ffff:a.b.c.d`) must never exist. A receiver converts a mapped source to
  native IPv4 exactly once, at the socket boundary, before any address is used
  for a session table or an encryption mapping. Session state keyed on an
  address is where a mapped-versus-native mismatch detonates, and the boundary
  rule is what prevents it.

### The client address is the connection identity

**RocketNet assumes a client keeps the same IP address and port for the whole
duration of its connection to a server.** This is stated rather than left
implicit, because it is the assumption everything else rests on: a server keys
its encryption mapping, its client slot, its replay state and its cookie check
on the client's source address and port. A packet arriving from a different
address is a packet from a different client.

The consequence, plainly: **if a client's address or port changes mid-session,
the connection is lost.** NAT rebinding, a carrier-grade NAT reassigning a port,
and a mobile handover between networks all end the session; the client sees a
timeout and must obtain a fresh connect token and connect again. A game whose
players are largely on mobile networks should know it is choosing this
behavior.

Seamless connection migration in the QUIC style — an address-independent
connection ID on every packet, path validation against redirection, ID rotation
against cross-network tracking — is a real capability this protocol does not
have, and the cost of having it would fall on everyone: on a 24-byte packet
overhead an always-present identifier is a third again, paid by every player on
a fixed connection for an event most never experience. The targets here are PC
and console games on fixed connections, and the keep-alives run often enough
that a NAT binding never goes idle, which removes the common cause of a
mid-session address change on such a network.

**Deliberate scope choice, and deliberately not a wire break to reverse.**
Migration needs an identifier only at the moment the path changes, not on every
packet. It can be added later as a new packet type: sealed under the existing
session keys, carrying an identifier so the server need not trial-decrypt against
every connected client, and followed by the same cookie round trip to the new
address that the connection made at connect time — which is the anti-redirection
protection QUIC's path validation provides. Cost on ordinary packets: nothing.
Cost when it fires: one round trip, rarely. **Packet types 8 through 255 are
reserved and MUST be ignored by a conforming receiver**, so a later
implementation adding migration degrades gracefully against an earlier one: the
packet is ignored and the client reconnects, which is exactly today's behavior.

The case that would justify revisiting this is a title whose players are
predominantly on mobile networks, where cellular-to-wifi handover is routine
rather than rare.

Higher-level transports may name endpoints by an opaque string rather than an IP
address. How a non-IP transport encodes its endpoint in a
connect token is that transport's business; the address encoding below is the
form for the IP transports. **Open for design, and it must be decided before
1.0 freezes: the address fields live inside the signed buffer, so generalising
them later is a wire break. If a string endpoint lands, it must be a fixed-size
field with a length byte bounded against a constant — never a length parsed out
of the data being validated.**

## The Prefilter Header

Every RocketNet UDP packet begins with a 4-byte prefilter header:

    [packet type] (uint8)
    [title tag] (3 bytes)               // a per-title constant

It is not cryptography and it protects nothing. It exists so a server — in
particular a kernel XDP program in front of a server — can drop garbage and
off-path noise at line rate before any cryptographic work happens. The check is
a 3-byte comparison against a constant compiled into the filter.

**The tag depends on nothing but the title.** Not the source address, not the
destination address, not the port, not the packet length. This is a
requirement, not a simplification for its own sake, and it is the accumulated
lesson of the design this one descends from:

* **A client behind NAT cannot know its own source address.** It knows its
  pre-NAT address; the server sees the post-NAT one. Network Next solved this by
  having the server tell the client its observed external address, which its own
  source marks *"only known post-upgrade"* — a bootstrap dependency where early
  packets cannot carry a correct filter at all.
* **Ports cannot be used at all under carrier-grade NAT**, which rewrites them.
  Network Next had already learned this: its filter binds four IPv4 address
  bytes and deliberately excludes the port.
* **A server often does not know its own public address either** — behind a load
  balancer, or on a cloud instance holding a private address while clients
  address a public one. Binding the destination makes the filter a configuration
  problem.

Every one of those disappears if the tag is a constant, and nothing is lost,
because of the property below.

**RocketNet also does not assume that the address a client uses to reach the
backend is the address it uses to reach the server.** Steam Datagram Relay takes
that assumption as a baseline, and it is usually true — but not always: some
carrier-grade NAT deployments egress from an address pool, so a client's TCP and
UDP flows can leave on different public addresses, and a dual-stack client may
reach an HTTPS backend over IPv6 while reaching a game server over IPv4. Network
Next did not assume it either; it had the server observe the client's address and
tell it, which is robust but creates the bootstrap dependency noted above.
Depending on no address at all is the only one of the three approaches that needs
neither the assumption nor the round trip.

**The filter cannot resist a targeted attacker, and is not asked to.** Nothing
in it is secret: an attacker who observes one packet learns the tag and can
reproduce it forever. That is equally true of a derived, per-packet structural
header — observing one packet reveals the construction. So the only threat the
filter can address is *unaimed* traffic: internet background noise, scanners,
and floods of random bytes at the port. Against that, a 3-byte tag admits one
packet in 16.7 million, and a false pass costs almost nothing, because the next
step is a fixed size check (connection requests) or an encryption-mapping lookup
that fails for an unknown source (sealed packets). Even a 10 Gbps flood of minimum-size
frames — about 14.9 million packets per second — puts roughly one packet per
second in front of a check that rejects it in a handful of instructions.

The tag is 3 bytes because that is proportionate to the job. The prefilter is a
traffic-classification convenience, not a security boundary — the sealed layer
beneath it carries the guarantees — so a false-positive rate of 2⁻²⁴ against
random data is ample, and every byte beyond that is spent on every packet
forever for nothing.

**The tag is an arbitrary per-title 3-byte constant, and interoperability is by
value, not by derivation.** Every implementation of a title carries the same
three bytes, agreed the way the protocol id is; how a title chooses them is not
normative. Tooling generates them from a per-title seed and emits the filter as
code in both forms — the userspace check and the XDP program — so no two titles
share a tag by accident. **The tag for the embedded test title of this
standard, used with the published test keys, is `b7 59 01`.**

That is what per-title
generation buys, and the claim should be stated no wider: **it prevents a single
universal RocketNet matcher; it does not prevent fingerprinting.** The tag is
stable for a title by construction, so anyone willing to look at one packet of a
title's traffic has a signature for all of it. Nothing unkeyed can do better, and
a protocol should not claim otherwise.

A receiver drops any packet smaller than the minimum for its type, larger than
1232 bytes, or whose title tag does not match.

There is no keyed or stateful filter. Network Next distributed a rotating
"magic" value to servers from a backend; a player-hosted server has no backend
feeding it per-packet secrets, so the concept is absent by design rather than
deferred.

**The upgrade path for a deployment that needs to resist a targeted flood is a
whitelist in the kernel** — an XDP map of currently authorized source addresses.
That is stateful, is outside this standard, and is the only thing that actually
helps; no unkeyed header can substitute for it.

*Where such a whitelist gets its addresses is the design decision that matters,
and this protocol makes the better answer available.* Steam Datagram Relay
populates its relay whitelist with the client address the backend observed
during the backend conversation — which inherits the same-address assumption
above, and fails wherever that assumption does. A RocketNet server does not need
to guess: the address-validated handshake ends with the server holding the client's actual
UDP source address, **proven** by the cookie round trip rather than assumed. A
whitelist populated after the handshake, from verified addresses, is correct
under CGNAT address pools and dual-stack mismatch alike. Populating one from a
backend-supplied address would import a defect this design does not otherwise
have.

## Packet sizes

One budget governs both address families: the IPv6 minimum MTU is 1280 bytes,
less 40 for the IPv6 header and 8 for UDP, giving **1232 bytes** as the largest
UDP payload that never fragments on either family.

Every sealed packet spends, before the application's bytes:

    prefilter header                  4
    sequence number                   4
    AEAD tag                         16
                                   ----
                                     24

So payload packets carry **1 to budget−24 bytes** of application data — 1 to
1208 at the default budget.

Per-packet overhead is a fixed 24 bytes, every packet, no variable cases.

Every packet type has a fixed total size on the wire except the payload, which
has a range. A receiver checks the size before any other per-type work:

| Type | Packet | Wire size | Composition |
|---|---|---|---|
| 0 | connection request | 708 | 4 + 704 |
| 1 | connection denied | 32 | 4 + 4 + 8 + 0 + 16 |
| 2 | connection challenge | 64 | 4 + 4 + 8 + 32 + 16 |
| 3 | connection response | 48 | 4 + 4 + 24 + 16 |
| 4 | connection keep-alive | 32 | 4 + 4 + 8 + 16 |
| 5 | connection payload | 25 to budget | 4 + 4 + (1 to budget−24) + 16 |
| 6 | connection disconnect | 24 | 4 + 4 + 0 + 16 |
| 7 | connection cookie | 36 | 4 + 8 + 8 + 16 |

The composition columns are prefilter header + sequence + data + AEAD tag for
sealed types, with the additional 8 being the clear session nonce on types 1 and
2; the request is prefilter header + its unsealed body; the cookie is prefilter
header + its three unsealed fields.

The connect token is a fixed 768 bytes, inside a single 1500-byte MTU with room
to spare, by design.

**The 1232-byte budget is the default, not a ceiling built into the protocol.**
It is derived from the one MTU that is guaranteed everywhere — and on IPv6 a
packet exceeding the path MTU is not fragmented in transit but dropped, so
exceeding it on a 1280-byte tunnel path is silent connection death. On IPv4
today, everywhere that matters, the real MTU is 1500.

The budget governs **sending**: a sender MUST NOT emit a packet larger than its
configured budget, default 1232, and a deployment that knows its paths MAY raise
it, to at most 1400. A receiver accepts payload packets up to 1400 bytes
regardless of its own budget, so a budget mismatch between two correctly
implemented ends degrades to nothing worse than one side sending smaller packets
than it could. The budget is a deployment constant, not negotiated, and not
carried on the wire. The fixed packet sizes above do not change with it; only
the payload range does. The 1232 default is the only value that is safe with no
knowledge of the network; every raise is a statement about paths, and it is the
deployment's to make.

## Sequence numbers

Sealed packets carry a **fixed 4-byte sequence number**, sent in the clear
before the sealed data.

**There are four sending sequences per connection: one for each direction of
each key phase.** Handshake and session packets do not share a counter. Each of
the four starts at one and increments by one per packet sent under that key.

**Sequence zero is never sent and never valid: a receiver MUST ignore any
packet whose reconstructed sequence is zero.** Zero is what an uninitialized
counter, a zeroed structure or a forgotten field produces, so making it invalid
on the wire turns that whole class of implementation bug into an immediate,
visible rejection instead of a silently accepted packet.

A receiver tracks, per key phase and per direction, the **highest sequence it
has successfully opened**, and its replay window. At the start of a phase the
highest-opened value is zero — which, since zero is never a real sequence,
unambiguously means nothing has been opened yet — and every window entry is
unmarked.

The value used as the AEAD nonce is the full **64-bit** sequence, reconstructed
from the 4 wire bytes as follows. Let `expected` be the highest successfully
opened sequence plus one — which is 1 when nothing has been opened yet, with no
special case. The candidates are the 64-bit values whose low 32
bits equal the wire value; take the candidate nearest to `expected`, and if two
are exactly equidistant — the wire value sits exactly 2^31 away in both
directions — take the larger. A wrong reconstruction produces a failed open and
a dropped packet, never a wrong acceptance, so the tie rule is for determinism
between implementations rather than for security.

*Why not a variable-length encoding.* Packing the sequence into 1 to 8 bytes
with the count in a nibble makes every packet parse do nibble unpacking, a
range check, and offset arithmetic that depends on the result — variable
geometry that every consumer of the layout has to get right, and the classic
source of size-recheck and offset defects. A fixed field deletes the encoding,
the range check and the dependent arithmetic.

*Why not a fixed 8 bytes.* **It is not this protocol's business to decide how
often an application sends.** A transport that assumes a packet rate becomes the
thing an application has to work around, and the rates differ by orders of
magnitude between a turn-based game, a shooter at 60Hz, and a simulation
fragmenting large state across many packets per frame. Four extra bytes on every
packet is negligible at the low end and a measurable fraction of the link at the
high end, so the choice is made where it costs nothing either way: four bytes
on the wire, a 64-bit value reconstructed in about ten lines, never wrapping,
and no rate assumed.

*Why not two bytes.* A 16-bit sequence wraps quickly enough at high send rates
to leave no room for reordering or a loss burst, which would put a ceiling on
the application's send rate — exactly the constraint this protocol must not
impose.

## Connect Token

A _connect token_ ensures that only authenticated clients can connect to the
dedicated server. The
backend says "this is a valid client, coming from your matchmaker," and the
server verifies that statement holding only the backend's public key.

*If it helps to place this: a connect token is a JWT for game servers.* Signed
claims, an issuer the verifier trusts by public key, an expiry, and an audience —
the named server — with the whole thing carried by the party it describes. That
is the same shape, and saying so is worth more than pretending otherwise: this
converges on a well-understood pattern, which is a point in its favor whichever
way it was arrived at.

The JWT *format* would be a poor fit, and the places it differs are the places
JWT has historically gone wrong. There is no algorithm field, so there is no
`alg: none` and no confusion between verifying an asymmetric signature and a
symmetric one; the algorithm is fixed by this document and never negotiated. The
layout is fixed binary rather than base64url JSON, so a verifier in a hot path
parses nothing and never reads a length out of the data it is validating. And
the confidential part is a sealed box rather than JWE, which keeps the one thing
that must be secret from the carrier — the session keys — out of a construction
that is difficult to use correctly.

*What the fixed layout does and does not protect against, since it is easy to
credit it with the wrong thing.* Length extension proper — the Merkle-Damgård
weakness where `H(secret ‖ message)` can be extended without knowing the secret —
does not apply anywhere in this protocol, and not because of the layout. Ed25519
is a Schnorr construction with no message-extension operation, and BLAKE2b
carries a finalization flag in its last block for exactly this purpose; both
would be safe with variable-length data. What the fixed layout removes is the
neighbouring family, which is the one that has actually produced vulnerabilities
in token formats: a length parsed out of the data being validated, two encodings
of one logical message with a single signature valid for both, and ambiguity
about precisely what the signature covered. Fixed offsets over a fixed buffer
make all three unrepresentable rather than merely unlikely.

### Private connect token data

What only the server may read. Prior to sealing:

    [client id] (uint64)                // globally unique authenticated client id
    [protocol id] (uint64)              // must equal the signed protocol id
    [expire timestamp] (uint64)         // must equal the signed expire timestamp
    [client to server key] (32 bytes)
    [server to client key] (32 bytes)
    [user data] (256 bytes)             // application-defined, per protocol id
    <zero pad to 384 bytes>

The 384-byte block is sealed to the server with `crypto_box_seal` and the
server's X25519 public key:

    [sealed private block] (432 bytes)  // 384 + crypto_box_SEALBYTES

The protocol id and expire timestamp appear inside the seal as well as in the
signed data, and the server MUST check they match. They cost 16 of the block's
free bytes and they domain-separate the sealed block to the token that carries
it, so a sealed block can never be meaningfully reused in another token — for
instance by a backend bug that reissues across two titles sharing one signing
key.

**The client id is inside the seal deliberately.** A
connection request crosses the open internet, so anything in its clear portion is
visible to any observer on the path, and an identifier naming a player must not
be one of those things.

The client cannot open the sealed block and does not need to — it holds the
session keys in the clear. Only the target server can open it.

### Signed connect data

Everything the server checks is gathered into one fixed 576-byte buffer and
signed by the backend. The request carries this buffer and its signature
verbatim, so the server verifies with no shared secret and no other state:

    signed connect data (576 bytes):
        [version] (uint32)              // 0x4E7C1A93 for RocketNet 1.0
        [protocol id] (uint64)          // unique to this title
        [create timestamp] (uint64)     // unix time the token was created
        [expire timestamp] (uint64)     // unix time the token expires
        [timeout seconds] (int32)       // negative disables timeout (dev only)
        [address flags] (uint8)         // bit 0 = has IPv4, bit 1 = has IPv6;
                                        // at least one set, bits 2-7 zero
        [ipv4 address] (6 bytes)        // a,b,c,d (4 x uint8) + port (uint16);
                                        // zero when bit 0 is clear
        [ipv6 address] (18 bytes)       // a..h (8 x uint16) + port (uint16);
                                        // zero when bit 1 is clear
        [server public key] (32 bytes)  // the X25519 key the block is sealed to
        [sealed private block] (432 bytes)
        <zero pad to 576 bytes>

    [signature] (64 bytes) = crypto_sign_detached( signed connect data,
                                                   backend secret key )

Both address fields occupy their bytes whether or not they are used, so the
layout is fixed and a reader never parses a length out of the data it is
validating.

The **server public key** names which of a server's keys the block was sealed
to. It lets a server hold more than one keypair and rotate without rejecting
every in-flight token, and it lets a server reject a token meant for a different
key with a 32-byte compare instead of a failed seal-open.

**The version is an opaque 32-bit constant, not a readable string.** It has to
be on the wire in the clear, because the connection request carries the signed
data a server verifies before it trusts anything. A protocol banner in ASCII
would be a universal signature for every RocketNet deployment in existence,
readable by anyone scanning traffic, which would undo in the connect path what
the per-title prefilter tag is for. A future version takes an unrelated
constant rather than an increment.

The buffer is 576 bytes and uses 521, leaving 55 free **deliberately**. The
signed length is part of the signed message, so growing this buffer later is a
hard wire break; the slack is there so a future field does not force one.

The signature covers the entire buffer including the zero padding. Every field
the server checks is inside it, so none can be modified without failing
verification.

### Connect token format

The full token, handed to the client over the secure channel:

    [signed connect data] (576 bytes)
    [signature] (64 bytes)
    [client to server key] (32 bytes)   // in the clear, for the client
    [server to client key] (32 bytes)   // in the clear, for the client
    <zero pad to 768 bytes>

A fixed **768 bytes** — 1024 characters in base64, inside a single 1500-byte MTU
with room to spare.

The session keys appear twice on purpose: once in the clear for the client,
which receives the token over a secure channel and needs them to speak; and once
sealed to the server, so they reach it confidentially through an untrusted
carrier — the client. The clear copies are outside the signature: they are
trusted because the secure channel delivered them, which is also why the client
does not verify the signature at all. A tampered clear copy causes the handshake
to fail, and nothing worse.

## Per-connection keys

The token keys are never used to seal a packet. Each connection attempt derives
its own keys.

The client generates a random 8-byte **connection salt** for every connection
attempt and sends it in the clear in the connection request. Both sides then
derive four keys with `crypto_generichash`, keyed with the relevant token key,
over the inputs below. Every context string is exactly 8 ASCII bytes with no
terminating null.

    handshake client to server = crypto_generichash(
                                     32 bytes,
                                     salt ‖ session nonce ‖ "RKTNHNDC",
                                     key = token c2s key )

    handshake server to client = crypto_generichash(
                                     32 bytes,
                                     salt ‖ session nonce ‖ "RKTNHNDS",
                                     key = token s2c key )

**Both sides contribute, and the server's contribution is chosen at the moment
the mapping is created.** The client's salt is chosen per attempt; the server's
**session nonce** is a fresh random 64-bit value generated when it creates the
encryption mapping, and sent in the clear in the challenge so the client can
derive with it.

*Choosing it at mapping-creation time rather than earlier is what makes the
guarantee hold.* A nonce issued earlier — at cookie time, say — inherits the
cookie's replayability: replaying the exchange that carried it reproduces the
same derived keys with the sending sequence reset to zero, a repeated
(key, nonce) pair. The same reasoning applies across a restart. Binding the
nonce to the creation of the mapping removes the whole
class: a mapping is created once, gets one nonce, and a nonce a server has used
is never reachable again, so no lifecycle path — replacement, eviction, expiry,
rebuild, restart — can recreate a key whose sequence has been reset.

*Both sides contribute for a reason that generalizes: whichever party is the
only contributor to key derivation decides whether a key recurs, and if that
party is the client, so does anyone who can spoof it. No lifecycle rule about
mapping replacement can substitute, because lifecycles have more exits —
eviction, expiry, retry, restart — than any rule enumerates. Key uniqueness has
to come from the derivation itself.*

The **session** keys additionally mix in an ephemeral key exchange, so that
recovering the token keys later does not recover the traffic. Each side
generates an X25519 keypair for the attempt: the client sends its public half in
the connection request, and the server sends its public half inside the sealed
connection challenge. Both compute the shared secret with `crypto_scalarmult`
and derive:

    shared = crypto_scalarmult( own ephemeral secret, peer ephemeral public )

    session client to server = crypto_generichash(
                                   32 bytes,
                                   shared ‖ client ephemeral public
                                          ‖ server ephemeral public
                                          ‖ salt ‖ session nonce ‖ "RKTNSESC",
                                   key = token c2s key )

    session server to client = crypto_generichash(
                                   32 bytes,
                                   shared ‖ client ephemeral public
                                          ‖ server ephemeral public
                                          ‖ salt ‖ session nonce ‖ "RKTNSESS",
                                   key = token s2c key )

Both ephemeral public keys are in the hash, not only the shared secret they
produce. A shared secret does not uniquely determine the pair of public keys
that produced it, so binding the transcript is what makes the derived key
specific to this exchange rather than to any exchange yielding the same secret.
libsodium's own key exchange hashes both public keys with the scalar
multiplication output for the same reason. Every input is fixed length, so the
concatenation is unambiguous.

The raw shared secret is never used as a key; it is hashed, which is what
libsodium's own documentation requires of a scalar multiplication result. Keying
the hash with the token key means a session key needs **both** the ephemeral
secret and the token key, so neither an attacker who recovers the token later
nor one who somehow obtains an ephemeral secret alone learns the traffic.

**`crypto_scalarmult` returns an error for a small-order peer public key, and an
implementation MUST check it and abandon the handshake on failure.** Ignoring
that return is a classic way to end up agreeing on an all-zero shared secret
with an attacker who chose the point.

**The context distinguishes the direction as well as the phase**, so the two
directions are separated by the derivation itself and not only by the backend
having generated two different token keys. If a backend ever set the two token
keys equal, identical contexts would produce identical derived keys in both
directions, and an attacker spoofing the server's address could reflect a
client's own packet back at it and have it authenticate: same associated data,
same nonce, and replay windows are per direction so a fresh sequence passes. A
protocol should not depend on a backend's key-generation hygiene for a property
it can make structural at no cost.

_Connection challenge_, _connection response_ and _connection denied_ packets
are sealed with the handshake keys. _Connection keep-alive_, _connection
payload_ and _connection disconnect_ packets are sealed with the session keys.

**The whole cost is in the handshake: 64 bytes across two packets and two
scalar multiplications per side, once per connection. Not one byte and not one
operation is added to any game packet.** That is the condition this was built
to satisfy: forward secrecy is worth having only if the packets carrying the
game do not pay for it.

**Ephemeral secrets MUST be erased, and a property with no destruction
requirement is a comment rather than a property.** Both sides MUST zero the
shared secret as soon as the four keys are derived, and MUST zero their own
ephemeral secret once it is no longer needed for a handshake in progress.

A server MAY reuse one ephemeral keypair across a short rotation window rather
than generating one per connection, which makes the cost per connection
negligible. A server that does so MUST bound the window and MUST zero the
keypair when it closes. Note what the reuse actually costs, which is more than
it first appears: the exposure is not the window's duration but **the lifetime
of every session opened during it**, so a long session begun at the start of a
window is protected by a secret that must survive until that session ends. A
server SHOULD generate per connection when it can afford to.

Without erasure the property is worth nothing: an attacker who later reads a
process image, a core dump, a swap file or a backup recovers the ephemeral
secret, and combined with token keys — which this document elsewhere notes can
leak from a proxy cache, a log or the backend itself — recomputes every session
key that secret ever produced.

This is a handful of BLAKE2b invocations and two scalar multiplications per
connection, and it is what makes the following true by construction rather than
by careful bookkeeping:

* **A key exists for exactly one connection, so no sequence space is ever
  entered twice.** Reusing a token key directly as a packet key would make every
  event that restarts a sequence counter under an unchanged key — a server
  restart with an unexpired token, a reconnect from the same address, a replayed
  handshake — a nonce reuse event, and nonce reuse in a Poly1305 construction is
  not only a confidentiality loss; it can expose the authenticator key and yield
  forgery. Here the salt and the session nonce are fresh per connection, so the
  keys differ before any counter is ever reused, and no restart-tracking or
  reconnect-tracking machinery needs to exist.
* **Pre-slot and post-slot traffic cannot collide under one key**, because the
  handshake and session phases are different keys, not offset ranges of one
  sequence space. Both phases simply count from zero.

Tampering with the salt in flight makes the two sides derive different keys and
the handshake fails. That requires an on-path attacker, who can drop the packets
instead; no key is exposed and nothing is accepted.

## The Handshake

The address is proved once, by the cookie exchange, and never again.

1. **Request, cookie.** The client sends a connection request with an empty
   return cookie. The server performs only fixed-offset comparisons, answers
   with a _connection cookie packet_, and allocates nothing. A source that
   cannot receive at the address it claimed gets no further, for the cost of one
   keyed hash.
2. **Request with cookie, challenge.** The client repeats the request carrying
   the cookie. The server validates it, verifies the signature, opens the sealed
   block, derives the keys, creates the mapping, and answers with a _connection
   challenge_ carrying its ephemeral public key, sealed under the handshake key.
3. **Response, keep-alive.** The client opens the challenge, derives the session
   keys, and answers with an empty sealed _connection response_. The server
   assigns a slot and sends the first keep-alive.

*The third exchange carries no cookie, and that is a deliberate subtraction.*
A third-exchange cookie would exist to prove the address again, or to carry
state for a server that stayed stateless through the handshake. Neither purpose
exists here. The address was already proved in step 1, so proving it
again is redundant; and by step 3 the server holds a mapping for that address,
so it is not stateless and has nowhere it needs the client to carry its state
for it. What remains for the response to prove is possession of the handshake
key, and an empty sealed packet proves exactly that: a party who captured the
request bytes but cannot open the challenge cannot produce one.

That is why there is only one cookie construction in this protocol rather than
two.

**The response carries 24 bytes of required zero padding, and the padding is
there for anti-amplification.** Having removed the cookie, the response became
smaller than the keep-alive it provokes, which would have made a connected
client a small reflector. The padding restores the invariant that no response
exceeds the packet that provoked it. **A receiver MUST check both the size and
that the padding is zero** — padding that is merely permitted rather than
required is no protection at all, since an attacker simply omits it.

**Step 3 is still necessary, and it is worth saying what it stops.** Without it
a server would assign a slot at the end of step 2, and an on-path attacker who
spoofs a victim's address could replay the victim's captured request, take the
victim's slot, and leave the victim refused as already connected. Because the
challenge goes to the address, not to the attacker, only the real client can
answer it.

The cookie key is generated at server start and rotated periodically; a
deployment SHOULD accept cookies under the previous key for one rotation period
so a rotation does not fail connections in flight. Cookie lifetime SHOULD be
short — a few seconds is ample, since the client returns it immediately.

## Packets

RocketNet has the following packets:

* _connection request packet_ (0)
* _connection denied packet_ (1)
* _connection challenge packet_ (2)
* _connection response packet_ (3)
* _connection keep-alive packet_ (4)
* _connection payload packet_ (5)
* _connection disconnect packet_ (6)
* _connection cookie packet_ (7)

Every packet begins with the 4-byte prefilter header, whose first byte is the
packet type. The type byte carries the type and nothing else.

### Connection request packet

The connection request (0) is not sealed. After the prefilter header:

    [return cookie] (24 bytes)          // expire and MAC; zero if none held
    [connection salt] (8 bytes)         // client-generated, this attempt
    [client ephemeral public key] (32 bytes)
    [signed connect data] (576 bytes)   // from the token, verbatim
    [signature] (64 bytes)              // from the token, verbatim

A fixed 704 bytes after the prefilter header, 708 bytes on the wire.

The connection salt and the client ephemeral public key sit outside the
signature, because the backend fixed the signed data when it issued the token
and the client chooses these per attempt. Tampering with either makes the two
sides derive different keys and the handshake fails; nothing is exposed and
nothing is accepted.

The **return cookie** field is all zero on a client's first request, which the
server always answers with a _connection cookie packet_. The client fills the
field in with those bytes and sends the request again. See _Processing
Connection Requests_.

### Connection cookie packet

The connection cookie (7) is **not sealed**, because the server sends it before
doing any work that would give it a key, and before allocating any state at all:

    [connection salt] (8 bytes)         // echoed from the request, in the clear
    [cookie expire timestamp] (uint64)
    [cookie mac] (16 bytes)

32 bytes after the prefilter header, 36 on the wire — far smaller than the
708-byte request that provoked it.

    cookie mac = crypto_generichash( 16 bytes,
                                     client address ‖ connection salt ‖ expire,
                                     server cookie key )

The client address input is exactly this encoding of the request's source, 8 or
20 bytes:

    [family] (uint8)                    // 1 = IPv4, 2 = IPv6
    [address] (4 or 16 bytes)           // IPv4 octets in order, or
                                        // 8 x uint16 groups, little-endian
    [port] (uint16)                     // little-endian

followed by the 8-byte connection salt and the 8-byte expire timestamp as they
appear on the wire. The source is normalized per _Addresses and dual stack_
before encoding, so an IPv4 client observed through a dual-stack socket MACs as
IPv4. The port is included because address and port together are the connection
identity. The family byte keeps the two families from ever producing colliding
inputs.

The cookie proves only that the client can receive at the address it claimed.
It carries no key material and no key-schedule input; see _Per-connection keys_
for where the server makes its contribution.

A client that receives one stores its last 24 bytes — the expire timestamp and
the MAC, but not the echoed salt, which the request already carries in its own
field — and puts them in the return cookie field of every subsequent request in this attempt.

**A cookie packet does not provoke a reply.** A client that receives one stores
it and includes it in its next *scheduled* request; it MUST NOT send a request
because a cookie arrived. Otherwise the packet becomes an amplifier pointed at
the server — 36 bytes in, 708 out — that anyone able to see a client's salt
could drive at whatever rate they chose. With the rule, the client's request
rate is its own retransmit rate no matter how many cookie packets arrive, and
the pair is not a provoked pair at all.

**A client MUST discard a cookie packet whose connection salt does not match its
current attempt**, which is why the salt is echoed in the clear alongside the
cookie. Without that check the packet is an off-path denial of service and not a
mild one: injecting a cookie requires only the client's address and the ability
to spoof the server's, where dropping the client's packets requires being on its
path. An attacker injecting junk faster than the client's retransmit rate would
ensure every request the client sends carries an unusable cookie, so it never
passes the gate — a permanent connect denial against a named player for a few
bytes every hundred milliseconds. With the salt checked, an off-path attacker
must guess an eight-byte random value it has never seen.

The server verifies the signature over the signed connect data against the
backend public key, with no shared secret and no other state. Authenticity is
that signature — never the fact that the sealed block opens.

### Sealed packets

Packet types 1 through 6 are sealed. Type 7, the connection cookie, is not. Prior to sealing, after the prefilter header:

    [sequence number] (4 bytes)
    [per-packet type data] (variable length according to packet type)

The per-packet type data:

_connection denied_: no sealed data — the packet is the tag alone.

_connection challenge_:

    [server ephemeral public key] (32 bytes)

**The challenge and the denied packet are both preceded, in the clear and
outside the sealed data, by the session nonce the server chose for the
mapping**, because both can be the first packet of a handshake the client ever
receives, and the client cannot derive the handshake key without it:

    challenge:                              denied:

    [prefilter header] (4 bytes)            [prefilter header] (4 bytes)
    [sequence number] (4 bytes)             [sequence number] (4 bytes)
    [session nonce] (8 bytes)               [session nonce] (8 bytes)
    [sealed data] (48 bytes)  // 32 + 16    [sealed data] (16 bytes)  // 0 + 16

64 bytes on the wire for the challenge, 32 for the denied. The nonce is in the
clear because the receiver needs it to derive the key that opens the sealed
part, and it needs no confidentiality or separate integrity: altering it in
flight makes the receiver derive a different key, the open fails, and the packet
is ignored. Nothing is exposed and nothing is accepted. A client that has
already derived its handshake keys — a denied received after the challenge —
MUST ignore the carried nonce and use the keys it holds.

_connection response_:

    [padding] (24 bytes)                // MUST be zero, and MUST be checked

_connection keep-alive_:

    [client index] (uint32)
    [max clients] (uint32)

_connection payload_:

    [payload data] (1 to budget−24 bytes; 1208 at the default budget)

_connection disconnect_: no data.

The per-packet data is sealed with `crypto_aead_chacha20poly1305_ietf_encrypt`
using the following associated data, which binds the connection's immutable
identity into every packet and so makes any splice across connections fail the
tag:

    [version] (uint32)                  // 0x4E7C1A93 for RocketNet 1.0
    [protocol id] (uint64)
    [packet type] (uint8)               // binds the type; altering it fails the tag
    [connection salt] (8 bytes)
    [client ephemeral public key] (32 bytes)   // types 2 and 3 only

The associated data is these fields packed with no padding: exactly **21 bytes**
for types 1, 4, 5 and 6, and exactly **53 bytes** for types 2 and 3. It is
constructed by both ends and never travels on the wire.

**The client's ephemeral public key is in the associated data of the connection
challenge and the connection response**, types 2 and 3 only. It is omitted from
the connection denied packet, type 1, which is handshake-keyed but may be sent
before the ephemeral has been validated, and from the session types 4, 5 and 6 —
so it costs nothing on any game packet. It is there because the client ephemeral
travels in the clear, outside the signature, and without this binding an on-path
attacker who alters it in flight achieves something worse than a dropped packet:
the handshake runs to completion on handshake keys, which do not involve the
ephemeral, the server assigns a slot, and only then does the client discover it
cannot open a session-keyed packet. The slot is held for the full timeout. One
modified packet per connecting player would empty a server and hold it empty,
which is exactly the zombie-client outcome this protocol is required to prevent.
With the ephemeral in the associated data the client cannot open the challenge,
never responds, and no slot is ever allocated.

The 96-bit nonce is 4 zero bytes followed by the reconstructed 64-bit sequence
as a little-endian value. Post sealing:

    [prefilter header] (4 bytes)
    [sequence number] (4 bytes)
    [sealed per-packet type data] (per-packet data + 16-byte tag)

## Reading packets

In this exact order:

* If the prefilter structural check fails, ignore the packet.

* If the type byte is greater than 7, ignore the packet. Types 8 through 255
  are reserved and MUST be ignored by a conforming receiver.

* **If the packet's total size does not match its type — the exact wire size
  from the _Packet sizes_ table, or the 25-byte-to-budget range for a payload —
  ignore it.** Every non-payload type has a single fixed size, so this is one
  compare, and it runs before any per-type handling so that no later step can
  read past the end of a short packet.

* The server ignores _connection denied_, _connection challenge_ and _connection
  cookie_ packets. The client ignores _connection request_ and _connection
  response_ packets. These types are never legitimately sent in those
  directions, so they are dropped before any further work.

* **If the type byte is zero this is a connection request: process it per
  _Processing Connection Requests_ and stop.** Everything below applies to
  sealed packets only.

* **If the type byte is 7 this is a connection cookie: the client processes it
  per _Processing a connection cookie packet_ and stops.** It is not sealed, so
  nothing below applies to it.

* Look up the keys and replay state for the packet's source address. On the
  server this is the encryption mapping; on the client it is the connection's
  own state, as _Client-Side Connection Process_ specifies — including the case
  where
  the handshake keys do not exist yet and this packet carries the nonce needed
  to derive them. If there is nothing for this address, ignore the packet.

* Read the 4-byte sequence and reconstruct the 64-bit value.

* **If the type is _connection denied_ (1) or _connection challenge_ (2), read
  the 8-byte session nonce that follows the sequence.** The sealed data begins
  after it. For every other type the sealed data begins directly after the
  sequence.

* If the packet type is _connection keep-alive_, _connection payload_ or
  _connection disconnect_, and it fails the replay-protection already-received
  test, ignore it. **Replay protection applies to those three types only**; the
  handshake types are protected by the handshake's own logic and by their
  separate keys.

* Compute the sealed length as the remaining bytes. If it is less than the
  16-byte tag, ignore the packet. *(Implementations must compute this without
  unsigned underflow: check the length before subtracting.)*

* If the sealed data fails to open with the expected key for its type
  (handshake or session), the associated data above, and the sequence-derived
  nonce, ignore the packet.

* For those same three types, **mark this sequence as received in the replay
  window and then advance the most-recent sequence number.** Both steps are
  required: advancing the high-water mark without setting the window entry
  leaves a receiver that accepts duplicates arriving within the window.

* If the opened data size does not match the expected size for the type, ignore
  the packet:
  * 0 bytes — _connection denied_
  * 32 bytes — _connection challenge_
  * 24 bytes, all zero — _connection response_
  * 8 bytes — _connection keep-alive_
  * 1 to budget−24 bytes — _connection payload_
  * 0 bytes — _connection disconnect_

* Otherwise, process the packet.

## Replay Protection

Sealed packets carry sequence numbers starting at one and increasing by one per
packet sent. The sequence is on the wire and read before opening; it is the AEAD
nonce, so any modification fails the tag.

Discard anything older than the most recent sequence minus the replay buffer
size; within the window accept a sequence only once; on a successful open mark
it received and advance the most recent. Applied to _connection keep-alive_,
_connection payload_ and _connection disconnect_ on both ends.

### The AEAD integrity limit

**A server and a client MUST each count the sealed packets that fail to open on
a connection, across all of that connection's keys, and MUST disconnect it on
reaching 2^36 failures.** ChaCha20-Poly1305's integrity limit is 2^36 invalid
packets; both QUIC and DTLS 1.3 make counting them mandatory and name the same
number, because a datagram protocol silently discards what it cannot
authenticate and therefore grants an attacker unlimited forgery attempts where a
stream protocol would have torn down the connection on the first one.

RocketNet's response is simply to disconnect. It has no key update to fall back
on and does not need one: the limit is far enough away that reaching it is an
attack rather than an accident, and a connection under 2^36 forgery attempts has
nothing worth preserving. Both reference protocols permit closing as the
response.

*This is not only theoretical bookkeeping.* At the minimum sealed packet size,
2^36 attempts is a few terabytes, which is under an hour at 10 Gbps against one
connection — and this standard explicitly contemplates sessions running for
hours. In practice a server would exhaust its CPU on the AEAD opens long before
the counter filled, so this is a backstop rather than the thing that saves it;
the RFCs require it regardless, and it is four lines.

### Replay window sizing

A replay buffer of 256 entries per client is a floor, not a recommendation.
**The application decides its packet rate and this protocol does not constrain
it, so the buffer is sized from that rate rather than from any assumption made
here:** an implementation MUST size it to at least one second of the maximum
per-client receive rate the deployment intends to support. An undersized buffer discards
legitimate packets; it never accepts a replay, so the failure is lost traffic
rather than lost security.

*One second is far more than reordering needs — real reordering is milliseconds.
The number is sized for recovering from a loss burst, where a gap in the window
must still be fillable when the retransmissions arrive, and for the case where a
receiver falls behind and catches up. Over-sizing costs only memory, which is
the cheap side of this trade.*

### Lookup complexity

Not wire format, but a requirement all the same, because at a thousand clients a
linear scan is quadratic work in disguise and the protocol is specified for that
scale.

**Every lookup a server performs per packet or per connection MUST be constant
time in the number of client slots.** A linear scan is tolerable at a few
dozen clients and is whole cores at the reference target of a thousand,
because the per-packet rows multiply by the aggregate packet rate:

| Lookup | Performed | Complexity required |
|---|---|---|
| address to encryption mapping | every received packet | constant time |
| address to client slot | every received packet, and both handshake steps | constant time |
| client id to client slot | connection request and response | constant time |
| token identifier to history entry | every connection request | constant time |
| free client slot | every completed connection | constant time |

The per-connection rows are less severe than the per-packet ones, but they are
no harder to do properly.

**A free slot is found from a free list, not a search** — a stack of unused
indices, popped on connect and pushed on disconnect.

**The address and client-id lookups are hash tables, and the hash MUST be keyed
with a secret generated at server start.** The obvious objection to hashing an
attacker-influenced key is that an attacker can choose collisions and restore
the linear behavior deliberately. Two things answer it, and the second is the
stronger:

* `crypto_shorthash` in libsodium is SipHash-2-4, which exists precisely to make
  hash-table flooding impossible and was adopted by several language runtimes
  after that attack class was demonstrated. With a per-run random key an attacker
  cannot predict which bucket an address lands in, so cannot construct a
  colliding set.
* **Insertion is already authorized.** An entry appears only for an address that
  answered a cookie and presented a backend-signed token, so the set of keys in
  the table is not attacker-chosen in the first place. This is a property of the
  handshake rather than of the data structure, and it holds even if the hash were
  predictable.

*An alternative was considered and declined: carrying the client's slot index in
the clear in every packet, so a server indexes its slot array directly and
verifies that the slot's address matches the source. That is genuinely constant
time with no hash and no collision surface at all. It was declined because it
costs two bytes on every game packet, and per-packet overhead is the budget this
protocol guards most closely; a keyed hash costs tens of nanoseconds and no
bytes. The option is recorded because it is the right answer if a future
deployment ever finds the hash to be the bottleneck.*

### Check placement

**Every check this standard specifies is defined by what it rejects and in what
order, never by where it runs.** A transport may perform any prefix of the
sequence as early as it is able. This is a general property of the design and
worth stating once rather than per check.

The AF_XDP transport on Linux should push as much as the kernel allows: the
prefilter comparison, the size bounds, and a lookup against a map of currently
authorized source addresses can all happen in an XDP program, so traffic that
would have been rejected is dropped before it is ever queued to userspace. The
earliest possible reject is the cheapest one, and it takes the work off the
game's cores entirely. **Any part of the protocol that can move down, should.**

Transports that have no equivalent — plain sockets, io_uring, completion ports —
perform the same checks in userspace and are equally conformant. Nothing in the
protocol depends on a check happening in the kernel, because most platforms have
no way to do that.

**A kernel map is a hash table too, and inherits the same question.** A
whitelist held in a `BPF_MAP_TYPE_HASH` is subject to the same collision
concern as a userspace table, and the same reasoning resolves it — with the
same ordering of importance.

The protection that actually holds is **that an attacker cannot choose what is
in the table.** Entries appear only for addresses that answered a cookie and
presented a backend-signed token, so the key set is legitimate clients and its
size is bounded by the slot count. An attacker who cannot insert cannot lengthen
a chain; the most they can do is aim lookups at whichever chain is already
longest, and with a table sized for its occupancy that is a handful of
comparisons rather than a degradation. This argument does not depend on the hash
at all, which is why it is the one to rely on.

Hash quality is then defence in depth rather than the primary control. The
kernel's hash maps are understood to seed their hashing with a per-map random
value chosen at creation, which would put them in the same position as a keyed
userspace table — **but that is a property of a particular kernel, not of the
protocol, and an implementation targeting a specific kernel should confirm it
rather than assume it.** Where certainty is wanted, the alternatives are to
compute a keyed hash inside the program and index an array map directly, taking
full control of placement, or to key the map by something the server assigns
rather than something the client presents.

**Two rules keep this safe.** A transport that rejects early MUST reject exactly
what the full sequence would have rejected: no more, or it discards legitimate
traffic that the userspace path would have accepted; no less, or the guarantee is
weaker than specified. And the userspace path MUST NOT assume an earlier stage
ran, since the same implementation serves every transport. Pushing work down
therefore reduces the *volume* reaching userspace rather than removing code from
it — the check still runs, but on the handful of packets that survived instead of
on the flood. That is where the win is, and it keeps one implementation of the
logic rather than one per transport.

## Timing

Every rate in this protocol is fixed and measured in wall-clock time. None of
them adapt to the round trip time, and none of them are expressed per
application update.

| Value | Default | Applies to |
|---|---|---|
| Send rate | 10 Hz | handshake retransmission, and keep-alives on an established connection |
| Handshake deadline | 5 s | server: mapping created but connection not established |
| Total connect deadline | token lifetime | client: whole handshake, from the start of the attempt |
| Connection timeout | token `timeout seconds` | either end: no traffic received on an established connection |
| Cookie lifetime | 3 s | server: how long an issued cookie is accepted |
| Cookie key rotation | 60 s | server: previous key accepted for one period |
| Redundant disconnect packets | 10 | either end, on a deliberate disconnect |

**The send rate is a rate in time, not a rate per update.** An implementation
MUST NOT send a packet per application update. This protocol places no
constraint on how often an application updates or how many packets it sends, and
an application may update at a thousand hertz; a handshake that retransmitted on
every update would flood the network on such an application and starve on a slow
one. Retransmission and keep-alive generation are driven by elapsed time and
nothing else.

**Retransmission is at a fixed rate, with no backoff and no round-trip
estimation.** This is a connection protocol whose handshake completes in a
handful of packets and whose deadline is a few seconds; an adaptive scheme would
add state and failure modes to buy nothing measurable over that interval. A
deployment MAY raise the send rate — 20 Hz is reasonable — and MUST keep it
constant for the duration of an attempt.

**Every value here is a default and may be tuned by a deployment**, which knows
its players better than this document does. The defaults are chosen so that a
game that never touches them is correct and safe.

*A note on one interaction:* the cookie lifetime and the send rate together
determine whether a client can lose a race — if a cookie expires before the
client's next scheduled request, the server reissues one and the client uses it
on the request after that. A cookie lifetime shorter than two send intervals
makes that race likely and wastes a round trip; a few seconds against a 10 Hz
send rate leaves ample margin.

## Client State Machine

A client is in exactly one of these states. Rates and deadlines are in
_Timing_.

* _connect token expired_ (-7)
* _invalid connect token_ (-6)
* _connection timed out_ (-5)
* _connection response timed out_ (-4)
* _connection request timed out_ (-3)
* _cookie request timed out_ (-2)
* _connection denied_ (-1)
* _disconnected_ (0)
* _requesting cookie_ (1)
* _sending connection request_ (2)
* _sending connection response_ (3)
* _connected_ (4)

**The state MUST be readable from outside at any time**, and it is the primary
means of diagnosing a connection that is not progressing. A state machine whose
current position cannot be observed can only be debugged by inference, and the
failure it is worst at explaining is the one that matters most here: a client
that never connects and never says why.

*Why the cookie phase gets a state of its own.* The client sends a request with an empty cookie field and waits for
a cookie, then sends a request carrying that cookie and waits for a challenge.
Those are two different waits with two different failure meanings, and collapsing
them would make the protocol's two most common deployment faults
indistinguishable: _cookie request timed out_ means nothing was ever heard from
the server — wrong address, wrong port, wrong title tag, server down, a firewall
— while _connection request timed out_ means the server answered, issued a
cookie, and then silently refused the token, which points at the signature, the
sealed block, the advertised address or the protocol id. Both surface as a client
that will not connect; only the state separates them.

Both states send a _connection request packet_. The state names the phase, not
the packet.

**The numbering carries an invariant: a waiting state N times out to state
-(N+1).** _requesting cookie_ (1) → -2, _sending connection request_ (2) → -3,
_sending connection response_ (3) → -4, _connected_ (4) → -5. Only _connection
denied_ (-1), which is told to the client rather than inferred from silence, and
the two token defects the client detects before sending anything, fall outside
it. An implementation SHOULD assert the pairing, so that a state added later
cannot silently ship without its timeout.

**The ordering is load-bearing, not cosmetic.** The handshake is exactly the
range `state > disconnected && state < connected`, so any rule that applies
"while connecting" is one range check that stays correct as states are added
between. The rule below relies on it.

**A total connect deadline bounds the client, as the handshake deadline bounds
the server.** Per-state timeouts alone are not enough on either side: a client
that keeps making just enough progress could walk between states indefinitely,
and per-state timers would never fire. So while the client is in the handshake
range it MUST abandon the attempt once the token's lifetime — `expire - create`,
measured from the start of the attempt — has elapsed, transitioning to _connect
token expired_ (-7). The deadline is measured as a duration from the attempt's
start rather than against the absolute expire timestamp, so that a client whose
clock disagrees with the backend's still gives up after the intended interval.
The check stops at _connected_: an established connection is governed by its
idle timeout, not by the token that admitted it.

Before its first request the client verifies the token: the address flags name
at least one family with bits 2-7 zero, and the create timestamp is not later
than the expire timestamp; otherwise _invalid connect token_. The client selects
the address whose family it can reach, preferring IPv6 when both are present and
both are reachable, and uses that address for the whole attempt. If no listed
family is reachable it transitions to _cookie request timed out_ — the failure
belongs to the first exchange, which is the one it could not begin.

The client generates its connection salt once per connection attempt, before the
first request, and reuses it for every retransmission of that attempt. **The
client's packet sequence number MUST NOT be reset within an attempt.** A new
attempt means a new salt, hence new keys, so restarting the sequence at zero is
correct there and only there.

In each of the three sending states the client retransmits its current packet at
the send rate until it receives the packet that advances it or a deadline
elapses. Storing and echoing the cookie is specified in _Connection cookie
packet_; establishment, keep-alive generation, timeout and disconnect are in
_Connected Clients_.

## Client-Side Connection Process

The client's half of the handshake, as ordered processing per packet type. The
governing property is the one stated in _Processing Connection Requests_ from
the other side: **no single lost packet may strand either end.** Every state the
client waits in is a state it retransmits from, every retransmission re-provokes
the server's answer, and every answer is one the server can repeat — so any
dropped packet, in either direction, is repaired by the next send interval.

### Starting an attempt

* Validate the token and select the address as specified above.
* Generate the connection salt: 8 bytes from a cryptographically secure random
  source, fresh for this attempt.
* Generate the client ephemeral X25519 keypair, fresh for this attempt.
* Initialize the sending sequences for both key phases so the first packet
  sent under each key carries sequence 1, with empty replay state.
* Enter _requesting cookie_ and send a connection request with an all-zero
  return cookie field at the send rate.

### Processing a connection cookie packet

Processed in _requesting cookie_ and _sending connection request_; ignored in
every other state.

* If the echoed connection salt does not equal this attempt's salt, ignore the
  packet.
* Store the packet's last 24 bytes — the expire timestamp and the MAC —
  **replacing any cookie already stored.** Replacement matters: if an attempt
  outlives a cookie's lifetime, the server answers the stale-cookie request with
  a fresh cookie, and a client that kept the old one would present it forever
  and never connect.
* If in _requesting cookie_, transition to _sending connection request_.
* Do not send anything in response; the next scheduled request carries the
  cookie. (_Connection cookie packet_ gives the amplification reason this is a
  MUST.)

### Processing a connection challenge packet

Processed in _sending connection request_ only. In every other state a
challenge is a duplicate or an injection, and is ignored — a genuine
retransmitted challenge exists only to answer request retransmissions, and a
client past _sending connection request_ no longer sends requests.

* Read the clear session nonce following the sequence.
* Derive the two handshake keys from the token keys, the salt and that nonce,
  **into temporary storage**. A forged nonce must not be able to overwrite
  anything: keys are committed only after a successful open.
* Open the sealed data with the temporary handshake server-to-client key and
  the type 2 associated data. If the open fails, discard the temporaries and
  ignore the packet.
* Compute `shared = crypto_scalarmult( client ephemeral secret, server
  ephemeral public )` with the key just unsealed. If it returns non-zero,
  discard everything and ignore the packet; the attempt then ends at its
  deadline, which is the correct outcome for a peer offering a degenerate key.
* Derive the two session keys. Commit all four keys and the session nonce.
* **Erase the client ephemeral secret and the shared secret.** From this moment
  the client holds only the four derived keys, exactly as the server does.
* Transition to _sending connection response_ and send the connection response
  at the send rate. The response MAY additionally be sent immediately on this
  transition: the challenge that triggered it is authenticated, so this is not
  a reflection lever, and it saves most of a send interval of latency. The
  cookie transition has no such allowance because the cookie is unsealed.

### Processing a connection denied packet

Processed in _sending connection request_ and _sending connection response_;
ignored in every other state.

* If handshake keys are held, ignore the carried nonce and open with the held
  keys. If not, derive temporary handshake keys from the carried nonce exactly
  as for a challenge.
* If the open fails, ignore the packet. An off-path attacker cannot deny a
  connection: without the token keys, nothing it sends opens.
* On success, transition to _connection denied_ (-1), stop sending, and release
  all connection state.

### Processing keep-alive, payload and disconnect packets

These open under the session keys, which exist from _sending connection
response_ onward; in earlier states there are no keys for them and they are
ignored at the key lookup.

* A keep-alive in _sending connection response_ carries the client index and
  the slot count: store both, transition to _connected_. In _connected_ it
  refreshes the last-received time, as does any opened packet.
* A payload is delivered to the application in _connected_ only. In _sending
  connection response_ it is ignored: the client does not yet know its client
  index, and the server prefixes every payload with a keep-alive while the
  client is unconfirmed, so the transition arrives with the traffic.
* A disconnect in _sending connection response_ or _connected_ transitions to
  _disconnected_ (0) immediately, releasing all state. It is never answered;
  the sender's redundant copies are the whole delivery mechanism.

### The loss walkthrough

Walk every loss. A lost request or cookie: the client is in states 1–2, still
retransmitting requests, and every request re-provokes a cookie or the stored
challenge. A lost challenge: the client retransmits its request, and the
server's mapping answers it byte-identically. A lost response: the client
retransmits it at the send rate, and the server's response processing tolerates
it — the first copy assigns the slot, later copies find the address connected
and are ignored, while the keep-alives generated at the send rate keep
answering. A lost keep-alive: the next one is at most a send interval away, in
both directions, forever. A lost denied: re-provoked by the next retransmitted
request or response, because the server holds no-slots state (none, or the
mapping) that repeats it. A lost disconnect: nine more copies follow, and the
timeout stands behind all ten. There is no packet whose loss leaves either end
waiting for something that will never be sent again — and any change to the
protocol MUST preserve this by construction, not by luck: every wait state
retransmits, and everything a server sends, it can send again.

## Server-Side Connection Process

A server has a fixed number of client slots, configured at startup.
Implementations are expected to support high slot counts; the reference target
is 1000.

### Processing Connection Requests

**The governing intent, which every rule in this section serves: a client
without a valid connect token must be trivially rejectable.** That is what the
connect token is for beyond authorization — it is the protection against being
flooded off the internet. A server should be able to throw away traffic from
anyone who has not been sent by the matchmaker at a cost so small that an
attacker gains nothing by sending it, and the moment rejection stops being
trivial the token has stopped doing half its job.

Strict rules follow from that intent: clients must have
a valid connect token; respond only when necessary; ignore malformed requests as
cheaply as possible; every response smaller than the request, so there is
nothing to amplify.

**Public key cryptography puts that intent under real pressure, which is why the
cookie exists.** Verifying a signature is not trivial, and an attacker who can
make a server verify one has made rejection expensive by definition. The cookie
restores the property: a request without one is refused after a single keyed
hash, and only an address that has proved it can receive gets any asymmetric
work spent on it. Read the ordering below as the implementation of the intent
above, not as a list of checks that happen to be in an order.

**The two properties, and why they take four layers.** What a server must have
is a bar that is hard to get over — you cannot connect unless the matchmaker
sent you — and no way to occupy a slot with a zombie client that is not really
there. A shared-secret design gets both from one symmetric check, because the
shared key makes the bar real *and* makes enforcing it cost about a
microsecond, so cheap rejection and a high bar are the same operation. Without
a shared secret the two separate, and they are rebuilt as a ladder, each rung
rejecting what it can at its own cost so the expensive rung is reached only by
traffic that has already cleared the cheap ones:

| Layer | Rejects | Cost | Bar |
|---|---|---|---|
| Prefilter | traffic that is not this title's | a 3-byte compare | none — unkeyed |
| Cookie | a source address that cannot receive | one keyed hash | must hold the address |
| Signature | anyone the matchmaker did not send | one Ed25519 verify | must hold a real token |
| Challenge | a client that never answers | already paid | must complete a round trip |

The bar is the signature and it is as hard as it has ever been — an attacker
needs a token the backend actually issued, which means authenticating with the
backend. The zombie is stopped by the challenge: no slot
is allocated until the client has answered from the address it claimed. What the
cookie adds is not a new bar but a cheap gate in front of the expensive one, so
that the cost of *enforcing* the bar cannot itself be turned into the attack.

Under public key crypto the ordering of these checks carries real weight. A
connection request that reaches the crypto costs a signature verification and a
seal-open — on the order of a hundred microseconds, against about one for a
symmetric check. Every
check that can reject without touching the token therefore runs first. In
particular the already-connected check reads only the packet's source address,
and it comes **before** the crypto: otherwise an attacker who captures a single
valid connection request can replay it at line rate and burn a full verify on
every copy.

**Address validation always comes before asymmetric work.** A server MUST
require a valid return cookie before performing the signature verification, and
answers a request without one by sending a _connection cookie packet_ and
stopping. There is no threshold and no mode: the cookie exchange happens on
every connection.

*Always-on rather than load-triggered, for two reasons. The round trip is cheap
here: connections last minutes and a connect already takes on the order of a
second, so one extra exchange at the start is invisible — a protocol with short,
numerous connections would weigh this differently. And a defence that engages
only above a threshold is a code path that never runs in normal operation or in
testing, most likely to be found broken the day it is first needed, with a
tuning knob and a seam an attacker can sit just underneath. Always-on deletes
the rate measurement, the threshold, the seam, and the untested branch.*

*Why this is required rather than advised, and why the earlier mitigations were
not enough.* Every check that precedes the signature verify reads a field the
attacker either controls or can copy from the client binary: the size, the
version, the protocol id, the expire timestamp it writes itself, the address
flags, the advertised address, the server public key, and a used-token
identifier that is a hash of its own bytes and therefore unique every time. The
already-connected check passes trivially, because a spoofed source is never
connected. So a request built from public constants and random padding forces a
full Ed25519 verification, from a spoofed address, with no captured packet and
no on-path position. At roughly a hundred microseconds each that is about ten
thousand per second per core, or **tens of megabits per second of attack traffic
to saturate a core** — three orders of magnitude below what the same attack
costs against a symmetric check. The
prefilter cannot help: it is unkeyed and its code ships in every client. Rate
limiting cannot help: the source address is spoofable, which this standard
already says. The used-token history cannot help: it bounds replays of captured
tokens, not fabrications. **The cookie is the only one of these that changes the
attacker's cost**, and it changes it completely — to obtain a cookie you must
receive at the address you claim, so the flood must come from addresses you
really hold, where per-source rate limiting finally bites. This is the SYN
cookie pattern, and it is what DTLS 1.3 and QUIC both do: cheap keyed symmetric
work first, expensive asymmetric work only for an address that has answered.
Making it load-triggered rather than always-on follows DTLS 1.3, which performs
the cookie exchange by default but permits a deployment to skip it where
amplification is not a threat.

In this exact order, on a _connection request packet_:

* If the packet, less the prefilter header, is not 704 bytes, ignore it.
* **If a client from the request source address and port is already connected,
  ignore it.** This costs one lookup and no crypto.
* **If an encryption mapping already exists for the source address and port,
  resend the stored challenge packet, byte-identical, and stop.** This is the
  ordinary case of a retransmitted request whose challenge was lost, and it is
  the only correct answer: the mapping already has its session nonce and its
  keys, and deriving new ones for a source that already holds a mapping is
  exactly the key-recurrence hazard the nonce exists to prevent. A mapping is
  created once, answered identically for as long as it lives, and replaced
  never — a different token from the same address waits until the mapping
  expires. The resend costs no crypto and is smaller than the request that
  provoked it, so it is safe this early; it is placed after the
  already-connected check so an established client's address cannot be made to
  emit anything.
* If the version is not 0x4E7C1A93, ignore it.
* If the protocol id does not match the server's, ignore it.
* If the expire timestamp is <= the current time, ignore it.
* If the address flags have bits 2-7 set, or name no family, ignore it.
* If neither flagged address in the signed data is one this server is advertised
  at, ignore it. Either advertised address is accepted, not only the first.
* If the server public key in the signed data is not one this server holds,
  ignore it.
* **Recompute the cookie MAC** from this packet's source address, its connection
  salt, and the expire timestamp in its return cookie field, and
  compare in constant time. If the field is zero, the MAC does not match, or the
  cookie has expired, respond with a _connection cookie packet_ and stop. One
  keyed hash, no asymmetric work, and no state allocated.

  *This is placed after every check above it and before every check below it, and
  the position is deliberate on both sides.* Everything above is a fixed-offset
  comparison against a constant that rejects silently and costs nothing, so a
  server must not answer a packet that fails one of them: doing so would emit a
  cookie packet for any datagram carrying the right title tag and a zero type
  byte, making the server a packet-rate reflector for garbage and spending a hash
  and a system call on every piece of it. Everything below it is expensive — the
  used-token lookup hashes 576 bytes, and the three asymmetric operations follow —
  so nothing below may run for an address that has not answered.
* If the token identifier already appears in the used-token history bound to a
  different source address and port, ignore it. **The token identifier is the
  output of `crypto_generichash` invoked with a 16-byte output length and no
  key, over the 576-byte signed connect data** — a content-determined value.
  BLAKE2b binds its output length into the hash, so a 16-byte invocation is not
  a truncation of the 32-byte one; implementations MUST use the 16-byte output
  length, not truncate. *(It is deliberately not the signature
  bytes. Signature malleability means an implementation that does not enforce
  canonical Ed25519 verification would see a mangled copy of the same token as a
  new one, and an attacker could mint unbounded distinct identifiers from one
  captured request. Hashing the signed data has no such freedom, and is smaller
  per entry.)* Comparisons MUST be constant time.
* **Verify the signature over the 576-byte signed connect data against the
  backend public key (`crypto_sign_verify_detached`). If it fails, ignore the
  packet.** This is the authenticity check and it comes before the seal is
  opened.
* Open the sealed private block with the keypair named by the server public key
  field (`crypto_box_seal_open`). If it fails, ignore the packet.
* If the protocol id or expire timestamp inside the sealed block does not equal
  the signed one, ignore the packet.
* If a client with this client id is already connected, ignore it. If this
  token's identifier is already bound to a connected client in this run, ignore
  it — a token admits one connection per server run.
* Record the token identifier with this source address and port in the used-token
  history.

* **Generate a fresh random 64-bit session nonce for this mapping.** It is
  never reused, and because it is chosen here rather than at cookie time, no
  replayed cookie can cause a key to recur.
* **Compute `shared = crypto_scalarmult( server ephemeral secret, client
  ephemeral public )`. If it returns non-zero, ignore the packet.** libsodium
  returns an error for a small-order peer key, and this check must happen here —
  before any mapping is created — so that a client offering such a point never
  obtains a mapping. This is the third asymmetric operation on this path and it
  is deliberately after both the cookie and the signature verification.
* Derive the four per-connection keys: the two handshake keys from the token
  keys, the salt and the session nonce just generated; the two session keys from
  those plus the
  shared secret and both ephemeral public keys.
* **Erase the shared secret**, and the server's ephemeral secret if it is not
  being reused across a rotation window, as soon as the four keys exist.
* If no client slots are available, respond with a _connection denied packet_ —
  sealed under the handshake server-to-client key just derived, at sequence
  one, carrying the session nonce in the clear — and stop. No mapping is
  created, so a retransmitted request runs this path again and is denied again.
  *The slot check is placed here rather than earlier because a denied packet is
  sealed, and before this point no keys exist to seal it with.*

* Add an encryption mapping for the source address and port, holding the four
  derived keys, the connection salt, the session nonce, the server ephemeral
  public key, the client id, the user data, and the sending sequence and replay
  state for both key phases. It is bound by **two independent deadlines**,
  described below. If it cannot be added, ignore the packet.
* Build a _connection challenge packet_ carrying the server's ephemeral public
  key and the session nonce, send it, and store it in the mapping, so that a
  retransmitted request is answered with a byte-identical copy for as long as
  the mapping lives.

**Every valid request is answered every time it arrives, not only the first
time.** This is an invariant of the whole protocol, and the reason is packet
loss: a response sent only once is a response that can be lost, leaving the peer
retransmitting a request that is silently ignored forever — a connection that
dies not from any failure but from one dropped packet. So a request without a
valid cookie gets a cookie packet on every arrival; a request whose mapping
exists gets the stored challenge on every arrival; a request that finds the
slots full is denied on every arrival; a retransmitted connection response finds
the client connected and is ignored, but the keep-alives that answer it are
generated at the send rate for as long as the client is connected, so the answer
repeats anyway. Silence is reserved for invalid traffic. When adding any new
exchange, this invariant is part of its design: the response must be repeatable,
and the state that repeats it must live somewhere.

**Half-open state, and the two deadlines that bound it.** A handshake that is
started and never finished is the classic way to attack a connection protocol,
and there are two separate things to get right: what a stalled handshake can
occupy, and how long it can occupy it.

*What it can occupy is an encryption mapping, never a client slot.* Slots are
assigned only in step 3, after the client has answered a challenge sent to the
address it claimed, so no amount of stalling before that point consumes live
server capacity. This separation is the structural defence;
everything below is about duration.

*How long it can occupy it is bounded twice, and the second bound is the one
that matters.*

* An **idle deadline**, refreshed by traffic, which reclaims a mapping whose
  peer has gone silent.
* A **handshake deadline**, fixed when the mapping is created and **never
  refreshed**, by which the connection must be established or the mapping is
  destroyed.

The idle deadline alone would be a hole. A mapping exists from the end of step 2,
and a client may legitimately retransmit its request — so a peer that retransmits
just often enough to touch the mapping resets the idle timer forever and stays
half-open for as long as it cares to, at the cost of one small packet per
interval. The handshake deadline is what makes that pointless: it does not move,
whatever the peer sends. Connect within it or be discarded.

**The handshake deadline MUST NOT be the token's `timeout seconds`.** That field
is the *connected* idle timeout — how long an established player may go quiet
before being dropped — and it is sized for gameplay, in tens of seconds. A
legitimate client completes steps 2 and 3 in one round trip; a few seconds
covers that with room for several retransmissions. Reusing the gameplay timeout
here would hand an attacker a ten- to sixty-fold multiplier on the duration of
every mapping they hold, bought with nothing. The handshake deadline is a
server-side configuration value, in seconds, and it is not carried in the token,
because it protects the server and nothing the client presents should set it.

**It has a sensible default and it is tunable per game.** The default SHOULD be
five seconds: a client retransmitting at the send rate gets tens of attempts
inside it, which covers ordinary loss and a poor round trip with room to spare,
while cutting an attacker's hold time to a fraction of a gameplay timeout. But
five seconds is a judgement about a population of players, not a property of the
protocol, and the population differs per game — a title with satellite or
cellular players wants more, a LAN or console title under attack wants less. So
the value is exposed as configuration, with the default chosen to be safe for a
game that never touches it. The same applies to the idle deadline and to the
client's total connect deadline: each has a default that works unattended, and
each can be tuned by a deployment that knows its players better than this
document does.

**On establishment the handshake deadline is discharged**, and the mapping is
governed by the idle deadline alone.

**Size the mapping table far above the slot count.** Mappings are small and
short-lived where slots are large and long-lived, so there is no reason to ration
them; a generous table decouples pressure on the handshake from capacity for live
players, and makes filling it a much more expensive thing to attempt. An
implementation MUST document the ratio it chose.

*What remains, and whose problem it is.* A peer holding a real token can complete
the whole handshake, take a slot, and then send nothing but keep-alives forever.
That is not a protocol defect: it is a connected client that the backend admitted
and that is behaving legally, and this layer cannot tell it from a player sitting
in a menu. The bound on it is the backend's issuance policy, and the decision to
drop it belongs to the game, which knows what a player ought to be doing. This
layer's obligation is to make it *visible* — see the counters below — not to
police it.

**The used-token history** is a fixed-size structure. An implementation MUST
document its capacity and eviction policy, and SHOULD size it to at least eight
entries per client slot with oldest-first eviction. Entries
expire with the tokens they name. Eviction is security-relevant: an attacker
holding several legitimately obtained tokens can flush a victim's entry and then
replay the victim's captured request from a different address, so capacity
should exceed the plausible rate of legitimate connections within a token
lifetime.

**Rate limiting.** A deployment SHOULD rate limit connection requests per source
address, and MUST NOT treat that as sufficient: connection requests are
unauthenticated and their source address is spoofable, so per-source limiting
does not bound the attacker it most needs to stop. An implementation MAY cache
recently verified (signed data hash → verified) results and short-circuit the
signature check on a byte-identical repeat, which is sound because it accepts
only data already verified.

### Processing Connection Response Packets

* The packet has already opened under this mapping's handshake key, which is all
  the response has to prove: the address was validated in step 1 and the token was
  verified in step 2.
* If a client from the source address and port is already connected, ignore it.
* If a client with that client id is already connected, ignore it.
* If no slots are available, respond with a _connection denied packet_ under
  the handshake keys, carrying the mapping's session nonce, and stop. The
  mapping is left in place, so a retransmitted response is denied again.
* Assign a slot, copy the user data from the encryption mapping into it, mark
  the client unconfirmed, and respond with a _connection keep-alive packet_. If
  this keep-alive is lost, the ones generated at the send rate repeat the
  answer.

### Connected Clients

A server has a fixed number of client slots, configured at startup. A slot is
identified by its **client index**, an integer from zero to one less than the
slot count, and a slot holds the client id and user data recovered from the
connect token, the encryption mapping for that client's address, and the
sending sequence and replay state for each key phase.

**The confirmed flag.** A slot is marked unconfirmed when it is assigned. It
becomes confirmed the first time the server receives any sealed packet from that
client on that slot — a keep-alive or a payload. The flag exists because the
server has not yet observed that the client received its keep-alive, so it does
not yet know the client considers itself connected.

**While a client is unconfirmed, the server prefixes every payload packet it
sends to that client with a keep-alive packet.** The keep-alive carries the
client index and the slot count, which is how the client learns its own index,
so a client that lost the first keep-alive would otherwise receive payloads it
cannot interpret. Once confirmed, the server sends payloads alone.

**Keep-alive generation.** The server sends a keep-alive to every connected
client whenever the send rate interval has elapsed since it last sent that
client anything. The client does the same toward the server once connected.
Keep-alives are therefore suppressed by ordinary traffic and appear only when a
connection is otherwise idle.

**Establishment on the client.** A client transitions to _connected_ on the
first keep-alive it receives, taking its client index and the slot count from
that packet.

**Payload delivery.** A payload packet carries application bytes with no
interpretation. Payloads received on an established connection are delivered in
the order they arrive; this layer does not reorder, retransmit, or deduplicate
beyond refusing replays.

**Timeout.** An established connection times out at either end when no packet
has been received from the peer for the connection timeout interval. The server
resets the slot and destroys the encryption mapping; the client transitions to
_connection timed out_.

**Deliberate disconnect.** An end that disconnects on purpose sends ten
redundant disconnect packets, sealed, at consecutive sequence numbers, and then
immediately releases its state without waiting for any acknowledgement. The
redundancy is because a disconnect is never retransmitted — it is the last thing
sent — so its delivery is bought with copies rather than with time, and losing
all ten costs only a peer that waits for the timeout instead.

**Slot reset.** Releasing a slot clears the client id, the user data, the
sequence and replay state, and the confirmed flag, and destroys the associated
encryption mapping. A released slot is immediately available for reuse. An
implementation MUST zero the key material rather than merely marking the slot
free.

## Observability

A protocol this layered fails in ways that are invisible from outside. A client
that will not connect looks identical whether the title tag was wrong, the cookie
never arrived, the signature failed, or every slot was full — and at scale, logs
are the wrong instrument for telling them apart, because the volume that makes a
problem visible is the same volume that makes logs unreadable.

Three instruments are required, and they sit at three different timescales:

| Instrument | Timescale | Answers |
|---|---|---|
| **Counters** | cumulative, whole run | how often does this happen |
| **Connection state** | instantaneous | where is this connection now, and what stopped it |
| **Statistics** | sampled per frame | what is the trend |

Connection state is specified in _Client State Machine_. This section covers the
other two.

### Counters

**An implementation MUST maintain one counter per distinct reason a packet is
rejected**, not one per category. `wrong size`, `token expired` and
`signature did not verify` are three counters and never one
`connection_request_errors`. The granularity is the entire value: a single
error counter tells you something is wrong, which you already knew, while a
counter per reason tells you which check refused and therefore where to look.

Counters follow these rules:

* **One flat namespace**, `ROCKETNET_COUNTER_<SUBJECT>_<OUTCOME>`. No hierarchy,
  no tags, no labels.
* **The outcome vocabulary is small, closed, and reused across every subject**:
  `RECEIVED`, `WRONG_SIZE`, `WRONG_VERSION`, `WRONG_PROTOCOL_ID`, `EXPIRED`,
  `DID_NOT_VERIFY`, `DID_NOT_OPEN`, `ALREADY_RECEIVED`, `ALREADY_CONNECTED`,
  `NO_MAPPING`, `NO_SLOTS`, `ACCEPTED`. Reuse is what keeps a hundred counters
  memorable, and lets a reader predict a name instead of grepping for it.
* **Reasons are cross-producted with packet type**, not shared. A replayed
  payload and a replayed keep-alive are separate counters even though the reason
  word is the same.
* **Arrival is counted separately from outcome.** Each packet type has a
  `RECEIVED` counter incremented once on entry, and then exactly one outcome
  counter. This makes `RECEIVED == Σ outcomes` an invariant that can be checked
  by eye and asserted in tests, and it is how you tell "the check rejected it"
  from "it never reached the check".
* **Names state the observed condition**, never a severity and never a metric
  type.
* **Counters are monotonic and MUST NOT be reset.** Rates are derived by
  differencing snapshots. A counter that resets cannot support the assertion
  that something never happened, which is the most valuable assertion of the set.
* **Storage is per-execution-context**, summed only at readout — per-CPU in a
  kernel dataplane, per-thread in userspace — so the hot path never touches a
  shared cache line.
* **Every counter site SHOULD also log**, with the log compiled out in
  production and the counter never compiled out. The log explains one incident;
  the counter reveals the pattern.
* **Indices, where counters are exposed as a fixed-width array, are a wire
  contract.** They are never renumbered, new counters take unused slots, and an
  implementation MUST reject a report whose width it does not recognize.
  Duplicate indices MUST be checked mechanically: two counters sharing a slot
  sum into one number, and a rare rejection sharing a slot with a routine
  success is simply erased.

**The connection request path is the one that matters most**, because it is the
attack surface and because it has the most ways to fail. Every step of the
ordered list in _Processing Connection Requests_ gets its own counter, in that
order:

| Counter | Fires when |
|---|---|
| `CONNECTION_REQUEST_RECEIVED` | a request reaches the handler |
| `..._WRONG_SIZE` | not 704 bytes after the prefilter header |
| `..._ALREADY_CONNECTED` | source address already holds a slot |
| `..._CHALLENGE_RESENT` | mapping exists; stored challenge resent |
| `..._WRONG_VERSION` | version mismatch |
| `..._WRONG_PROTOCOL_ID` | protocol id mismatch |
| `..._TOKEN_EXPIRED` | expire timestamp has passed |
| `..._BAD_ADDRESS_FLAGS` | reserved bits set, or no family named |
| `..._ADDRESS_NOT_ADVERTISED` | names no address this server answers at |
| `..._WRONG_SERVER_KEY` | server public key is not one held |
| `..._NO_COOKIE` | cookie field zero — the ordinary first request |
| `..._COOKIE_DID_NOT_VERIFY` | cookie MAC mismatch |
| `..._COOKIE_EXPIRED` | cookie past its expiry |
| `..._TOKEN_ALREADY_USED` | token identifier bound to another address |
| `..._SIGNATURE_DID_NOT_VERIFY` | Ed25519 verification failed |
| `..._COULD_NOT_OPEN_SEALED_BLOCK` | seal open failed |
| `..._SEALED_MISMATCH` | inner protocol id or expire disagrees with signed |
| `..._CLIENT_ID_ALREADY_CONNECTED` | that client id already holds a slot |
| `..._TOKEN_ALREADY_CONNECTED` | token already admitted a connection this run |
| `..._SMALL_ORDER_KEY` | `crypto_scalarmult` refused the peer key |
| `..._NO_SLOTS` | slot table full; a denied packet was sent |
| `..._COULD_NOT_ADD_MAPPING` | mapping table full |
| `..._SENT_CHALLENGE` | success — mapping created, challenge sent |

The three cookie counters are not failures in the ordinary sense: every
legitimate connection increments `NO_COOKIE` exactly once, and their sum is the
number of cookie packets sent. Their separation is diagnostic —
`COOKIE_DID_NOT_VERIFY` climbing means injected cookies, a rotated key, or a bug,
and it means something entirely different from `NO_COOKIE` climbing.

The same treatment applies to connection responses and to each sealed packet
type, and to the lifecycle events that no packet triggers:

* `MAPPING_CREATED`, `MAPPING_IDLE_TIMEOUT`, `MAPPING_TABLE_FULL`
* **`MAPPING_HANDSHAKE_DEADLINE_EXPIRED`** — a handshake started and never
  finished
* `CLIENT_CONNECTED`, `CLIENT_DISCONNECTED`, `CLIENT_TIMED_OUT`
* `INTEGRITY_LIMIT_REACHED`

**`MAPPING_HANDSHAKE_DEADLINE_EXPIRED` is the direct observable for the stalled
handshake described above**, and it is the counter to watch. A deployment under
that attack shows a large and climbing count here with no matching growth in
`CLIENT_CONNECTED` — peers starting handshakes and walking away — and there is no
other signal that says so. In ordinary operation it is near zero, which is what
makes a rise legible.

### Statistics

An implementation SHOULD expose a flat structure of per-frame quantities, read
by the application each update: round trip time, jitter, packet loss each way,
packets sent and received, packets lost, packets out of order, and bandwidth up
and down. These are for graphing, and they are what lets an integrator see what
the library is doing rather than infer it.

The structure is plain data — scalars only, no pointers, no nesting, no
allocation, returned by const pointer — so it costs nothing to read every frame
and crosses the C API without marshalling.

### Why this is in a protocol standard

Counters and states look like implementation detail, and the naming of them
partly is. They are here because two implementations that reject the same packet
for the same reason should say so in the same words: a deployment running a
third-party server should be diagnosable with the knowledge that this document
gives, and a bug report that says `COOKIE_DID_NOT_VERIFY is climbing` should mean
the same thing to everyone reading it. An implementation MAY add counters; it
MUST NOT merge two of the reasons listed here into one.

## Development without a backend

A developer's first use of this protocol is a server on a box with a public
address and a client pointed at it. **That must work with no backend, no
configuration and no copying of keys between machines**, because every hoop
between downloading the library and seeing a connection is paid by every
customer, forever, and first impressions of a transport are formed in the first
ten minutes.

Two things stand in the way of the obvious flow, and both are key distribution.
A token must be signed by a backend the server trusts, and it must be sealed to
the server's own public key — so a naive development story is "run a keygen,
copy the backend public key to the server, copy the server public key to
whatever mints your token, then start both." That is four steps and two file
copies before a single packet moves.

**Both keys are therefore published in this standard as fixed test values,
identical for everyone.** These are the values, secret halves included —
publishing the secret halves is the point:

    test backend signing public key (Ed25519):

        b0 ab 03 b3 e2 6e f1 7c 72 e3 bd 99 29 a5 b4 bf
        48 bd ba 50 57 3f 1b 8d 08 4a 08 d8 58 bd 24 2c

    test backend signing secret key (Ed25519):

        75 29 02 15 e1 ff 4d f2 09 92 57 8a 22 3c 7c d4
        63 b9 a2 7c f8 3a a7 1c 7a 16 93 1c 01 9b 7c 30
        b0 ab 03 b3 e2 6e f1 7c 72 e3 bd 99 29 a5 b4 bf
        48 bd ba 50 57 3f 1b 8d 08 4a 08 d8 58 bd 24 2c

    test server key exchange public key (X25519):

        3d 24 90 88 9d 37 4c 29 6f ab 1d 57 70 cb 71 47
        6a 06 f7 4e f2 f2 54 0c f2 21 5f 76 14 f4 80 1c

    test server key exchange secret key (X25519):

        6f fb 8f 73 37 ec 37 76 97 f5 68 de 31 18 d8 39
        59 56 0b 25 92 8d 22 70 7b a2 29 75 89 d0 8b 97

The Ed25519 secret key is in libsodium's 64-byte form, the seed followed by the
public key. The test title tag that accompanies these keys is given in _The
Prefilter Header_.

That reduces development to what it should be:

    ./server                       # uses the test server keypair
    ./client 203.0.113.10:40000    # mints its own token, sealed to it

The client mints a token locally with the published backend secret key, naming
the address it was given, and seals it to the published server key. No backend
runs, nothing is generated, nothing is copied.

**The safety property is that the keys are public, not that a flag is off**, and
that distinction is the whole design. A mode flag that disables checks behaves
identically in development and in production, so nothing fails when it ships by
accident — the bug is discovered by an attacker. Published keys fail in exactly
the right direction: a deployment that leaves them enabled has an authorization
system every one of its players can forge, which is not a subtle condition, and
any adversarial look finds it immediately. There is no state in which the test
keys are quietly load-bearing.

**The test keys are embedded in the library and used by default.** A developer
who configures nothing gets a working connection, which is the entire objective;
requiring configuration to reach the working path would put the hoop back where
it started. Supplying real keys is what takes an act, and that act belongs to
the deployment, not to the person trying the library for the first time.

The rules that keep it honest:

* An implementation MUST warn, unmissably and on every start, whenever either
  test key is in use. This is the one thing standing between a default that
  helps everybody and a default that ships, so it is a MUST and not a SHOULD,
  and it should be hard to filter out of a log.
* An implementation SHOULD refuse to use either test key in a build configured
  for release, and where it does, SHOULD NOT allow configuration alone to
  override that refusal.
* A deployment that supplies its own backend public key MUST NOT also accept the
  test one. Accepting both would mean every player could forge a token for a
  server that looks correctly configured, which is the worst of both worlds.

**Nothing else is relaxed, and that is the point.** The handshake, the cookie
exchange, the address validation, the sealing, the key derivation, the replay
protection and the erasure requirements all run exactly as specified. A
development server is a real server that trusts a public signing key, so the
code path exercised on a laptop is the code path that ships. This document asks
transports elsewhere not to have defences that only run under attack; the same
principle applies to itself.

## Security properties

What a conforming implementation guarantees, in one place. Each row names the
mechanism; a change to any mechanism MUST be checked against the property it
carries.

| Property | Mechanism |
|---|---|
| Only backend-authorized clients connect | Ed25519 signature, verified before the seal is opened |
| Session keys confidential in transit | sealed to the server's public key |
| Client identity hidden from observers | client id inside the sealed block |
| Token fields unmodifiable | signature over the whole signed buffer |
| No asymmetric work for a spoofed source | MAC cookie required before any signature verification |
| No slot for a client that is not there | challenge answered from the claimed address before any slot |
| Packet type and salt unmodifiable | AEAD associated data |
| Packet replay refused | per-phase, per-direction sequence window; sequence is the nonce |
| Token reuse from another address refused | used-token history keyed on a hash of the signed data |
| A key or nonce never outlives one connection | keys derived per connection from a fresh salt and a fresh session nonce |
| No response larger than what provoked it | the amplification table, re-checked on any size change |
| Recorded traffic stays closed after any later compromise | ephemeral X25519 exchange; secrets erased at derivation |
| A compromised server mints nothing | servers hold no signing key |

**The deliberate cost this design carries: rejecting a request that reaches the
signature check costs an Ed25519 verify plus a seal-open, on the order of a
hundred microseconds — roughly a hundred times a symmetric check.** That cost is
the price of servers that hold no shared secret, and the reason it is safe to
pay is that it is reachable only from an address that has proven it can receive:
a request without a valid cookie is refused after one keyed hash. The prefilter,
the used-token history and per-source rate limiting are all real and all
specified above, but none of them binds a spoofing attacker, and this document
says so where each is described. The cookie is the one that does.

### Security considerations specific to this design

**A sealed box proves nothing about its sender.** Repeated here because it is the
one way this design fails quietly: anyone can seal a block to a server's public
key. An implementation that treats a successful open as authenticity lets any
party mint tokens for that server. Verify, then open.

**Session traffic has forward secrecy; the handshake does not, and that is the
right split.** Recovering a token, a server's X25519 secret, or even the
backend's signing key at some later date does not decrypt recorded game traffic,
because the session keys also require an ephemeral secret that existed only for
the duration of the connection and was never transmitted. What such a compromise
still yields is the handshake packets — a cookie and a client index — which
carry nothing worth recovering.

*The exposure this closes is wider than it first appears.* The token keys are
generated by the backend and travel inside the connect token, so without the
ephemeral exchange the readers of a session's traffic would include: the backend, permanently and for
every server it ever issued for, including player-hosted ones it does not
control; anyone who obtained a server's X25519 secret at any later date; and
anyone who found a connect token in a proxy cache, a log or a crash dump. That
is a stronger trust anchor than a certificate authority, which can impersonate
but cannot decrypt. It now costs 64 bytes across two handshake packets, two
scalar multiplications per side, and **nothing at all per game packet**.

**Connection-request replay is a denial of service against one client, not an
authentication break.** An on-path observer can capture a connection request and
replay it from its own address; it cannot complete the handshake, because the
challenge returns sealed under a key it does not hold. If its replay arrives
first, the used-token history binds that token to the attacker's address and the
legitimate client must obtain a fresh token. It
requires an on-path position, from which simply dropping the client's packets is
a more effective attack — but it is not equivalent, because it is harder to
attribute: dropped packets look like a broken network and get investigated as
one, while this looks to the player like the game's own backend misbehaving. Mild
in effect, quieter than the alternative.

**No response may be larger than the packet that provoked it.** This is a
normative invariant, not inherited advice, and **every packet type MUST be
re-checked against it whenever any size changes.** It is easy to break by
accident and hard to notice, because shrinking one packet quietly changes some
other pair's margin: the _connection response_ carries 24 bytes of required
padding for exactly this reason — without it the response would be smaller than
the keep-alive it provokes, and a connected client would be a small reflector.

| Provoking packet | Size | Response | Size | |
|---|---|---|---|---|
| connection request | 708 | connection cookie | 36 | ok |
| connection request | 708 | connection challenge, fresh or resent | 64 | ok |
| connection request | 708 | connection denied | 32 | ok |
| connection challenge | 64 | connection response | 48 | ok |
| connection response | 48 | connection keep-alive | 32 | ok |
| connection response | 48 | connection denied | 32 | ok |
| connection cookie | 36 | *nothing* | — | see _Connection cookie packet_ |

Two rules keep the table true rather than merely accurate today. **Padding used
to satisfy it MUST be required on read**, checked for both length and content,
because padding that is optional is padding an attacker omits. And **a server
MUST NOT answer a request it has rejected** — every failure path in _Processing
Connection Requests_ ignores the packet silently, and the single exception is the
cookie, which is 36 bytes against 708 and is the price of address validation
having to start somewhere. The widest margins are a 708-byte request answered by
a 64-byte challenge or a 36-byte cookie. At those ratios neither response is
usefully a reflection primitive: an attacker spoofing a victim's address to place
64 bytes there spends 708 to do it, an eleven-to-one loss, and would do strictly
better sending those 708 bytes at the victim directly. The cost of a spoofed
request is CPU, which the cookie exchange addresses, rather than reflected bytes.

**Client proof-of-work was considered and declined.** Requiring a client to
solve a puzzle before connecting, with difficulty scaled up under attack, is a
recurring proposal and it is the wrong tool here. The cost falls about equally
on an attacker and on a legitimate client, but the attacker commands a botnet's
aggregate compute and is indifferent to latency, while the player is on a
laptop, a console or a phone on battery and is watching a loading screen — so
the tax is regressive against exactly the clients least able to pay it. It also
misses every threat that matters: it removes not one byte of a volumetric flood,
the cookie already defeats a spoofed one more cheaply, and an attacker with real
addresses needs real connect tokens to get past the bar at all.

That last point is the decisive one. **An attacker holding valid tokens is a
problem for the backend, not for this layer.** The backend knows who requested
them and can limit issuance per account; a server sees only an address. A puzzle
here would duplicate, worse, a control that belongs upstream where identity is
known. Note also that TLS, DTLS and QUIC all considered client puzzles and none
adopted one. Proof-of-work would only be worth its cost in a design with no
address validation at all, where it substitutes for proving the client is really
there — and this protocol has the cookie for that.

### What this layer defends against, and what it does not

Stated plainly, because a protocol document that lists defences without naming
their limits invites an operator to deploy it believing the problem is solved.

**This layer handles:** traffic that is not this title's, at line rate, for a
byte comparison. Spoofed source addresses, for one keyed hash and no allocated
state. Clients the matchmaker never authorized, by a signature they cannot
produce. Zombie clients that never answer, which never reach a slot.
Amplification, structurally, since no response may exceed the packet that
provoked it. Tampering with any packet or any token field, by construction.

**This layer cannot handle, and does not pretend to:** a volumetric flood large
enough to fill the link. Nothing a server does with a packet matters once the
pipe is saturated; that is answered by capacity and by upstream scrubbing at the
provider, and it is where a kernel whitelist would eventually help by moving the
drop as early as the machine allows. Nor can it handle an attacker holding many
genuinely issued connect tokens, because such an attacker is past every bar this
protocol has by definition.

**Both of those are the backend's to answer, and that is the deliberate
division.** The backend is the only party that knows *who* is asking — an
account, a session, a payment history — where a server knows only an address.
Rate limiting by identity is a control that works; rate limiting by address is
one an attacker routes around. So the real denial-of-service posture of a
deployment lives in how its backend issues tokens, not in how its servers parse
packets. This protocol's job is to make sure that anyone the backend did not
send is rejected as cheaply as possible, and that being rejected is never more
expensive for the server than for the attacker.

**It is also not a new responsibility.** A backend already decides whether a
player owns the game, whether they are who they claim to be, and whether they
are banned. The connect token is that decision made concrete: it is the backend
saying *I vouch for this player, on this server, until this timestamp*. Declining
to vouch for someone who is abusing a server is the same gate the backend is
already standing at, applied for one more reason — not a denial-of-service
mechanism grafted onto a system that had none. Everything a server enforces here
is downstream of a judgment the backend was making anyway.

**And it is delegation, not abdication, because the delegated control is made to
work.** A protocol can push a problem upstream and leave the
upstream party holding a lever connected to nothing. This one does not: **a
connect token buys exactly one connection.** The used-token history binds a
token to the first source address that presents it, a token whose client has
connected is refused for the remainder of the server run, and a client id
already connected is refused. An attacker who wants a thousand concurrent
connections therefore needs a thousand genuinely issued tokens, and their cost
scales linearly with the number of times they can authenticate to the backend.
That is what makes issuance limiting an effective control rather than a hopeful
one.

**What the backend must do to hold up its end**, stated so a deployment does not
discover it during an incident: issue tokens with short expiry, since expiry
bounds both the reuse window and the revocation latency; rate limit issuance per
authenticated identity rather than per address; and refuse issuance to
identities that abuse it.

**The backend and the game servers may talk to each other, and probably will —
and that is not this standard's business.** It is not our backend and not our
game. A title's backend and its fleet will have whatever relationship the title
needs: health reporting, allocation, bans, shutdown, abuse signals. None of it is
specified here, none of it is required here, and a flood of validly issued
tokens is handled there.

**That is the connect token's quieter purpose, and it is a product property as
much as a security one: the token gets this library out of the loop.** A
customer runs their own backend, authenticates their own players, enforces their
own bans, and orchestrates their own servers. The only thing that has to cross
from their world into ours is a signed token, and the only thing we have to do
with it is verify it. We never need to know how they decided, never need to be
told when they change their minds, and never need to run anything on their
behalf. That is what makes this a library a customer adopts rather than a
service they depend on — and it is why removing the backend-to-server shared
secret matters beyond cryptography: a shared secret would have put us back in
the middle of a relationship that is theirs.

**The backend is the real admission control.** A server allocates an encryption
mapping only for a request carrying a valid backend signature, so the number of
mappings an attacker can create is bounded by the number of real tokens they
hold, not by the packets they can send. That is a strong bound and it is the
backend's to maintain: **a deployment that issues connect tokens freely has
removed the only limit that works against a determined attacker.** Token
issuance policy is outside this standard and inside its security argument.

**Bound the token lifetime.** A deployment SHOULD keep `expire - create` short —
minutes, for a matchmaker that assigns a server at request time. Token lifetime
is the revocation latency of this design: if a server's X25519 secret key leaks,
tokens already sealed to it stay openable until they expire, and nothing can
recall them. It also bounds how large a used-token history must be to be useful.

**Clamp the timeout.** `timeout seconds` is signed, with negative disabling
timeouts for development. It is inside the signed data so a client cannot forge
it, but a backend bug or a leaked development token can produce a connection
that never times out and never frees its slot. A server SHOULD hold a configured
maximum and clamp the token's value to it, and SHOULD reject non-positive values
unless explicitly configured to accept them.

**Require a strict Ed25519 verification profile.** Ed25519 verifiers disagree in
practice on non-canonical scalars, small-order points, and cofactored versus
cofactorless equations, so two implementations can differ on whether a signature
is valid. A conforming implementation MUST reject non-canonical `S` values and
small-order public keys — the behavior libsodium's `crypto_sign_verify_detached`
already has. This matters more here than in a closed implementation, because this
document invites independent ones.

**The AEAD is not swappable.** ChaCha20-Poly1305's confidentiality limit is
larger than the number of packets a connection can carry, which is why this
protocol needs no key update mechanism at all. That reasoning does not survive
substitution: AES-GCM's confidentiality limit is about 8.4 million encrypted
packets, which at the packet rates this standard contemplates is **minutes**, and
responding to it requires a key update this protocol does not have. An
implementation MUST NOT substitute another AEAD without adding one.

**There is no 0-RTT and no session resumption, deliberately.** Both add a replay
surface that the standards containing them spend chapters bounding, and the
strongest advice in those chapters is that disabling the feature is the most
effective defense. A game connects once per match, so the round trip it would
save is not worth the class of attack it opens.

**No stateless reset, for a structural reason rather than an oversight.** A
peer's crash is discovered here by timeout, where QUIC discovers it immediately
via a stateless reset token. That mechanism requires a static key shared across
a server fleet — which is exactly the fleet-wide shared secret this design exists
to eliminate. Omitting it follows from the central commitment. Graceful close is
already covered by the sealed, redundantly sent _connection disconnect_; only the
crash case waits out a timeout, and a game reporting a lost connection after a
few seconds is acceptable.

**If connection migration is ever added, three things must arrive together.** A
connection identifier alone would be a regression. It needs path validation
before the server sends anything to a new address, or an off-path attacker who
reads the identifier can redirect a session onto a victim; the cookie in this
protocol is already exactly that primitive. It needs header protection or
identifier rotation, or the identifier becomes a permanent cleartext tag that
follows a player across every network they join — strictly worse for privacy than
today's address keying, which offers nothing to link. And **if header protection
is ever added, the replay check must move to after the open**: it is placed
before the open here precisely because the sequence is already in the clear, so
an early drop leaks nothing and costs a window lookup instead of an AEAD.

## Conformance

An implementation conforms if it produces and accepts the exact binary formats
above, performs the processing steps in the exact orders given, and holds the key
inventory of the _Keys_ section — in particular, no secret shared between backend
and servers, and signature verification before the sealed block is trusted.

The prefilter header must be present and correctly formed on every packet an
implementation sends, and its derivation must not depend on the sender's own
source address. Whether a receiver checks it in userspace, in a kernel program,
or not at all is a quality-of-implementation choice, since the sealed layer
beneath it carries the security guarantees.

### Demonstrating conformance

The ordered check lists in this document are a test plan, and the counters of the
_Observability_ section are what make it executable. Each numbered step is one
negative test: construct a packet that fails exactly that check, and assert that
exactly that counter fired.

* **A negative test MUST assert two counters, not one**: the reason counter, and
  the packet type's `RECEIVED` counter. The reason alone is not enough — a test
  that asserts only "rejected" still passes when the packet never reached the
  check at all, which is how negative tests silently rot into testing nothing.
  Asserting arrival rules out the whole upstream path having eaten the input.
* **A positive test asserts the success counter and independently observes the
  result** — the packet on the wire, the state transition, the slot assigned.
  The counter says why; the observation says whether.
* **The strongest tests pair a must-fire with a must-not-fire.** Asserting that a
  rejection counter rose *and* that the corresponding success counter stayed at
  exactly zero is a far stronger claim than watching a socket and seeing nothing
  arrive, which only proves that one listener missed it in one window. It is
  available only because counters never reset.
* **A drop is unobservable by construction.** Without counters every rejection
  test collapses into the same assertion — "nothing came back" — which cannot
  distinguish twenty different reasons, several of which are bugs.

**A reason counter with no test naming it is an untested branch.** The counter
list and the test list should be the same list, and diffing them is a coverage
check that costs nothing and does not depend on a coverage tool understanding the
kernel dataplane.

**Every rule in this document that exists because of a specific failure needs a
test that reproduces that failure.** Much of what is written here is of the form
"do X, because otherwise Y" — the response carries required padding or a
connected client becomes a reflector; the session nonce is chosen at mapping
creation or a replayed cookie makes a key recur; the handshake deadline never
refreshes or a peer stays half-open forever. Each of those `Y`s is a situation
that is hard to stumble into and easy to reintroduce, and the prose explaining it
protects nothing once the code is being changed for some other reason.

**The failure mode this guards against is repairing one defect and breaking
something essential in the process**, and it is not a careless-programmer
problem — it is what happens to someone concentrating hard on a different part of
the protocol. These rules interact: a change to how mappings are replaced touches
key derivation, a change to a packet's size touches the amplification invariant,
a change to when a nonce is chosen touches replay. The person making the second
change is being careful about the second thing. Only a test is careful about the
first.

This is true for one author working alone across months, and more so for a team,
where the person making the second change may never have read the reasoning
behind the first and has no way to know it is there. Prose in a standard is
available to whoever goes looking; a failing test arrives uninvited, which is the
only delivery that works.

Reproducing these paths is laborious — they are, by construction, the states a
correct implementation never enters — and it is the work that makes the
difference between a protocol that is specified and one that stays correct.

The counter and the test do different halves of this: the counter proves the
intended branch was reached, and the test keeps it reachable and correct
thereafter.

**Known-answer test vectors are REQUIRED for interoperability.** A conforming
implementation publishes, and checks itself against, fixed vectors covering: a
connect token built from a fixed backend keypair and server keypair, the four
derived per-connection keys, one sealed packet of each type with its exact
associated data bytes, and one cookie MAC. Self-consistency proves an
implementation agrees with itself; only vectors prove two implementations agree
with each other. The failure this prevents is the expensive one — two correct
readings of an ambiguous sentence, and a handshake that silently never
completes.

## Appendix A: Known-answer test vectors

These vectors satisfy the requirement in _Demonstrating conformance_. Every
input is fixed and stated here; an implementation reproduces the deterministic
outputs exactly, and verifies the frozen token. All values are hex bytes in
wire order.

**Fixed inputs.** The test backend and server keypairs and the test title tag
are the published values from _Development without a backend_ and _The
Prefilter Header_. In addition:

    protocol id                 0x1122334455667788
    client id                   0x0123456789abcdef
    create timestamp            1756166400
    expire timestamp            1756166700
    timeout seconds             10
    token client-to-server key  01 02 .. 20   (bytes 0x01 through 0x20)
    token server-to-client key  a1 a2 .. c0   (bytes 0xa1 through 0xc0)
    connection salt             53 41 4c 54 30 30 30 31
    session nonce               4e 4f 4e 43 45 30 30 31
    client ephemeral secret     c1 c2 .. e0   (bytes 0xc1 through 0xe0)
    server ephemeral secret     e1 e2 .. ff 00
    server cookie key           10 20 30 40 50 60 70 80 90 a0 b0 c0 d0 e0 f0 00
                                0f 1f 2f 3f 4f 5f 6f 7f 8f 9f af bf cf df ef ff
    server address              203.0.113.10:40000 (IPv4, in the token)
    client source address       198.51.100.7:56789 (IPv4, for the cookie MAC)
    cookie expire               1756166403 (create + 3)
    user data                   256 zero bytes
    keep-alive fields           client index 3, max clients 1000
    payload plaintext           68 65 6c 6c 6f

The ephemeral public keys below are `crypto_scalarmult_base` of the stated
secrets; the shared secret is checked equal from both sides before use.

**Deterministic outputs.**

    client ephemeral secret (32 bytes):
    c1 c2 c3 c4 c5 c6 c7 c8 c9 ca cb cc cd ce cf d0
    d1 d2 d3 d4 d5 d6 d7 d8 d9 da db dc dd de df e0

    client ephemeral public (32 bytes):
    3a 55 3d 74 79 2d 72 7e fa 9b 9a 4c de 3d a1 ad
    93 f1 a2 d0 c0 9c b6 39 b1 a3 c0 fd a1 4c be 24

    server ephemeral secret (32 bytes):
    e1 e2 e3 e4 e5 e6 e7 e8 e9 ea eb ec ed ee ef f0
    f1 f2 f3 f4 f5 f6 f7 f8 f9 fa fb fc fd fe ff 00

    server ephemeral public (32 bytes):
    48 9c eb ba 7a 16 6c 61 98 35 fe 12 ee dc 58 1d
    bf c1 b3 6d e1 0c 83 0e 4f 0b c1 c4 87 34 86 4b

    shared secret (32 bytes):
    57 5f d8 2f 7c 53 8e 42 33 8f 74 8f 9d 35 39 ea
    01 ed 0d 38 4d 73 b3 02 36 3f b2 65 8f 1a 9f 1b

    handshake client-to-server key (32 bytes):
    4b 4d 63 0c db 00 c8 7b 73 d4 6b 34 21 d8 91 c2
    4c fd 0a b9 28 c9 87 51 5a ab bb d7 8b e2 73 50

    handshake server-to-client key (32 bytes):
    86 04 5b 5f 8e 2d a9 4d bf 44 5a 65 80 86 d6 3d
    50 b7 3b 1e 66 6c 76 c4 95 2f ab f9 98 c0 c0 e0

    session client-to-server key (32 bytes):
    ae dc 46 56 e3 f8 2b 7e 87 91 27 95 88 c7 f6 20
    4e d2 c1 bc 0c 17 ba 97 79 c0 4e 3b 1c 9e f4 3c

    session server-to-client key (32 bytes):
    3c 63 7e 37 b1 01 b8 2c 9e da a9 a3 b5 5a 31 a8
    a3 bc 10 8f 13 3b a0 5e 8a 41 6b 3c fb 73 0e f0

    cookie mac input (23 bytes):
    01 c6 33 64 07 d5 dd 53 41 4c 54 30 30 30 31 03
    f9 ac 68 00 00 00 00

    cookie mac (16 bytes):
    0f b2 b2 8f 32 db d6 c9 eb 36 4f 57 74 86 f3 e9

    == connection challenge (type 2, handshake s2c, seq 1) ==
    associated data (53 bytes):
    93 1a 7c 4e 88 77 66 55 44 33 22 11 02 53 41 4c
    54 30 30 30 31 3a 55 3d 74 79 2d 72 7e fa 9b 9a
    4c de 3d a1 ad 93 f1 a2 d0 c0 9c b6 39 b1 a3 c0
    fd a1 4c be 24

    sealed (48 bytes):
    68 6f b7 09 43 5f f0 0e 1a c2 60 ee ea fb b2 91
    20 20 10 86 c8 27 62 55 d7 d3 87 c3 92 32 94 7b
    3d a1 3d dd 68 56 b0 94 15 f3 08 6c 59 f7 6f 88

    == connection denied, response path (type 1, handshake s2c, seq 2) ==
    associated data (21 bytes):
    93 1a 7c 4e 88 77 66 55 44 33 22 11 01 53 41 4c
    54 30 30 30 31

    sealed (16 bytes):
    e7 12 b8 8e e2 36 cf d9 05 38 7c bc 0f 9d ee 13

    == connection response (type 3, handshake c2s, seq 1) ==
    associated data (53 bytes):
    93 1a 7c 4e 88 77 66 55 44 33 22 11 03 53 41 4c
    54 30 30 30 31 3a 55 3d 74 79 2d 72 7e fa 9b 9a
    4c de 3d a1 ad 93 f1 a2 d0 c0 9c b6 39 b1 a3 c0
    fd a1 4c be 24

    sealed (40 bytes):
    9c 08 60 98 96 b9 d4 05 d5 14 9e 1e d9 da e4 dc
    2b d2 4a e4 cd a1 93 10 5c bf a9 ce 74 61 b3 97
    ce 4b 50 c0 1b 2f 8f 7e

    == connection keep-alive (type 4, session s2c, seq 1) ==
    associated data (21 bytes):
    93 1a 7c 4e 88 77 66 55 44 33 22 11 04 53 41 4c
    54 30 30 30 31

    sealed (24 bytes):
    d1 48 d8 10 58 6a 78 86 ae 2e 0e 01 46 a4 5a ba
    a0 a0 1d da bd 24 f1 65

    == connection payload (type 5, session c2s, seq 3) ==
    associated data (21 bytes):
    93 1a 7c 4e 88 77 66 55 44 33 22 11 05 53 41 4c
    54 30 30 30 31

    sealed (21 bytes):
    8b 83 8d c7 a7 34 bc e2 37 04 65 48 35 a2 13 cf
    c2 f6 5a 12 11

    == connection disconnect (type 6, session c2s, seq 4) ==
    associated data (21 bytes):
    93 1a 7c 4e 88 77 66 55 44 33 22 11 06 53 41 4c
    54 30 30 30 31

    sealed (16 bytes):
    9a b0 80 30 1b 70 95 59 85 f3 97 88 87 d9 13 f5

    token identifier (16 bytes):
    07 c9 19 dc f8 2f 78 79 e5 91 70 48 3f fc 9a 1b

    frozen connect token (768 bytes):
    93 1a 7c 4e 88 77 66 55 44 33 22 11 00 f9 ac 68
    00 00 00 00 2c fa ac 68 00 00 00 00 0a 00 00 00
    01 cb 00 71 0a 40 9c 00 00 00 00 00 00 00 00 00
    00 00 00 00 00 00 00 00 00 3d 24 90 88 9d 37 4c
    29 6f ab 1d 57 70 cb 71 47 6a 06 f7 4e f2 f2 54
    0c f2 21 5f 76 14 f4 80 1c f6 7d 13 8d e8 d3 b7
    5b f7 d9 0d 3c 95 95 97 70 4d cc 75 1a 41 2a 27
    ee 54 e0 5c 39 f5 dd a5 48 4e bc b3 a3 ab 31 9e
    b4 92 ce c5 57 46 a4 95 c1 2d c8 7a 43 1f e5 11
    1c 8d 20 cf b1 9d c5 d3 e5 73 3e 0e a5 ac 4f d0
    1b 71 54 ae 1a 6b 5b 88 32 82 a7 ce 89 d1 7d 02
    9a 0a e1 fe cd aa ba 72 75 ea 8b 92 b1 65 5d 58
    36 73 c3 0e 84 9b 6f 14 43 1c 07 2b f2 ce 36 9b
    eb 4a c0 aa 4a 66 f0 fa 3b 3f fc 98 ee 27 ef a0
    ff db 81 2a 76 37 64 d9 0c bc 18 27 6b 9a 46 bd
    35 02 6c 7b fa 93 13 32 84 f7 a2 7b 8b ee 4f a3
    49 12 6c b5 0f 6a 3a fb b9 55 35 db e1 e3 68 a0
    6c 72 d8 2e cc 1b 75 e9 4a 8d b7 bd a1 c1 0c d2
    46 46 ce 61 ce 31 ac 10 33 4f a1 79 4c 9b 8a a3
    bf db a0 41 36 b4 a4 74 b8 92 c1 ed f5 6e 5d f5
    e0 0b c6 61 88 01 0c 6e f1 a1 c2 08 7e 2e c3 1f
    4b 11 68 ce da fd f6 08 f1 17 06 ae e8 8c 79 29
    e6 11 09 fa 96 48 cb 0d 5e 37 d7 06 0e d6 22 85
    b7 09 39 7a 62 4e f2 85 ad c5 44 e2 3e 98 f1 d4
    55 95 3b 1a df 41 58 7d cb e0 df 37 cc 70 58 72
    c4 3f 36 64 f9 78 06 f1 20 b1 d1 98 e7 1c e1 35
    ff 2a a1 3f e3 b4 d2 9a 00 0f 30 ba 0c 15 2f 33
    04 3c 94 cc 55 a3 e4 d2 d1 9c 18 e7 63 41 48 4d
    42 1b 3f f3 40 00 9c 2e 98 36 84 74 30 30 74 a5
    5d 44 0c c9 cb 2b 88 c6 33 36 4b 94 ab bc 72 3f
    80 c5 67 dd ed e8 99 7d 1b c2 a3 8d bf c7 c5 37
    e0 fe 2d 5f f4 8a be 9e 7a 36 76 25 c3 93 02 ae
    c8 8d 9c fd 83 82 4e 82 47 00 00 00 00 00 00 00
    00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
    00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
    00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
    a2 a2 b2 3b c1 e8 da a2 44 c1 60 02 fb 32 eb 25
    f0 d5 e0 12 d9 be 9b 29 a7 b1 1e 9e 69 76 e9 1e
    6f 06 e4 89 9f 51 f5 20 ba fc cc d6 df c1 36 da
    51 91 fa 44 57 18 46 8d 7f 88 86 34 39 58 fd 0a
    01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f 10
    11 12 13 14 15 16 17 18 19 1a 1b 1c 1d 1e 1f 20
    a1 a2 a3 a4 a5 a6 a7 a8 a9 aa ab ac ad ae af b0
    b1 b2 b3 b4 b5 b6 b7 b8 b9 ba bb bc bd be bf c0
    00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
    00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
    00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
    00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00

**The frozen connect token** above is not reproducible byte-for-byte — sealing
draws randomness — so its vector runs the other way: a conforming server-side
implementation verifies its signature against the test backend public key,
opens its sealed block with the test server keypair, and recovers exactly the
private fields listed in the fixed inputs. The token identifier printed above
is the 16-byte unkeyed `crypto_generichash` of its first 576 bytes and is
deterministic.
