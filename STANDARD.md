# RocketNet 1.0 — connection standard (draft)

**RocketNet** is a protocol for creating secure client/server connections over
UDP. This document is the security and connection standard, so people can write
their own conforming implementations. It is the packet-level connection layer
only; reliability, flows and broadcast ride above it in payload packets and are
specified separately.

It is netcode 1.02 rebuilt on public key cryptography, and simplified where the
simplification costs no security. The shape is netcode's — connect token,
two-RTT handshake, address challenge, sealed packets, per-client slots. Two
things change and they run through everything below.

**There is no secret shared between the backend and the dedicated servers.** The
backend signs connect tokens with a private key that never leaves the backend;
it seals the session keys to the target server's own public key. A dedicated
server holds only the backend's public key and its own keypair. A compromised
server cannot mint tokens, and leaks only its own sessions. This is what lets a
title run player-hosted servers on hardware the backend operator does not
control — netcode's fleet-wide shared private key cannot.

**Every connection derives fresh keys.** The client contributes a random salt to
each connection attempt, and both sides derive the working keys from the token
keys and that salt. netcode reused the token keys directly, which is the root of
its documented restart hazard and of an undocumented one on reconnect. Deriving
per connection removes both, and removes the machinery netcode needed to manage
them.

Crypto is libsodium. Nothing here needs a primitive outside a stock build:

* Ed25519 signatures (`crypto_sign`) — backend token authenticity.
* Sealed boxes (`crypto_box_seal`, X25519) — delivering the session keys to the
  server confidentially through the untrusted client.
* BLAKE2b key derivation (`crypto_kdf`) — per-connection keys.
* BLAKE2b keyed hashing (`crypto_generichash`) — the address challenge cookie,
  and session key derivation.
* X25519 (`crypto_scalarmult`) — the ephemeral exchange giving forward secrecy.
* ChaCha20-Poly1305 IETF AEAD (`crypto_aead_chacha20poly1305_ietf`) — every
  sealed packet.

## Architecture

Three components, as in netcode:

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

There is no key in the system that is both shared and secret. This is the
load-bearing difference from netcode 1.02.

**The one channel that must still be trusted** is server provisioning: the
backend must learn each server's X25519 public key, bound to that server's
addresses. That registration MUST be integrity-protected and authenticated to
the backend. If an attacker can register their own public key against a
legitimate server's address, the backend will seal real players' session keys to
the attacker. How registration happens is deployment-specific and outside this
standard; that it must be authenticated is not. (netcode's shared key arrived
through the same kind of channel, so this is not a new requirement — but it is
the trust anchor of the whole design and it is stated rather than assumed.)

## General Conventions

RocketNet is a binary protocol. All data is written in little-endian byte order
unless otherwise specified, including sequence numbers converted to nonce byte
arrays and associated data passed to the AEAD primitive.

Byte counts from libsodium, named so an implementation checks them against its
own headers rather than trusting this document:

    crypto_sign_BYTES                            64   // Ed25519 signature
    crypto_sign_PUBLICKEYBYTES                   32
    crypto_sign_SECRETKEYBYTES                   64
    crypto_box_PUBLICKEYBYTES                    32   // X25519
    crypto_box_SECRETKEYBYTES                    32
    crypto_box_SEALBYTES                         48   // 32 ephemeral pk + 16 mac
    crypto_kdf_KEYBYTES                          32
    crypto_kdf_CONTEXTBYTES                       8
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
conformance section, because it is the one way a public-key rebuild of netcode
fails quietly: an implementation that treats a successful open as authentication
lets any party mint tokens for that server.

## Addresses and dual stack

A connect token names **one server**, reachable at up to two addresses: an IPv4
address and an IPv6 address, each optional, at least one present. (netcode
allowed a list of up to 32 candidate servers per token; that multi-address
design was a mistake — a matchmaker assigns a server, and the only real
multiplicity is address family.)

The two addresses are two ways to reach one machine, **not two candidates to try
in turn**. The client selects the address whose family it can reach and uses
that address for the whole connection. If that address does not answer, the
client does not switch to the other one; it reports failure and the correct
response is to request a fresh connect token from the backend, which has better
information about server health than any stale token does.

*This is deliberate, and it closes a defect an earlier draft of this document
had. Because both addresses belong to one server, they share one used-token
history, and that history binds a token to the source address that first
presented it. A client that failed over between families would present the same
token from a different source address and be refused — locking itself out of
both paths. Failover between families is therefore not a client behavior; it is
the backend's job, exactly as failover between servers is.*

