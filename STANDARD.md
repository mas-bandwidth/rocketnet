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
* BLAKE2b keyed hashing (`crypto_generichash`) — the address challenge cookie.
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
both paths. Failover between families is therefore not a client behaviour; it is
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
program — so no two titles share a tag and nobody can write a universal
RocketNet matcher (issue #17). That is what the per-title generation buys: not
strength, but non-universality.

A receiver drops any packet smaller than the minimum for its type, larger than
1232 bytes, or whose title tag does not match.

There is no keyed or stateful filter. Network Next distributed a rotating
"magic" value to servers from a backend; a player-hosted server has no backend
feeding it per-packet secrets, so the concept is absent by design rather than
deferred. **The upgrade path for a deployment that needs to resist a targeted
flood is a whitelist in the kernel** — an XDP map of currently authorized source
addresses, maintained by the server as connections are established. That is
stateful, is outside this standard, and is the only thing that actually helps;
no unkeyed header can substitute for it.

## Packet sizes

One budget governs both address families: the IPv6 minimum MTU is 1280 bytes,
less 40 for the IPv6 header and 8 for UDP, giving **1232 bytes** as the largest
UDP payload that never fragments on either family.

Every sealed packet spends, before the application's bytes:

    prefilter header                  4
    sequence number                   4
    AEAD tag                         16
                                   ----
                                     28

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
        [version info] (14 bytes)       // "ROCKETNET 1.0" ASCII, null terminated
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

The buffer is 576 bytes and uses 531, leaving 45 free **deliberately**. The
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

    handshake client to server = kdf( token c2s key, salt, "RKTNHAND" )
    handshake server to client = kdf( token s2c key, salt, "RKTNHAND" )
    session   client to server = kdf( token c2s key, salt, "RKTNSESS" )
    session   server to client = kdf( token s2c key, salt, "RKTNSESS" )

_Connection challenge_, _connection response_ and _connection denied_ packets
are sealed with the handshake keys. _Connection keep-alive_, _connection
payload_ and _connection disconnect_ packets are sealed with the session keys.

This is four BLAKE2b invocations per connection and it deletes a great deal:

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
                                     client address ‖ client id ‖ cookie expire,
                                     server cookie key )

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
nonce space, and shrinks the challenge and response packets from 366 bytes to 48
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

Every packet begins with the 4-byte prefilter header, whose first byte is the
packet type. The type byte carries the type and nothing else.

### Connection request packet

The connection request (0) is not sealed. After the prefilter header:

    [connection salt] (8 bytes)         // client-generated, this attempt
    [signed connect data] (576 bytes)   // from the token, verbatim
    [signature] (64 bytes)              // from the token, verbatim

A fixed 648 bytes after the prefilter header, 652 bytes on the wire.

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

    [version info] (14 bytes)           // "ROCKETNET 1.0" ASCII, null terminated
    [protocol id] (uint64)
    [packet type] (uint8)               // binds the type; altering it fails the tag
    [connection salt] (8 bytes)

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

* If the type byte is greater than 6, ignore the packet.

* The server ignores _connection challenge_ packets. The client ignores
  _connection request_ and _connection response_ packets.

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
  * 24 bytes — _connection challenge_
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

A replay buffer of 256 entries per client is a floor, not a recommendation.
RocketNet does not assume a low packet rate: a game sending fragmented state at
tens of thousands of packets per second per direction needs a buffer sized to at
least one second of its maximum per-client receive rate, and an implementation
serving such a title MUST size it accordingly. An undersized buffer discards
legitimate reordered packets; it never accepts a replay.

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

All other client behaviour — send rates, storing and echoing the cookie,
transition to _connected_ on the first keep-alive, keep-alive generation, timeout
handling, redundant disconnect packets — is netcode 1.02, reading "sealed" for
"encrypted".

## Server-Side Connection Process

The server manages n client slots as in netcode. Implementations are expected to
support high slot counts; the reference target is 1000.

### Processing Connection Requests

netcode's strict rules hold: clients must have a valid connect token; respond
only when necessary; ignore malformed requests as cheaply as possible; every
response smaller than the request, so there is nothing to amplify.

