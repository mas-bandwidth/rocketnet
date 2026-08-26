# RocketNet 1.0 — connection standard (draft)

**RocketNet** is a protocol for creating secure client/server connections over
UDP. This document is the security and connection standard, so people can write
their own conforming implementations. It is the packet-level connection layer
only; reliability, flows and broadcast ride above it in payload packets and are
specified separately.

It is netcode 1.02 rebuilt on public key cryptography. The shape is netcode's —
connect token, two-RTT handshake, challenge token, sealed packets, per-client
slots — and everything above the connection handshake is netcode unchanged. One
thing changes and it runs through the whole token design:

**There is no secret shared between the backend and the dedicated servers.** The
backend signs connect tokens with a private key that never leaves the backend;
it seals the session keys to the target server's own public key. A dedicated
server holds only the backend's public key and its own keypair. A compromised
server cannot mint tokens and leaks only its own sessions. This is what lets a
title run player-hosted servers on hardware the backend operator does not
control — netcode's fleet-wide shared private key cannot.

Crypto is libsodium. Nothing in this protocol requires a primitive outside a
stock libsodium build:

* Ed25519 signatures (`crypto_sign`) — backend token authenticity.
* Sealed boxes (`crypto_box_seal`, X25519 + XSalsa20-Poly1305) — delivering the
  session keys to the server confidentially through the untrusted client.
* ChaCha20-Poly1305 IETF AEAD (`crypto_aead_chacha20poly1305_ietf`) — challenge
  tokens and per-packet sealing, exactly as netcode 1.02 uses it.

## Architecture

Three components, as in netcode:

1. **The backend** — whatever service authenticates players and decides who
   plays where: a web server, a matchmaker, a game's own service. It holds the
   long-term signing keypair.
2. **Dedicated servers** — instances of the server side of the game, in data
   centers, in the cloud, or on community-hosted machines. Each holds its own
   key exchange keypair and the backend's public key.
3. **Clients** — endpoints running the protocol that want to connect to a
   dedicated server.

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
  32-byte public key, 32-byte secret key). The public half is registered with
  the backend when the server is provisioned; how is outside the scope of this
  standard. The secret half never leaves the server, and its exposure
  compromises only the sessions sealed to that one server.

* Each **client** holds the backend's public key as configuration, and the
  per-session keys handed to it inside a connect token.

There is no key in the system that is both shared and secret. This is the
load-bearing difference from netcode 1.02.

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
    crypto_aead_chacha20poly1305_ietf_KEYBYTES   32
    crypto_aead_chacha20poly1305_ietf_NPUBBYTES  12
    crypto_aead_chacha20poly1305_ietf_ABYTES     16   // packet/challenge tag

Where this document says a block is _sealed to the server_, it means
`crypto_box_seal` with that server's X25519 public key: the output is
`crypto_box_SEALBYTES` (48) longer than its plaintext, carries its own ephemeral
public key, and can be opened only with the server's secret key. A sealed box is
anonymous: it authenticates nothing about who sealed it. **Authenticity of a
connect token comes only from the backend's Ed25519 signature, and a server MUST
verify that signature before it trusts anything the sealed block contains.**
This is stated here because it is the one place a public-key rebuild of netcode
can go quietly wrong.

## Addresses and dual stack

A connect token names **one server**, reachable at up to two addresses: an IPv4
address and an IPv6 address, each optional, at least one present. (netcode
allowed a list of up to 32 candidate servers per token; that multi-address
design was a mistake — a matchmaker assigns a server, and the only real
multiplicity is address family.)

RocketNet is dual stack from day one, not as a later addition:

* A server MAY advertise both an IPv4 and an IPv6 address, and it accepts a
  connection request arriving at **either** of them. (netcode's dual stack was
  retrofitted, and for years its server compared tokens against only its first
  advertised address; RocketNet does not repeat that.)
* Address normalization is a law of this protocol, not an implementation
  convenience: above the platform socket layer, an IPv4-mapped IPv6 address
  (`::ffff:a.b.c.d`) must never exist. A receiver converts a mapped source to
  native IPv4 exactly once, at the socket boundary, before any address is used
  for a session table, an encryption mapping, or a prefilter hash. Session
  state keyed on an address is where a mapped-versus-native mismatch detonates,
  and the boundary rule is what prevents it.