RocketNet is dual stack from day one, not as a later addition:

* A server MAY advertise both an IPv4 and an IPv6 address, and it accepts a
  connection request arriving at **either** of them. (netcode's dual stack was
  retrofitted, and for years its server compared tokens against only its first
  advertised address; RocketNet does not repeat that.)
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
timeout and must obtain a fresh connect token and connect again. netcode makes
the identical assumption, so this is not a regression — but a game whose players
are largely on mobile networks should know it is choosing this behavior.

QUIC removes this assumption with connection IDs: an identifier carried in every
packet, independent of the address, so a connection survives a path change. That
is the right design for a general-purpose internet transport, and it is a real
capability RocketNet does not have.

It is not free. QUIC's goal is *seamless* migration — no interruption, no round
trip — which requires the identifier to be present on every packet, plus a path
validation exchange so an attacker cannot redirect a session onto a victim's
address, plus connection ID rotation so a passive observer cannot use the
identifier to follow a client across networks. On a 24-byte packet overhead an
always-present identifier is a third again, paid by every player on a fixed
connection, for an event most of them never experience. RocketNet's targets are
PC and console games on fixed connections, and its keep-alives run often enough
that a NAT binding never goes idle — which removes the common cause of a
mid-session address change on such a network.

**Deliberate scope choice, and deliberately not a wire break to reverse.**
Migration needs an identifier only at the moment the path changes, not on every
packet. It can be added later as a new packet type: sealed under the existing
session keys, carrying an identifier so the server need not trial-decrypt against
every connected client, and followed by the same cookie round trip to the new
address that the connection made at connect time — which is the anti-redirection
protection QUIC's path validation provides. Cost on ordinary packets: nothing.
Cost when it fires: one round trip, rarely. **Packet types 7 through 255 are
reserved and MUST be ignored by a conforming receiver**, so a later
implementation adding migration degrades gracefully against an earlier one: the
packet is ignored and the client reconnects, which is exactly today's behavior.

The case that would justify revisiting this is a title whose players are
predominantly on mobile networks, where cellular-to-wifi handover is routine
rather than rare.

Higher-level transports may name endpoints by an opaque string rather than an IP
address (per REQUIREMENTS.md). How a non-IP transport encodes its endpoint in a
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
that fails for an unknown source (sealed packets). Even a 10 Gbps flood of
random small packets would put roughly one packet per twenty seconds in front of
a check that rejects it in a handful of instructions.

*What the earlier design spent for that.* netcode's Network Next ancestor
smeared a 64-bit hash across 15 bytes plus a 2-byte checksum, each byte
constrained to a range so the receiver could verify structure without computing
the hash. It is a clever construction and it delivers 2⁻⁶⁴ against random data —
but at 3.8 bits of filtering per byte, where a direct comparison delivers 8, and
2⁻⁶⁴ is roughly forty orders of magnitude more filtering than the threat calls
for. Seventeen bytes on every packet, forever, for protection the sealed layer
already provides. The header is 4 bytes here, of which 3 are the marginal cost
of filtering at all, because the type byte is needed regardless.