Under public key crypto the ordering matters far more than it did in netcode. A
connection request costs a signature verification and a seal-open — on the order
of a hundred microseconds, against about one for netcode's symmetric open. Every
check that can reject without touching the token therefore runs first. In
particular the already-connected check reads only the packet's source address,
and it comes **before** the crypto: otherwise an attacker who captures a single
valid connection request can replay it at line rate and burn a full verify on
every copy.

In this exact order, on a _connection request packet_:

* If the packet, less the prefilter header, is not 648 bytes, ignore it.
* **If a client from the request source address and port is already connected,
  ignore it.** This costs one lookup and no crypto.
* If the version info is not "ROCKETNET 1.0" — all 14 bytes, including the
  terminating null — ignore it.
* If the protocol id does not match the server's, ignore it.
* If the expire timestamp is <= the current time, ignore it.
* If the address flags have bits 2-7 set, or name no family, ignore it.
* If neither flagged address in the signed data is one this server is advertised
  at, ignore it. Either advertised address is accepted, not only the first.
* If the server public key in the signed data is not one this server holds,
  ignore it.
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
* Derive the four per-connection keys from the token keys and the connection
  salt.
* Add an encryption mapping for the source address and port, holding the derived
  keys, the client id and the user data. It expires after _timeout_ seconds
  without traffic, or if the connection is not established within _timeout_
  seconds. If it cannot be added, ignore the packet.
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
| Source address spoofing | encrypted challenge token, two RTT | MAC cookie, two RTT | equal |
| Packet type unmodifiable | AEAD associated data | AEAD associated data | equal |
| Packet replay | sequence window, sequence as nonce | identical | equal |
| Token reuse from another address | history keyed on the AEAD tag | history keyed on a hash of the signed data | equal |
| Nonce reuse across server restart | documented hazard, two optional mitigations | impossible: keys derived per connection | better |
| Nonce reuse on reconnect | undocumented hazard | impossible: keys derived per connection | better |
| Amplification | every response smaller than its request | identical, and the challenge is far smaller | equal |
| Compromised server | **mints tokens for the entire fleet** | mints nothing; leaks only its own sessions, including recorded past ones | better |
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

**Sealing gives the recipient no forward secrecy.** An attacker who records
connection requests and later obtains a server's X25519 secret key can open every
recorded sealed block and decrypt every past session that server ran. netcode
has the same retroactive exposure, fleet-wide rather than per-server, so this is
strictly better — but "leaks only its own sessions" includes recorded past ones.
Ephemeral key exchange was considered and declined: for game session content —
positions, inputs, chat — the value of retroactive decryption does not justify
the added handshake complexity.

**Connection-request replay is a denial of service against one client, not an
authentication break.** An on-path observer can capture a connection request and
replay it from its own address; it cannot complete the handshake, because the
challenge returns sealed under a key it does not hold. If its replay arrives
first, the used-token history binds that token to the attacker's address and the
legitimate client must obtain a fresh token. Inherited from netcode unchanged,
and mild: it requires an on-path position, from which simply dropping the
client's packets is a strictly better attack.

**The challenge is a small reflection primitive.** A spoofed request can make a
server emit 48 bytes at a victim. It is bounded — the response is far smaller
than the request that provoked it, and the used-token history stops repeats from
a second spoofed source — but the CPU is spent before the history is consulted,
which is why the ordering above puts every free check first.

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
   the challenge and response packets fall from 366 bytes to 48.
10. One packet-size budget of 1232 bytes from the IPv6 minimum MTU; payload
    packets carry 1 to 1208 bytes; the token is a fixed 768 bytes, one MTU by
    design.

**Keys and nonces**

11. Per-connection keys are derived from the token keys and a client-chosen
    salt, with separate handshake and session contexts. This deletes netcode's
    server-restart section and both of its optional mitigations, deletes the
    2^63 global sequence convention, and removes the reconnect nonce reuse
    netcode does not document.
12. The connection salt is included in the per-packet associated data.

**Dual stack**

13. A server accepts a connection request at either advertised address;
    boundary address normalization is normative.

Everything else — the two-RTT handshake shape, per-packet AEAD and its
associated data construction, replay protection, the client state machine, the
server slot model, the strict connection-request rules — is netcode 1.02.