Higher-level transports may name endpoints by an opaque string rather than an
IP address (per REQUIREMENTS.md: an endpoint is a string with a maximum size,
and the connect data handed to a client may be a base64-like token rather than
an IP). How a non-IP transport encodes its endpoint in a connect token is that
transport's business; the address encoding below is the form for the IP
transports and the reference the others follow. **Open for design: whether the
token's address fields generalise to a length-prefixed transport endpoint
string in 1.0, or the IP encoding stays normative with string endpoints one
layer up. Flagged for Glenn, not resolved here.**

## The Prefilter Header

Every RocketNet UDP packet begins with an 18-byte prefilter header. It is not
cryptography. It exists so a server — in particular a kernel XDP program in
front of a server — can reject garbage and off-path noise at line rate before
any cryptographic work happens.

    [packet type] (uint8)
    [structural bytes] (17 bytes)

The structural bytes are derived from a per-title function over the packet
type, the source and destination addresses, and the payload length, such that
random data satisfies the check with negligible probability: each byte is
constrained, and the product of the per-byte probabilities across the header is
astronomically small — redundancy, in short. The check is stateless and
verifiable byte by byte, so a filter drops a bad packet at the first failing
byte.

**The derivation is generated per title, not fixed by this standard.** A tool
takes a per-title seed and emits the filter as code in both forms from one
seed — the userspace check and the XDP program — so no two titles share a
filter and nobody can write a universal RocketNet matcher (issue #17). The
"pittle/chonkle" construction inherited from Network Next is the reference
example of the shape and is being replaced by the generated form. This standard
specifies the header's role and properties — 18 bytes, stateless, structural,
per title, checkable at line rate — and leaves the exact derivation to the
generator.

A receiver drops any packet smaller than 18 bytes, larger than 1232 bytes (see
_Packet sizes_), or whose structural bytes fail. The prefilter proves a packet
has a title's RocketNet shape; the cryptography below proves everything else.
There is no stateful or keyed filter — that concept (Network Next's distributed
"magic") is deliberately absent, because a player-hosted server has no backend
feeding it per-packet secrets.

## Packet sizes

One budget governs both address families: the IPv6 minimum MTU is 1280 bytes,
less 40 for the IPv6 header and 8 for UDP, giving **1232 bytes** as the largest
UDP payload that never fragments on either family.

Everything a sealed packet spends before the application's bytes:

    prefilter header                 18
    sequence number (worst case)      8
    AEAD tag                         16
                                   ----
                                     42

So payload packets carry **1 to 1190 bytes** of application data. That is
netcode's 1200 less the 17 extra bytes RocketNet spends on the prefilter header
where netcode had a single prefix byte — the cap is derived from this budget,
never inherited.

The connect token is a fixed 768 bytes, inside a single 1500-byte MTU with room
to spare, by design.

## Connect Token

A _connect token_ ensures that only authenticated clients can connect to the
dedicated server. It is netcode's connect token rebuilt on public keys: the
backend says "this is a valid client, coming from your matchmaker," and the
server verifies that statement holding only the backend's public key.

### Private connect token data

What only the server may read: who the client is, the session keys, and private
user data. Prior to sealing:

    [client id] (uint64)                // globally unique authenticated client id
    [client to server key] (32 bytes)
    [server to client key] (32 bytes)
    [user data] (256 bytes)             // application-defined, per protocol id
    <zero pad to 384 bytes>

The 384-byte block is sealed to the server with `crypto_box_seal` and the
server's X25519 public key:

    [sealed private block] (432 bytes)  // 384 + crypto_box_SEALBYTES

The session keys are the same ones the client is handed in the clear elsewhere
in the token; sealing hides them from anyone who captures the connection request
off the UDP wire, not from the client. The client cannot open the sealed block,
and does not need to — it holds the keys in the clear. Only the target server
can open it, with its own keypair.

**The client id is inside the seal deliberately, as it is in netcode.** A
connection request crosses the open internet, so anything in its clear portion
is visible to any observer on the path. An identifier that names a player must
not be one of those things: in the clear it would let a passive observer see
which player is connecting to which server, session after session. It is still
covered by the backend's signature, because the sealed block sits inside the
signed data below — sealing it costs no authenticity.

### Signed connect data

Everything the server checks on a connection request is gathered into one fixed
512-byte buffer and signed by the backend. The request carries this buffer and
its signature verbatim, so the server verifies it with no shared secret and no
other state:

    signed connect data (512 bytes):
        [version info] (14 bytes)       // "ROCKETNET 1.0" ASCII, null terminated
        [protocol id] (uint64)          // unique to this title
        [create timestamp] (uint64)     // unix time the token was created
        [expire timestamp] (uint64)     // unix time the token expires
        [timeout seconds] (int32)       // negative disables timeout (dev only)
        [address flags] (uint8)         // bit 0 = has IPv4, bit 1 = has IPv6;
                                        // at least one bit set
        [ipv4 address] (6 bytes)        // a,b,c,d (4 x uint8) + port (uint16);
                                        // zero when bit 0 is clear
        [ipv6 address] (18 bytes)       // a..h (8 x uint16) + port (uint16);
                                        // zero when bit 1 is clear
        [sealed private block] (432 bytes)
        <zero pad to 512 bytes>

    [signature] (64 bytes) = crypto_sign_detached( signed connect data,
                                                   backend secret key )

Both address fields occupy their bytes whether or not they are used, so the
layout is fixed and a reader never parses a length from the data it is
validating; the flags byte says which are valid.

The signature covers the entire 512-byte buffer including the zero padding.
Every field a server checks — version, protocol id, timestamps, timeout, both
addresses, and the sealed block with the client id and session keys inside it —
is inside the signed data, so none can be modified without failing
verification. Ed25519 signatures are deterministic, so an issued token has
stable signature bytes; the server's used-token history keys on the signature
(see _Processing Connection Requests_).

### Connect token format

The full token, handed to the client over the secure channel:

    [signed connect data] (512 bytes)
    [signature] (64 bytes)
    [client to server key] (32 bytes)   // in the clear, for the client
    [server to client key] (32 bytes)   // in the clear, for the client
    <zero pad to 768 bytes>

A fixed **768 bytes** — 1024 characters in base64, inside a single 1500-byte
MTU with room to spare. The session keys appear twice on purpose: once in the
clear for the client, which receives the token over a secure channel and needs
them to speak; and once sealed to the server, so they reach it confidentially
through an untrusted carrier — the client.

## Challenge Token

Challenge tokens stop clients with spoofed source addresses from connecting.
They are purely server-local and symmetric — no public key involved. This is
netcode's challenge token unchanged.

Prior to sealing:

    [client id] (uint64)
    [user data] (256 bytes)
    <zero pad to 300 bytes>

Sealed with `crypto_aead_chacha20poly1305_ietf_encrypt`, no associated data, a
random key generated when the server starts, and a nonce built from the
_challenge token sequence number_ (starts at zero, increments per challenge
generated): 4 zero bytes followed by the sequence as a 64-bit little-endian
value. Encryption covers the 300 bytes and appends the 16-byte tag:

    [sealed challenge token data] (316 bytes)

## Packets

RocketNet has the following packets:

* _connection request packet_ (0)
* _connection denied packet_ (1)
* _connection challenge packet_ (2)
* _connection response packet_ (3)
* _connection keep-alive packet_ (4)
* _connection payload packet_ (5)
* _connection disconnect packet_ (6)

Every packet begins with the 18-byte prefilter header. The prefilter's packet
type byte carries the packet type in its low 4 bits and, for sealed packets,
the number of sequence bytes in its high 4 bits, exactly as netcode's prefix
byte does. For the connection request the type byte is zero.

### Connection request packet

The connection request (0) is not sealed. After the prefilter header:

    [signed connect data] (512 bytes)   // from the token, verbatim
    [signature] (64 bytes)              // from the token, verbatim

A fixed 576 bytes after the prefilter header, 594 bytes on the wire. The client
sends the signed data and signature exactly as issued; the server reconstructs
nothing and verifies the signature over the 512 bytes as received. Authenticity
is the Ed25519 signature — never the mere fact that the sealed block opens,
since a sealed box opens for anyone who sealed to the server's public key.

### Sealed packets

All packet types >= 1 are sealed. Prior to sealing, after the prefilter header:

    [sequence number] (variable length 1-8 bytes)
    [per-packet type data] (variable length according to packet type)

The sequence encoding is netcode's: the high 4 bits of the type byte give the
byte count in [1,8], sequence bytes written low byte first.

The per-packet type data:

_connection denied_: no data.

_connection challenge_:

    [challenge token sequence] (uint64)
    [sealed challenge token data] (316 bytes)

_connection response_:

    [challenge token sequence] (uint64)
    [sealed challenge token data] (316 bytes)

_connection keep-alive_:

    [client index] (uint32)
    [max clients] (uint32)

_connection payload_:

    [payload data] (1 to 1190 bytes)

_connection disconnect_: no data.

The per-packet data is sealed with `crypto_aead_chacha20poly1305_ietf_encrypt`
using the following associated data — netcode's construction unchanged:

    [version info] (14 bytes)           // "ROCKETNET 1.0" ASCII, null terminated
    [protocol id] (uint64)
    [prefilter type byte] (uint8)       // binds the packet type; an attacker
                                        // altering it fails the tag check

The 96-bit nonce is built from the packet sequence number: 4 zero bytes followed
by the sequence as a 64-bit little-endian value. Client-to-server packets are
sealed with the client to server key; server-to-client packets with the server
to client key. Post sealing:

    [prefilter header] (18 bytes)       // type byte: (num_sequence_bytes << 4) | packet_type
    [sequence number] (variable length 1-8 bytes)
    [sealed per-packet type data] (per-packet data + 16-byte tag)

## Reading Sealed Packets

In this exact order:

* If the packet is smaller than 18 + 1 + 1 + 16 bytes, ignore it.
* If the prefilter structural check fails, ignore it.
* If the low 4 bits of the type byte are >= 7, ignore it.
* The server ignores _connection challenge_ packets.
* The client ignores _connection request_ and _connection response_ packets.
* If the high 4 bits (sequence bytes) are outside [1,8], ignore it.
* If the packet fails the replay-protection already-received test, ignore it.
* If the sealed data fails to open with the expected key, the associated data
  above, and the sequence-number nonce, ignore it.
* Advance the replay-protection most-recent sequence number.
* If the opened data size does not match the expected size for the type, ignore
  it:
  * 0 bytes — _connection denied_
  * 8 + 316 bytes — _connection challenge_
  * 8 + 316 bytes — _connection response_
  * 8 bytes — _connection keep-alive_
  * [1,1190] bytes — _connection payload_
  * 0 bytes — _connection disconnect_
* Otherwise, process the packet.

## Replay Protection

Identical to netcode 1.02. Sealed packets carry 64-bit sequence numbers starting
at zero, increasing per packet sent; the sequence is in the header and read
before opening; the sequence is the nonce, so any modification fails the tag.
Discard anything older than the most recent sequence minus the replay buffer
size; within the window accept a sequence only once; on a successful open mark it
received and advance the most recent. Applied to _connection keep-alive_,
_connection payload_ and _connection disconnect_ on both ends. A replay buffer of
256 entries per client is a reasonable floor; a high-rate server sizes it to at
least one second of its maximum per-client receive rate.

## Nonce Reuse and Server Restarts

Sequence numbers are the AEAD nonces, so netcode's discipline carries over whole:
per-client sequences start at zero; the server's global sequence number (for
packets sent before a client is assigned a slot — challenge and denied) is set to
2^63 at server start, keeping global and per-client nonce spaces disjoint under
one key within a run.