**The tag is generated per title.** A tool takes a per-title seed and emits the
filter as code in both forms from one seed — the userspace check and the XDP
program — so no two titles share a tag (issue #17). That is what per-title
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
to guess: the two-RTT handshake ends with the server holding the client's actual
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

So payload packets carry **1 to 1208 bytes** of application data.

Per-packet overhead is 24 bytes, against netcode 1.02's 18 to 25 — inside its
range, while carrying a prefilter header netcode has no equivalent of. A fixed
sequence costs a byte or two more than netcode's variable one in the common
case; the 4-byte prefilter and the retired variable-length encoding pay for it.

The connect token is a fixed 768 bytes, inside a single 1500-byte MTU with room
to spare, by design.

## Sequence numbers

Sealed packets carry a **fixed 4-byte sequence number**, sent in the clear
before the sealed data. Each direction of each connection has its own sequence,
starting at zero and incrementing by one per packet sent.

The value used as the AEAD nonce is the full **64-bit** sequence. The receiver
reconstructs the high 32 bits: it tracks the highest sequence it has
successfully opened, and takes the candidate whose low 32 bits match the wire
value and which is nearest that highest value. A wrong reconstruction produces a
failed open and a dropped packet, never a wrong acceptance.

*Why not netcode's variable-length encoding.* netcode packs a sequence of 1 to 8
bytes and puts the byte count in the high nibble of its prefix byte, so every
packet parse does nibble unpacking, a range check, and offset arithmetic that
depends on the result. That variable geometry was the direct cause of two
defects found in an earlier draft of this document: a missing size recheck after
the sequence was consumed, and a read order that dropped every connection
request because its type byte's high nibble is zero. A fixed field deletes the
encoding, the range check, the dependent arithmetic, and both defect classes.

*Why not a fixed 8 bytes.* RocketNet does not assume a 60Hz packet rate. A game
sending fragmented state at 10 Mbps sends tens of thousands of packets per
second per direction, where four extra bytes per packet is a percent of the link.
Four bytes on the wire with a reconstructed 64-bit value costs about ten lines
and never wraps.

*Why not two bytes.* At tens of thousands of packets per second a 16-bit
sequence wraps in seconds, leaving no room for reordering or loss bursts.

## Connect Token

A _connect token_ ensures that only authenticated clients can connect to the
dedicated server. It is netcode's connect token rebuilt on public keys: the
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

**The client id is inside the seal deliberately, as it is in netcode.** A
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
derive four keys with `crypto_kdf_derive_from_key`, using the salt as the subkey
id and the following 8-byte contexts:

    handshake client to server = crypto_generichash(
                                     32 bytes,
                                     salt ‖ server nonce ‖ "RKTNHNDC",
                                     key = token c2s key )

    handshake server to client = crypto_generichash(
                                     32 bytes,
                                     salt ‖ server nonce ‖ "RKTNHNDS",
                                     key = token s2c key )

**Both sides contribute, and that is what makes a repeated key impossible.** The
client's salt is chosen per attempt; the server's nonce is chosen per cookie and
never repeats within a run. A key therefore cannot recur unless the server
reissues a nonce, which it must not, and no sequence reset can produce a repeated
(key, nonce) pair however the mapping's lifecycle behaves.

*An earlier draft derived these from the salt alone, and it was wrong in a way
worth recording, because the mistake is natural. With only the client
contributing, the client — or anyone who can spoof it — decides whether a key
recurs. That draft tried to contain the consequence with a rule about when a
server may replace an encryption mapping, and the rule became the lever: an
attacker alternating two salts from a victim's address could force the server to
create, discard and recreate the same key with the sending sequence reset each
time, producing two plaintexts under one key at one nonce. A lifecycle rule
cannot be the guarantee, because lifecycles have more exits than the rule
enumerates — eviction under pressure and expiry-then-retry reach the same place
with no attacker at all. Making both sides contribute removes the need for the
rule to hold.*

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
                                          ‖ salt ‖ server nonce ‖ "RKTNSESC",
                                   key = token c2s key )

    session server to client = crypto_generichash(
                                   32 bytes,
                                   shared ‖ client ephemeral public
                                          ‖ server ephemeral public
                                          ‖ salt ‖ server nonce ‖ "RKTNSESS",
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
connection, and it deletes a great deal:

* **The restart hazard is gone.** netcode devotes a section to a client
  presenting an unexpired token to a restarted server and repeating nonces under
  the same key, and offers two optional mitigations that a conforming server may
  skip. A fresh salt per attempt means the keys differ, so the sequence spaces
  never meet. Both mitigations and the section are deleted, not documented.
* **The reconnect hazard is gone**, and netcode does not name that one at all: a
  client that reconnects with the same token from the same address gets a fresh
  slot with sequences back at zero and the *same* keys, replaying every nonce of
  the previous session under both. Nonce reuse in a Poly1305 construction is not
  only a confidentiality loss; it can expose the authenticator key and yield
  forgery. A fresh salt removes it.
* **The 2^63 hack is gone.** netcode starts the server's global sequence at 2^63
  so pre-slot packets cannot collide with per-client packets under one key.
  Separate handshake and session keys make the two spaces independent, so both
  simply start at zero.

Tampering with the salt in flight makes the two sides derive different keys and
the handshake fails. That requires an on-path attacker, who can drop the packets
instead; no key is exposed and nothing is accepted.

## The address challenge

The challenge stops a client with a spoofed source address from occupying a
slot. It is a cookie, not an encrypted token.

    [cookie expire timestamp] (uint64)
    [cookie mac] (16 bytes)

    cookie mac = crypto_generichash( 16 bytes,
                                     client address ‖ client id ‖ connection salt
                                                    ‖ cookie expire,
                                     server cookie key )

**There are two distinct cookies and they are not interchangeable.** The
*address-validation cookie* (domain byte `0x01`) is issued by the cookie packet
before any token processing, and cannot include the client id because the sealed
block has not been opened yet. The *challenge cookie* (domain byte `0x02`) is
issued with the challenge, after the client id is known, and binds it. They share
one server cookie key, so each MAC input begins with its domain byte to keep the
two constructions separate; without it they are two different messages under one
key with no separation, and an implementation that unified them would silently
change which value the response path verifies.

**The client address MUST be encoded in the MAC input in a fixed-width form** —
one byte of address family, sixteen bytes of address, two bytes of port, with
unused address bytes zero — so that no two distinct addresses can produce the
same input bytes. A variable-length encoding here would reintroduce exactly the
ambiguity the fixed token layout exists to remove.

**Both cookie MACs MUST be keyed constructions, and the key MUST be passed as the
key.** `crypto_generichash` with the cookie key in its key parameter is BLAKE2b's
keyed mode, which is safe. What must never be substituted is a bare hash of the
key followed by the message under a Merkle-Damgård function — `SHA256(key ‖
message)` and its relatives are extendable without the key, which is the whole
reason HMAC exists. This is written down because the substitution is the natural
one for someone later reaching for a familiar hash, and it would reintroduce a
weakness this protocol does not currently have.

The connection salt is in the input so a cookie belongs to one connection
attempt rather than being a bearer token for an address and client id over its
whole lifetime. It costs nothing and it narrows what a captured cookie is worth.

24 bytes on the wire. The server sends it in the _connection challenge_ packet;
the client echoes it unchanged in the _connection response_; the server
recomputes it from the source address of the response and compares in constant
time. A client that cannot receive at the address it claimed never sees the
cookie and can never produce it.

The client address in the MAC input is the full address and port, normalized as
_Addresses and dual stack_ requires.

*This replaces netcode's encrypted challenge token, and the simplification is
the point.* netcode's version is a 300-byte structure with its own encryption
key, its own sequence counter, its own nonce discipline, and a 316-byte wire
form appearing in two packet types. All of that existed to let the server stay
stateless until the address was proven — but the server already allocates an
encryption mapping for the address when it accepts the connection request, so it
already has somewhere to keep the client id and user data. The cookie proves
exactly the same property in 24 bytes, deletes a key, a sequence counter and a
nonce space, and shrinks the challenge to 80 bytes and the response to 48
— which also improves the amplification ratio.

The cookie key is generated at server start and rotated periodically; a
deployment SHOULD accept cookies under the previous key for one rotation period
so a rotation does not fail connections in flight. Cookie lifetime SHOULD be
short — a few seconds is ample, since the client echoes it immediately.

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

    [return cookie] (40 bytes)          // zero if the client holds none
    [connection salt] (8 bytes)         // client-generated, this attempt
    [client ephemeral public key] (32 bytes)
    [signed connect data] (576 bytes)   // from the token, verbatim
    [signature] (64 bytes)              // from the token, verbatim

A fixed 720 bytes after the prefilter header, 724 bytes on the wire.

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
    [server nonce] (8 bytes)            // never repeats within a server run
    [cookie expire timestamp] (uint64)
    [cookie mac] (16 bytes)

40 bytes after the prefilter header, 44 on the wire — far smaller than the
716-byte request that provoked it.

    cookie mac = crypto_generichash( 16 bytes,
                                     0x01 ‖ client address ‖ connection salt
                                          ‖ server nonce ‖ expire,
                                     server cookie key )

The **server nonce** is the server's contribution to key derivation, and it is
the reason this packet exists in the key schedule rather than only in address
validation. A server MUST NOT issue the same nonce twice within a run; a
counter is sufficient and a random 64-bit value is acceptable. Its purpose is
explained under _Per-connection keys_.

A client that receives one stores the 32 bytes and puts them in the return
cookie field of every subsequent request in this attempt.

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

All packet types >= 1 are sealed. Prior to sealing, after the prefilter header:

    [sequence number] (4 bytes)
    [per-packet type data] (variable length according to packet type)

The per-packet type data:

_connection denied_: no data.

_connection challenge_:

    [cookie] (24 bytes)
    [server ephemeral public key] (32 bytes)

_connection response_:

    [cookie] (24 bytes)

_connection keep-alive_:

    [client index] (uint32)
    [max clients] (uint32)

_connection payload_:

    [payload data] (1 to 1208 bytes)

_connection disconnect_: no data.

The per-packet data is sealed with `crypto_aead_chacha20poly1305_ietf_encrypt`
using the following associated data — netcode's construction, with the
connection salt added so the derived keys are bound to the exchange that chose
them:

    [version] (uint32)                  // 0x4E7C1A93 for RocketNet 1.0
    [protocol id] (uint64)
    [packet type] (uint8)               // binds the type; altering it fails the tag
    [connection salt] (8 bytes)
    [client ephemeral public key] (32 bytes)   // handshake packet types only

**The client's ephemeral public key is in the associated data of the handshake
packet types**, types 2 and 3, and is omitted from the session types 4, 5 and 6 —
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

* **If the type byte is zero this is a connection request: process it per
  _Processing Connection Requests_ and stop.** Everything below applies to
  sealed packets only.

* If the type byte is greater than 7, ignore the packet. Types 8 through 255
  are reserved and MUST be ignored by a conforming receiver.

* The server ignores _connection challenge_ and _connection cookie_ packets. The
  client ignores _connection request_ and _connection response_ packets.

* **If the type byte is 7 this is a connection cookie: the client stores its 24
  bytes for use in subsequent requests of this attempt, and stops.** It is not
  sealed, so nothing below applies to it.

* If the packet is smaller than 4 + 4 + 16 bytes, it cannot hold a sequence and
  a tag; ignore it.

* Look up the encryption mapping for the packet's source address. If there is
  none, ignore the packet — this is the first check on the sealed path and it
  costs one lookup. The mapping supplies the keys and the replay state used
  below.

* Read the 4-byte sequence and reconstruct the 64-bit value.

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

* Advance the replay-protection most-recent sequence number, for those same
  three types.

* If the opened data size does not match the expected size for the type, ignore
  the packet:
  * 0 bytes — _connection denied_
  * 56 bytes — _connection challenge_
  * 24 bytes — _connection response_
  * 8 bytes — _connection keep-alive_
  * [1,1208] bytes — _connection payload_
  * 0 bytes — _connection disconnect_

* Otherwise, process the packet.

## Replay Protection

Sealed packets carry sequence numbers starting at zero and increasing by one per
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
RocketNet does not assume a low packet rate: a game sending fragmented state at
tens of thousands of packets per second per direction needs a buffer sized to at
least one second of its maximum per-client receive rate, and an implementation
serving such a title MUST size it accordingly. An undersized buffer discards
legitimate packets; it never accepts a replay, so the failure is lost traffic
rather than lost security.

*One second is far more than reordering needs — real reordering is milliseconds.
The number is sized for recovering from a loss burst, where a gap in the window
must still be fillable when the retransmissions arrive, and for the case where a
receiver falls behind and catches up. Over-sizing costs only memory, which is
the cheap side of this trade.*

### No lookup may be linear in the number of clients

Not wire format, but a requirement all the same, because at a thousand clients a
linear scan is quadratic work in disguise and the protocol is specified for that
scale.

**Every lookup a server performs per packet or per connection MUST be constant
time in the number of client slots.** netcode performs all of the following by
scanning, which was reasonable at its target of a few dozen clients and is not
at a thousand:

| Lookup | When netcode does it | Required here |
|---|---|---|
| address to encryption mapping | every received packet | constant time |
| address to client slot | every received packet, and both handshake steps | constant time |
| client id to client slot | connection request and response | constant time |
| token identifier to history entry | every connection request | constant time |
| free client slot | every completed connection | constant time |

The per-packet rows are the ones that matter most: a thousand clients sending at
a high rate means the scan cost multiplies by the aggregate packet rate, and
becomes whole cores spent comparing addresses. The per-connection rows are less
severe but are no harder to do properly.

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

### Checks are a sequence, not a location

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
what the full sequence would have rejected: no more, or it discards legitimate
traffic that the userspace path would have accepted; no less, or the guarantee is
weaker than specified. And the userspace path MUST NOT assume an earlier stage
ran, since the same implementation serves every transport. Pushing work down
therefore reduces the *volume* reaching userspace rather than removing code from
it — the check still runs, but on the handful of packets that survived instead of
on the flood. That is where the win is, and it keeps one implementation of the
logic rather than one per transport.

## Client State Machine

States, transitions, timeouts, retry rates and error states are netcode 1.02's:

* _connect token expired_ (-6)
* _invalid connect token_ (-5)
* _connection timed out_ (-4)
* _connection response timed out_ (-3)
* _connection request timed out_ (-2)
* _connection denied_ (-1)
* _disconnected_ (0)
* _sending connection request_ (1)
* _sending connection response_ (2)
* _connected_ (3)

Before its first request the client verifies the token: the address flags name
at least one family with bits 2-7 zero, and the create timestamp is not later
than the expire timestamp; otherwise _invalid connect token_. The client selects
the address whose family it can reach, preferring IPv6 when both are present and
both are reachable, and uses that address for the whole attempt. If no listed
family is reachable it transitions to _connection request timed out_.

The client generates its connection salt once per connection attempt, before the
first request, and reuses it for every retransmission of that attempt. **The
client's packet sequence number MUST NOT be reset within an attempt.** A new
attempt means a new salt, hence new keys, so restarting the sequence at zero is
correct there and only there.

All other client behavior — send rates, storing and echoing the cookie,
transition to _connected_ on the first keep-alive, keep-alive generation, timeout
handling, redundant disconnect packets — is netcode 1.02, reading "sealed" for
"encrypted".

## Server-Side Connection Process

The server manages n client slots as in netcode. Implementations are expected to
support high slot counts; the reference target is 1000.

### Processing Connection Requests

**The governing intent, which every rule in this section serves: a client
without a valid connect token must be trivially rejectable.** That is what the
connect token is for beyond authorization — it is the protection against being
flooded off the internet. A server should be able to throw away traffic from
anyone who has not been sent by the matchmaker at a cost so small that an
attacker gains nothing by sending it, and the moment rejection stops being
trivial the token has stopped doing half its job.

netcode's strict rules follow from that intent and hold here: clients must have
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

**The two properties, and why they need four layers here where netcode needed
fewer.** What a server must have is a bar that is hard to get over — you cannot
connect unless the matchmaker sent you — and no way to occupy a slot with a
zombie client that is not really there. netcode obtained both from one symmetric
check: the shared key made the bar real *and* made enforcing it cost about a
microsecond, so cheap rejection and a high bar were the same operation. Removing
the shared secret separates them, and they have to be rebuilt as a ladder, each
rung rejecting what it can at its own cost so the expensive rung is reached only
by traffic that has already cleared the cheap ones:

| Layer | Rejects | Cost | Bar |
|---|---|---|---|
| Prefilter | traffic that is not this title's | a 3-byte compare | none — unkeyed |
| Cookie | a source address that cannot receive | one keyed hash | must hold the address |
| Signature | anyone the matchmaker did not send | one Ed25519 verify | must hold a real token |
| Challenge | a client that never answers | already paid | must complete a round trip |

The bar is the signature and it is as hard as it has ever been — an attacker
needs a token the backend actually issued, which means authenticating with the
backend. The zombie is stopped by the challenge, exactly as in netcode: no slot
is allocated until the client has answered from the address it claimed. What the
cookie adds is not a new bar but a cheap gate in front of the expensive one, so
that the cost of *enforcing* the bar cannot itself be turned into the attack.

Under public key crypto the ordering matters far more than it did in netcode. A
connection request costs a signature verification and a seal-open — on the order
of a hundred microseconds, against about one for netcode's symmetric open. Every
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

*An earlier draft made this load-triggered, to avoid spending a round trip on
players when no attack was happening. That is the wrong trade for two reasons.

**A round trip is cheap here because of what this protocol is for.** RocketNet
carries long-lived connections — a session lasts minutes, and a connect is
already expected to take on the order of a second. It is not a request-response
protocol where a client connects, asks one question and leaves, and where a
handshake round trip would be a meaningful fraction of the total work. Against a
session measured in minutes, an extra exchange at the start is invisible, and
resilience bought with it is bought cheaply. A protocol whose connections were
short and numerous would have to weigh this differently.

The second reason is that a defence which engages only above a threshold is a
code path that never executes in normal operation or in testing, which means it
is most likely to be discovered broken on the day it is first needed. A threshold
also has to be tuned, and creates a seam an attacker can sit just underneath.
Always-on deletes the rate measurement, the threshold, the seam, and the
untested branch.*

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
to saturate a core** — where netcode's symmetric open needed gigabits. The
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

* If the packet, less the prefilter header, is not 720 bytes, ignore it.
* **If a client from the request source address and port is already connected,
  ignore it.** This costs one lookup and no crypto.
* If the version is not 0x4E7C1A93, ignore it.
* If the protocol id does not match the server's, ignore it.
* If the expire timestamp is <= the current time, ignore it.
* If the address flags have bits 2-7 set, or name no family, ignore it.
* If neither flagged address in the signed data is one this server is advertised
  at, ignore it. Either advertised address is accepted, not only the first.
* If the server public key in the signed data is not one this server holds,
  ignore it.
* **Recompute the cookie MAC** from this packet's source address, its connection
  salt, and the server nonce and expire timestamp in its return cookie field, and
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
  used-token lookup hashes 576 bytes, and the two asymmetric operations follow —
  so nothing below may run for an address that has not answered.
* If the token identifier already appears in the used-token history bound to a
  different source address and port, ignore it. **The token identifier is the
  first 16 bytes of `crypto_generichash` over the 576-byte signed connect
  data** — a content-determined value. *(It is deliberately not the signature
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
* If no slots are available, respond with a _connection denied packet_.
* **Compute `shared = crypto_scalarmult( server ephemeral secret, client
  ephemeral public )`. If it returns non-zero, ignore the packet.** libsodium
  returns an error for a small-order peer key, and this check must happen here —
  before any mapping is created — so that a client offering such a point never
  obtains a mapping. This is the second asymmetric operation on this path and it
  is deliberately after both the cookie and the signature verification.
* Derive the four per-connection keys: the two handshake keys from the token
  keys, the salt and the server nonce; the two session keys from those plus the
  shared secret and both ephemeral public keys.
* **Erase the shared secret**, and the server's ephemeral secret if it is not
  being reused across a rotation window, as soon as the four keys exist.
* **If an encryption mapping already exists for this source address and port
  whose salt and server nonce both match this request, the server reuses it
  unchanged** — its keys, its sequence numbers and its replay state — and answers
  with a challenge sealed under that mapping's keys. This is the ordinary
  retransmission case: a client resends its request at about 10Hz with the same
  salt and the same returned cookie, so a lost challenge produces an identical
  request.

  *Correctness no longer rests on this rule.* Because the server nonce is part
  of the key derivation and is never reissued, a mapping created with different
  inputs has different keys, so a sequence starting again at zero cannot repeat a
  (key, nonce) pair. The rule exists to keep replay state and sequence continuity
  across a retransmission, not to prevent nonce reuse. An implementation that
  evicted mappings under pressure, or expired one and rebuilt it, would still be
  safe.
* Otherwise add an encryption mapping for the source address and port, holding
  the derived keys, the connection salt, the client id and the user data. It
  expires after _timeout_ seconds without traffic, or if the connection is not
  established within _timeout_ seconds. If it cannot be added, ignore the packet.
* Respond with a _connection challenge packet_ carrying a freshly computed
  cookie.

**The used-token history** is a fixed-size structure. An implementation MUST
document its capacity and eviction policy, and SHOULD size it to at least eight
entries per client slot with oldest-first eviction, as netcode does. Entries
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

* Recompute the cookie from the response's source address, the client id in the
  encryption mapping, and the cookie expire timestamp in the packet; if it does
  not match in constant time, or the cookie has expired, ignore the packet.
* If a client from the source address and port is already connected, ignore it.
* If a client with that client id is already connected, ignore it.
* If no slots are available, respond with a _connection denied packet_.
* Assign a slot, copy the user data from the encryption mapping into it, mark
  the client unconfirmed, and respond with a _connection keep-alive packet_.

### Connected Clients

Client slots, the client index, the confirmed flag, keep-alive prefixing until
confirmation, payload delivery both ways, redundant disconnect packets, and slot
reset on timeout or disconnect are all netcode 1.02.

## Security baseline: never weaker than netcode 1.02

**No security property of this protocol may be weaker than netcode 1.02's. That
is the absolute minimum baseline and it is a conformance requirement.** A change
that buys a new property at the cost of an old one is a defect, however
attractive the new property. Where a property is genuinely weaker it is named
here rather than hidden.

| Property | netcode 1.02 | RocketNet 1.0 | |
|---|---|---|---|
| Only backend-authorized clients connect | shared-key AEAD tag | Ed25519 signature, verified before the seal is opened | better |
| Session keys confidential in transit | encrypted under the shared key | sealed to the server's public key | equal |
| Client identity hidden from observers | client id inside the encrypted token | client id inside the sealed block | equal |
| Token fields unmodifiable | AEAD associated data | signature over the whole buffer | better |
| Source address spoofing | encrypted challenge token, two RTT | MAC cookie, three RTT | better |
| Packet type unmodifiable | AEAD associated data | AEAD associated data | equal |
| Packet replay | sequence window, sequence as nonce | identical | equal |
| Token reuse from another address | history keyed on the AEAD tag | history keyed on a hash of the signed data | equal |
| Nonce reuse across server restart | documented hazard, two optional mitigations | impossible: keys derived per connection | better |
| Nonce reuse on reconnect | undocumented hazard | impossible: keys derived per connection | better |
| Amplification | every response smaller than its request | identical, and the challenge is far smaller | equal |
| Compromised server | **mints tokens for the entire fleet** | mints nothing, and forward secrecy means recorded past sessions stay closed | better |
| **CPU cost of one unauthenticated request** | **one symmetric open, ~1 µs** | **Ed25519 verify + seal-open, ~100 µs** | **weaker** |

**The last row is a real and deliberate weakening, and it is the price of
removing the shared secret.** It is stated rather than argued away. The
mitigations are normative and specified above: the prefilter rejects at line
rate, every cheap check including the already-connected test runs before any
asymmetric work, the used-token history bounds repeats, and rate limiting is
required with its own limits named. Every other row is equal or better.

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

*This closes an exposure that is wider than it first appears, and wider than an
earlier draft of this document admitted.* The token keys are generated by the
backend and travel in the clear inside the connect token, so before this change
the readers of a session's traffic included: the backend, permanently and for
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
legitimate client must obtain a fresh token. Inherited from netcode unchanged. It
requires an on-path position, from which simply dropping the client's packets is
a more effective attack — but it is not equivalent, because it is harder to
attribute: dropped packets look like a broken network and get investigated as
one, while this looks to the player like the game's own backend misbehaving. Mild
in effect, quieter than the alternative.

**No response may be larger than the packet that provoked it.** This is a
normative invariant of the protocol, not inherited advice, and every packet type
must be checked against it as the protocol grows. Today the widest margins are a
724-byte request answered by an 80-byte challenge or a 44-byte cookie. At those
ratios the challenge is not usefully a reflection primitive: an attacker
spoofing a victim's address to place 80 bytes there spends 724 to do it and is
strictly worse off than sending the 676 at the victim directly. The cost of a
spoofed request is CPU, which the cookie exchange addresses, rather than
reflected bytes.

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

## Deviations from netcode 1.02, in one place

**Trust model**

1. The private connect token data is sealed to the server with
   `crypto_box_seal` to its X25519 public key, instead of encrypted under a key
   shared by the whole fleet.
2. Token authenticity is an Ed25519 signature, verified before the seal is
   opened, instead of the shared-key AEAD tag.
3. A server holds the backend public key and its own keypair; no secret is
   shared between backend and servers.
4. The signed data names the server's public key, so a server may hold several
   keypairs and rotate them.
5. The used-token history is keyed on a hash of the signed data rather than an
   authentication tag.

**Structure and size**

6. One server per token at up to two addresses (IPv4 and/or IPv6), instead of a
   list of up to 32 candidate servers. Failover is a fresh token; the client
   does not fail over between the two addresses.
7. The prefilter header is 4 bytes: a type byte and a per-title constant tag,
   depending on no address, port or length. netcode has no equivalent header;
   its Network Next ancestor spent 17 bytes on a derived structural one.
8. The sequence number is a fixed 4 bytes on the wire with the 64-bit value
   reconstructed, instead of a variable 1 to 8 bytes packed against the type
   byte. The type byte carries only the type.
9. The encrypted challenge token is replaced by a 24-byte MAC cookie: no
   challenge key, no challenge sequence counter, no challenge nonce space, and
   the challenge falls from 366 bytes to 80 and the response to 48.
10. The version is an opaque 32-bit constant rather than netcode's 14-byte
    ASCII string, which travels in the clear on every connection request and
    would be a universal signature for the protocol.
11. One packet-size budget of 1232 bytes from the IPv6 minimum MTU; payload
    packets carry 1 to 1208 bytes; the token is a fixed 768 bytes, one MTU by
    design.

**Keys and nonces**

12. Per-connection keys are derived from the token keys and a client-chosen
    salt, with separate handshake and session contexts. This deletes netcode's
    server-restart section and both of its optional mitigations, deletes the
    2^63 global sequence convention, and removes the reconnect nonce reuse
    netcode does not document.
13. The connection salt is included in the per-packet associated data.

**Dual stack**

14. A server accepts a connection request at either advertised address;
    boundary address normalization is normative.

Everything else — the two-RTT handshake shape, per-packet AEAD and its
associated data construction, replay protection, the client state machine, the
server slot model, the strict connection-request rules — is netcode 1.02.