The guarantee is scoped to a single run. The server to client key rides inside
the connect token, so a client presenting the same unexpired token to a restarted
server would repeat nonces under that key. The window is bounded by token expiry.
An implementation MAY close it entirely with either netcode mitigation, both
unchanged here: reject connect tokens whose expire timestamp predates server
start plus the deployment's maximum token lifetime, or persist the used-token
history. A server that implements neither remains conformant.

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

Before its first request, the client verifies the token: the address flags name
at least one family, and the create timestamp is not later than the expire
timestamp; otherwise _invalid connect token_. The client does not verify the
signature (it lacks nothing by not doing so — the server is the party that must,
and does).

**One server, address chosen by family.** The token names one server at up to two
addresses. The client picks the address whose family it has a local socket for,
preferring IPv6 when it has both and the token carries both (an IPv6-capable
client on an IPv6-reachable server should use it). If the chosen address times
out and the other family is present and locally reachable, the client tries the
other address before failing — this is the same one machine, so it is a fallback
between its network paths, not netcode's walk down a candidate list. If no listed
family is locally reachable, the client transitions to _connection request timed
out_ and the correct response is to request a fresh token, exactly as for an
unreachable server.

All other client behaviour — send rates, storing and echoing the challenge token,
transition to _connected_ on the first keep-alive, keep-alive generation, timeout
handling, redundant disconnect packets — is netcode 1.02, reading "sealed" for
"encrypted".

## Server-Side Connection Process

The server manages n client slots as in netcode. Implementations are expected to
support high slot counts; the reference target is 1000.

### Processing Connection Requests

The server follows netcode's strict rules: clients must have a valid connect
token; respond only when necessary; ignore malformed requests as cheaply as
possible; every response smaller than the request so there is nothing to amplify.
Under public key crypto the amplification rule bites harder than in netcode: a
connection request costs the server an Ed25519 verification and a seal-open, so
the cheap plaintext checks and the prefilter run first, before any asymmetric
work, and a source-address rate limit matters more than it did when the first
check was a symmetric open.

In this exact order, on a _connection request packet_:

* If the packet, less the prefilter header, is not 576 bytes, ignore it.
* If the version info is not "ROCKETNET 1.0", ignore it.
* If the protocol id does not match the server's, ignore it.
* If the expire timestamp is <= the current time, ignore it.
* OPTIONAL. If the server implements the restart mitigation and the expire
  timestamp is earlier than server start plus maximum token lifetime, ignore it.
  This uses only the plaintext expire timestamp and costs nothing on a request
  bound for rejection.
* If neither address in the signed data is one this server is advertised at,
  ignore it. (Either advertised address is accepted, not only the first.)
* **Verify the signature over the 512-byte signed connect data against the
  backend public key (`crypto_sign_verify_detached`). If it fails, ignore the
  packet.** This is the authenticity check, and it comes before the seal is
  opened.
* Open the sealed private block with the server's own keypair
  (`crypto_box_seal_open`). If it fails, ignore the packet. A successful open
  combined with a valid signature means the backend authorized this client for
  this server and delivered the session keys confidentially.
* If the opened private data fails to read for any reason, ignore it.
* If a client from the request source address and port is already connected,
  ignore it.
* If a client with this client id is already connected, ignore it.
* If this token's signature already appears in the used-token history with a
  different source address and port, ignore it. Otherwise record the signature
  with this source address and port. (Ed25519 is deterministic, so the signature
  is a stable per-token identifier — the role netcode's private-token tag played.)
* If no slots are available, respond with a _connection denied packet_.
* Add an encryption mapping for the source address and port: packets from it open
  with the client to server key, packets to it seal with the server to client
  key. The mapping expires after _timeout_ seconds without traffic, or if the
  connection is not established within _timeout_ seconds. If it cannot be added,
  ignore the packet.
* Respond with a _connection challenge packet_ and increment the challenge
  sequence number.

### Processing Connection Response Packets

Netcode 1.02, reading "sealed" for "encrypted": open the challenge token with the
server's challenge key and the challenge sequence as nonce; ignore on failure;
ignore if the source address or client id is already connected; respond
_connection denied_ if full; otherwise assign a slot, copy the user data across,
mark the client unconfirmed, and respond with a _connection keep-alive packet_.

### Connected Clients

Client slots, the client index, the confirmed flag, keep-alive prefixing until
confirmation, payload delivery both ways, redundant disconnect packets, and slot
reset on timeout or disconnect are all netcode 1.02.

## Security baseline: never weaker than netcode 1.02

**No security property of this protocol may be weaker than netcode 1.02's. That
is the absolute minimum baseline, and it is a conformance requirement, not an
aspiration.** RocketNet changes the trust model to remove a shared secret; it
does not spend any of netcode's existing guarantees to do so. A change that
buys a new property at the cost of an old one is a defect, however attractive
the new property.

The comparison, property by property:

| Property | netcode 1.02 | RocketNet 1.0 |
|---|---|---|
| Only backend-authorized clients connect | shared-key AEAD tag on the token | Ed25519 signature, verified before the seal is opened | 
| Session keys confidential in transit | encrypted under the shared key | sealed to the server's public key |
| Client identity hidden from observers | client id inside the encrypted token | client id inside the sealed block |
| Token fields unmodifiable | AEAD associated data | covered by the signature |
| Source address spoofing | challenge token, two RTT | identical |
| Packet type unmodifiable | AEAD associated data | identical |
| Packet replay | sequence window, sequence as nonce | identical |
| Token reuse from another address | used-token history keyed on the tag | keyed on the signature |
| Amplification | response smaller than request | identical, and see below |
| Compromised server | **mints tokens for the entire fleet** | mints nothing; leaks only its own sessions |

The last row is the only one where the two differ, and it moves in RocketNet's
favour. Every other row is equal or better.

### Security considerations specific to this design

**The unauthenticated path costs more.** A connection request makes the server
perform a signature verification and a seal-open, where netcode performed one
symmetric decrypt — roughly two orders of magnitude more CPU per unauthenticated
request. This does not weaken any property above, but it lowers the packet rate
at which a flood becomes expensive. Mitigations, in the order they run: the
prefilter rejects anything not shaped like this title's traffic at line rate; the
plaintext checks (size, version, protocol id, expiry, advertised address) reject
before any asymmetric work; and a deployment SHOULD rate limit connection
requests per source address. An implementation that skips the cheap checks and
verifies first is conformant and foolish.

**Connection-request replay is a denial of service against one client, not an
authentication break.** An on-path observer can capture a connection request and
replay it from its own address. It cannot complete the handshake — the challenge
comes back sealed with a key it does not hold — but if its replay arrives first,
the used-token history binds that token to the attacker's address and the
legitimate client is refused until it obtains a fresh token. This property is
inherited from netcode 1.02 unchanged, and the response is the same: the client
requests a new token. It is recorded here rather than left for an implementer to
discover.

**A sealed box proves nothing about its sender.** Stated in _General
Conventions_ and repeated here because it is the one way a public-key rebuild of
netcode fails quietly: anyone can seal a block to a server's public key, since
that key is public. If an implementation treats a successful open as evidence of
authenticity and skips the signature, any party can mint connect tokens for that
server. The ordering in _Processing Connection Requests_ — verify, then open — is
normative for this reason.

## Conformance

An implementation conforms if it produces and accepts the exact binary formats
above, performs the processing steps in the exact orders given, and holds the key
inventory of the _Keys_ section — in particular, no secret shared between backend
and servers, and signature verification before the sealed block is trusted. The
prefilter header must be present and correctly formed on every packet an
implementation sends; whether a receiver checks it in userspace, in a kernel
program, or not at all is a quality-of-implementation choice, since the sealed
layer beneath it carries the security guarantees.

## Deviations from netcode 1.02, in one place

1. Private connect token data is sealed to the server with `crypto_box_seal` to
   its X25519 public key, instead of encrypted under a key shared by the fleet.
2. Token authenticity is an Ed25519 signature, verified by the server before the
   seal is opened, instead of the shared-key AEAD tag.
3. A server holds the backend public key and its own keypair, not a shared
   private key. No secret is shared between backend and servers.
4. One server per token at up to two addresses (IPv4 and/or IPv6), instead of a
   list of up to 32 candidate servers. Failover is a fresh token from the
   backend.
5. The prefilter is generated per title (issue #17); pittle/chonkle and the
   stateful "magic" filter are retired.
6. Dual stack is designed in: a server accepting either advertised address,
   family-aware client address choice, and boundary address normalization as a
   normative rule.
7. One packet-size budget of 1232 bytes from the IPv6 minimum MTU; the token is
   a fixed 768 bytes, one MTU by design.

Everything else — challenge tokens, per-packet AEAD and its associated data,
replay protection, the nonce/restart discipline, the client state machine, the
server slot model — is netcode 1.02.
