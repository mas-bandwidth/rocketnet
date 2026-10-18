# RN-1: RocketNet connection layer

Version: 1.0 (draft)<br>
Status: normative<br>
Companion documents: RN-2 (`MESSAGES.md`), the message layer; RN-R
(`RATIONALE.md`), informative rationale.

Abstract. RN-1 specifies a secure client/server connection protocol over UDP:
a connect token minted by a backend, an address-validated handshake, sealed
packets with per-connection keys, replay protection, acknowledgement and
fragmentation. An implementation built from RN-1 alone interoperates with any
other conforming implementation.

<a id="t1"></a>
## t1. Introduction, scope, terminology and conventions

<a id="t1.1"></a>
### t1.1. Requirements language

The key words "MUST", "MUST NOT", "REQUIRED", "SHALL", "SHALL NOT",
"SHOULD", "SHOULD NOT", "RECOMMENDED", "NOT RECOMMENDED", "MAY", and
"OPTIONAL" in this document and in RN-2 are to be interpreted as described in
BCP 14 (RFC 2119, RFC 8174) when, and only when, they appear in all capitals,
as shown here.

<a id="t1.2"></a>
### t1.2. Scope

RN-1 specifies the bytes on the wire, the order of every check a receiver
makes, the key schedule, the states of a connection and the defaults of every
tunable value of the connection layer. It specifies the interface RN-2 relies
on ([t20 Conformance](#t20) lists what an implementation claims).

RN-1 does not specify:

1. how a backend authenticates a player or chooses a server;
2. how a server's public key reaches the backend, except that the channel is
   authenticated ([t2 Architecture and key inventory](#t2));
3. the transports that carry the bytes. Every transport carries the same
   bytes, and nothing in RN-1 varies by transport;
4. the content of a payload. A payload is opaque to RN-1; RN-2 specifies the
   message layer carried inside it.

<a id="t1.3"></a>
### t1.3. Terminology

| Term | Meaning |
|---|---|
| backend | the service that mints connect tokens. It holds the signing key. |
| server | the endpoint that accepts connections. It holds a key exchange keypair and the backend's public key. |
| client | the endpoint that initiates a connection with a connect token. |
| endpoint | a client or a server. |
| transport | a selectable packet backend of an implementation (for example `xdp`, `gso`, `msgx`, `portable`), chosen by name in the endpoint's configuration. |
| connect token | the 768-byte credential a backend gives a client ([t6 Connect token](#t6)). |
| slot | one of a server's connection places, indexed from 0 below `max_clients`. |
| connection | one client's session with one server, from the request that creates its mapping to its end. |
| attempt | one run of the client handshake, with a fresh connection salt. |
| key phase | the handshake phase or the session phase of a connection; each has its own keys and sequences. |
| sequence | the per-packet counter of one direction of one key phase ([t4 Wire encoding primitives](#t4)). |
| session packet | a packet of type 4, 5, 6, 8 or 9. |
| payload | the application bytes of a type 5 packet, or of a whole fragmented payload of type 9 packets. |
| sealed | encrypted and authenticated with the AEAD of [t9 Packet protection](#t9). |
| sealed to the server | encrypted with `crypto_box_seal` under the server's X25519 public key. |
| drop | discard a packet with no reply and no change of state, and count it ([t18 Error handling and counters](#t18)). |
| time | a value in seconds that the application passes to the endpoint on each update. |

<a id="t1.4"></a>
### t1.4. Conventions

These conventions apply to every structure in RN-1 and RN-2, and no section
restates them.

1. Every multi-byte integer is little-endian. This includes port numbers, the
   16-bit groups of an IPv6 address, nonces formed from sequences, and
   associated data passed to an AEAD.
2. Every structure is packed: fields lie end to end at the offsets their
   sizes imply, with no padding. Where a structure is longer than its fields,
   the remainder is a named `zero[n]` field.
3. A sender MUST write every `zero[n]` field as zero bytes. A receiver MUST
   drop a structure whose `zero[n]` field is not all zero, unless the section
   that defines it says otherwise.
4. Every size is in bytes unless its unit is named.
5. Timestamps on the wire are unsigned 64-bit counts of seconds since the
   unix epoch.
6. Elapsed time is an input. An endpoint MUST take the current time as an
   argument of its update, and MUST NOT derive a protocol decision from a
   clock it reads itself.
7. Cryptographic primitives are those of libsodium, named by function, with
   the sizes of [t3 Constants and registries](#t3):
   `crypto_sign` (Ed25519), `crypto_box_seal` (X25519 sealed box),
   `crypto_scalarmult` (X25519), `crypto_generichash` (BLAKE2b),
   `crypto_shorthash` (SipHash-2-4) and
   `crypto_aead_chacha20poly1305_ietf` (ChaCha20-Poly1305, IETF).
8. `a ‖ b` is the concatenation of byte strings `a` and `b`.

<a id="t1.5"></a>
### t1.5. How to read this document

Each section states its rule first, then its fields, then its states, then its
vectors. A constant is named by its row in
[t3 Constants and registries](#t3) and never repeated. The reasons are in
RN-R under the same section id; [RN-R t1 Introduction, scope, terminology and conventions](RATIONALE.md#t1)
explains this layout.

<a id="t2"></a>
## t2. Architecture and key inventory

<a id="t2.1"></a>
### t2.1. Components

RN-1 has three parties:

1. The backend authenticates players, decides which server a player joins,
   and mints connect tokens. It holds the only signing key.
2. A server accepts connections. It holds its own key exchange keypair and the
   backend's public key, and no secret shared with the backend.
3. A client connects with a connect token it receives from the backend over a
   confidential, authenticated channel outside RN-1 (for example HTTPS).

A connection proceeds in this order:

1. The client authenticates with the backend and asks to play.
2. The backend mints a connect token for one server and returns it to the
   client.
3. The client runs the handshake of [t11 Handshake: client](#t11) against
   that server; the server runs [t12 Handshake: server](#t12).
4. Client and server exchange sealed session packets
   ([t13 Session: keep-alive, timeout, disconnect, slot reset](#t13)).

<a id="t2.2"></a>
### t2.2. Key inventory

Every key in RN-1 is listed here. An implementation MUST NOT hold, derive or
transmit a key this table does not list, and MUST NOT let a key leave the
party the table names as its holder, except as the table states.

| Key | Holder | Primitive | Size row | Lifetime | Leaves the holder |
|---|---|---|---|---|---|
| backend signing secret key | backend | Ed25519 | Ed25519 secret key | long term | never |
| backend public key | backend, every server, every client | Ed25519 | Ed25519 public key | long term | as configuration |
| server secret key | one server | X25519 | X25519 secret key | long term | never |
| server public key | that server, the backend | X25519 | X25519 public key | long term | to the backend, over the provisioning channel |
| cookie key | one server | BLAKE2b key | token key | one rotation period | never; never stored |
| lookup key | one server | SipHash-2-4 key | SipHash key | one server run | never |
| token keys (client to server, server to client) | the backend, then one client and one server | ChaCha20-Poly1305 key input | token key | one connect token | inside the connect token: in clear to the client, sealed to the server |
| ephemeral keypair | one client attempt, one server mapping | X25519 | X25519 secret key | one attempt | the public half, in handshake packets |
| derived keys (four per connection) | one client, one server | ChaCha20-Poly1305 key | AEAD key | one connection | never |
| prefilter magic | backend, servers, clients of one deployment | BLAKE2b key | prefilter magic | one magic generation | as configuration |

Rules:

1. A server MUST verify the backend's signature on a connect token before it
   trusts any byte of the token's sealed block. A sealed box authenticates
   nothing about who sealed it.
2. The channel that registers a server's public key with the backend MUST be
   integrity-protected and authenticated to the backend. Its form is outside
   RN-1.
3. A server MUST generate its cookie key and its lookup key from a
   cryptographically secure random source at start, and MUST NOT write either
   to storage.
4. The prefilter magic is the one secret shared between parties. No key,
   token or derivation in RN-1 depends on it, and every guarantee of RN-1
   other than flood rejection holds when it is disclosed
   ([t5 Prefilter header](#t5)).
5. An endpoint MUST erase an ephemeral secret key, and every derived key, when
   the attempt or connection that owns it ends
   ([t7 Key schedule](#t7)).

<a id="t2.3"></a>
### t2.3. Configuration surface

A client is configured by one client configuration and a server by one
server configuration. Each configuration declares every field once, with its
type, default and valid range; the field's default is the value of its row in
[t3 Constants and registries](#t3) or in the owning RN-2 section. These rules bind every field of both
configurations, including the message layer's settings, the send budgets of
RN-2 and `max_clients`:

1. Every field MUST have an environment variable that overrides it, named
   `ROCKETNET_SERVER_` or `ROCKETNET_CLIENT_` followed by the field name in
   upper case (the server field `max_clients` is `ROCKETNET_SERVER_MAX_CLIENTS`).
   The only fields exempt are fields that hold a callback or a pointer, and
   each exempt field MUST be stated as exempt where it is declared.
2. Precedence is default, then configuration struct, then environment: a
   value from the environment replaces a value from the struct, which
   replaces the default.
3. An endpoint MUST log each environment override at create, naming the
   variable and the value it displaced. A field that holds secret material
   MUST be logged with its value redacted.
4. An endpoint MUST refuse to create, with an error naming the field, when any
   field holds a value outside its valid range, from the struct or from the
   environment alike. It MUST NOT clamp or otherwise correct a value.

<a id="t2.4"></a>
### t2.4. Transport selection

The field `transport` of each configuration selects the transport that carries
the endpoint's packets. Its environment variables are
`ROCKETNET_SERVER_TRANSPORT` and `ROCKETNET_CLIENT_TRANSPORT`. A transport
never changes a byte on the wire. An implementation with exactly one
transport MAY ignore the rules of this subsection except rule 6.

1. The value is an ordered, comma-separated list of transport names. The
   endpoint MUST try the candidates left to right at create; the first that
   succeeds carries the endpoint.
2. The value `auto`, and the empty value, MUST expand to the implementation's
   documented order for that endpoint on that platform.
3. An explicit list MUST be used exactly as given: nothing appended, nothing
   reordered. A list with one applicable name pins that transport: create
   succeeds on it or fails.
4. A name the implementation knows but that is not available to this
   endpoint on this platform MUST be skipped with one log line. A name the
   implementation does not know MUST fail the create, listing the valid
   names. A list whose every name was skipped MUST fall to `auto` with one log
   line.
5. A transport MUST NOT change machine state (kernel parameters, queue
   configuration, interrupt affinity) at create. It checks its conditions,
   and refuses by name when one is unmet.
6. An endpoint MUST report the name of the transport that carries it, and MUST
   log at create the chosen transport and the reason each earlier candidate
   refused.

<a id="t3"></a>
## t3. Constants and registries

Every constant and every default of RN-1 is a row of the table below, and is
stated nowhere else. A section that uses a value names its row. A row whose
kind is `default` is a value a deployment MAY change through the
configuration surface of [t2 Architecture and key inventory](#t2); every
other row is fixed by RN-1, and an implementation MUST use it exactly.

Registry rules:

1. A receiver MUST drop a packet whose type is the first reserved packet type
   or above.
2. Version, packet types and key derivation labels are assigned only by a
   revision of RN-1. A revision that changes any byte on the wire MUST assign
   a new version.
3. Each key derivation label is 8 ASCII bytes with no terminating zero.

| Name | Value | Unit | Kind | Defined in |
|---|---|---|---|---|
| version | `0x4E7C1A93` | | version | [t6 Connect token](#t6) |
| connection request packet | `0` | | packet type | [t8 Packet formats](#t8) |
| connection denied packet | `1` | | packet type | [t8 Packet formats](#t8) |
| connection challenge packet | `2` | | packet type | [t8 Packet formats](#t8) |
| connection response packet | `3` | | packet type | [t8 Packet formats](#t8) |
| connection keep-alive packet | `4` | | packet type | [t8 Packet formats](#t8) |
| connection payload packet | `5` | | packet type | [t8 Packet formats](#t8) |
| connection disconnect packet | `6` | | packet type | [t8 Packet formats](#t8) |
| connection cookie packet | `7` | | packet type | [t8 Packet formats](#t8) |
| connection ack packet | `8` | | packet type | [t8 Packet formats](#t8) |
| connection payload fragment packet | `9` | | packet type | [t8 Packet formats](#t8) |
| first reserved packet type | `10` | | packet type | [t10 Receive processing](#t10) |
| unkeyed filter epoch | `0` | | registry | [t5 Prefilter header](#t5) |
| handshake client to server label | `"RKTNHNDC"` | | key derivation label | [t7 Key schedule](#t7) |
| handshake server to client label | `"RKTNHNDS"` | | key derivation label | [t7 Key schedule](#t7) |
| session client to server label | `"RKTNSESC"` | | key derivation label | [t7 Key schedule](#t7) |
| session server to client label | `"RKTNSESS"` | | key derivation label | [t7 Key schedule](#t7) |
| Ed25519 public key | `32` | bytes | size | [t2 Architecture and key inventory](#t2) |
| Ed25519 secret key | `64` | bytes | size | [t2 Architecture and key inventory](#t2) |
| Ed25519 signature | `64` | bytes | size | [t6 Connect token](#t6) |
| X25519 public key | `32` | bytes | size | [t2 Architecture and key inventory](#t2) |
| X25519 secret key | `32` | bytes | size | [t2 Architecture and key inventory](#t2) |
| sealed box overhead | `48` | bytes | size | [t6 Connect token](#t6) |
| AEAD key | `32` | bytes | size | [t9 Packet protection](#t9) |
| AEAD nonce | `12` | bytes | size | [t9 Packet protection](#t9) |
| AEAD tag | `16` | bytes | size | [t9 Packet protection](#t9) |
| BLAKE2b key | `32` | bytes | size | [t7 Key schedule](#t7) |
| SipHash key | `16` | bytes | size | [t2 Architecture and key inventory](#t2) |
| prefilter header | `6` | bytes | size | [t5 Prefilter header](#t5) |
| filter value | `4` | bytes | size | [t5 Prefilter header](#t5) |
| prefilter magic | `16` | bytes | size | [t5 Prefilter header](#t5) |
| sequence | `4` | bytes | size | [t4 Wire encoding primitives](#t4) |
| per-packet overhead | `26` | bytes | size | [t8 Packet formats](#t8) |
| associated data | `21` | bytes | size | [t9 Packet protection](#t9) |
| associated data with ephemeral key | `53` | bytes | size | [t9 Packet protection](#t9) |
| connection salt | `8` | bytes | size | [t7 Key schedule](#t7) |
| session nonce | `8` | bytes | size | [t7 Key schedule](#t7) |
| connect token | `768` | bytes | size | [t6 Connect token](#t6) |
| signed connect data | `576` | bytes | size | [t6 Connect token](#t6) |
| sealed block | `432` | bytes | size | [t6 Connect token](#t6) |
| private block | `384` | bytes | size | [t6 Connect token](#t6) |
| user data | `256` | bytes | size | [t6 Connect token](#t6) |
| token key | `32` | bytes | size | [t6 Connect token](#t6) |
| token identifier | `16` | bytes | size | [t6 Connect token](#t6) |
| ack block minimum | `7` | bytes | size | [t15 Acknowledgement](#t15) |
| fragment header | `6` | bytes | size | [t16 Fragmentation and reassembly](#t16) |
| fragments per payload maximum | `256` | fragments | size | [t16 Fragmentation and reassembly](#t16) |
| link MTU | `1500` | bytes | size | [t8 Packet formats](#t8) |
| receive ceiling | `1472` | bytes | size | [t10 Receive processing](#t10) |
| maximum packet size limit | `1472` | bytes | size | [t8 Packet formats](#t8) |
| payload capacity | `1416` | bytes | size | [t8 Packet formats](#t8) |
| received payload maximum | `1439` | bytes | size | [t10 Receive processing](#t10) |
| fragment stride | `1413` | bytes | size | [t16 Fragmentation and reassembly](#t16) |
| received fragment stride maximum | `1433` | bytes | size | [t16 Fragmentation and reassembly](#t16) |
| maximum packet size | `1452` | bytes | default | [t8 Packet formats](#t8) |
| replay window depth | `4096` | sequences | default | [t14 Replay protection](#t14) |
| send rate | `10` | hertz | default | [t17 Timing and defaults](#t17) |
| handshake deadline | `5` | seconds | default | [t12 Handshake: server](#t12) |
| cookie lifetime | `3` | seconds | default | [t12 Handshake: server](#t12) |
| cookie key rotation | `60` | seconds | default | [t12 Handshake: server](#t12) |
| server redundant disconnect packets | `10` | packets | default | [t13 Session: keep-alive, timeout, disconnect, slot reset](#t13) |
| client redundant disconnect packets | `10` | packets | default | [t13 Session: keep-alive, timeout, disconnect, slot reset](#t13) |
| ack debt threshold | `64` | packets | default | [t15 Acknowledgement](#t15) |
| used-token history per slot | `8` | entries | default | [t12 Handshake: server](#t12) |

The token identifier of a connect token is the BLAKE2b hash, of token
identifier size and with no key, of the token's signed connect data. For the
frozen connect token of [t6 Connect token](#t6) it is:

```vector t3.token-identifier
input: signed connect data, bytes 0 to 575 of the frozen connect token
input: crypto_generichash, output 16 bytes, no key
f5 20 da 9d 95 fd 07 22 56 b4 7d 75 5f 81 0f 61
```

<a id="t4"></a>
## t4. Wire encoding primitives

Status: pending

Scope: integer encodings by type name; the 4-byte wire sequence and its
reconstruction to 64 bits against the highest opened sequence, with the tie
rule; sequence zero never valid; address encoding (family flag, IPv4 four
octets then port, IPv6 eight groups then port, all little-endian).

<a id="t5"></a>
## t5. Prefilter header

Status: pending

Scope: the 6-byte header on every packet (type, epoch, filter value); the
unkeyed value at epoch 0; the keyed value at epochs 1 to 255 and the magic
generation wrap; structural checks; vectors.

<a id="t6"></a>
## t6. Connect token

Status: pending

Scope: the private block, the signed connect data, the 768-byte token layout,
minting and verification order (signature before sealed block), the token
identifier, the frozen connect token vector.

<a id="t7"></a>
## t7. Key schedule

Status: pending

Scope: the connection salt, the session nonce, the ephemeral exchange, the
four derived keys and their labels, erasure; vectors.

<a id="t8"></a>
## t8. Packet formats

Status: pending

Scope: the layout of each packet type, its fixed size or size range,
and the maximum packet size rule for senders; assembled vectors.

<a id="t9"></a>
## t9. Packet protection

Status: pending

Scope: the AEAD, nonce formation from the sequence, associated data for each
packet type, which key seals which type; sealed vectors.

<a id="t10"></a>
## t10. Receive processing

Status: pending

Scope: the receiver's ordered checks for every packet, from prefilter to
dispatch, with the counter each refusal increments.

<a id="t11"></a>
## t11. Handshake: client

Status: pending

Scope: the client states, with their values, and the client's state table
from requesting a cookie to connected.

<a id="t12"></a>
## t12. Handshake: server

Status: pending

Scope: cookie issue and validation, connection requests, the used-token
history, encryption mappings, challenge and response, slot assignment, the
server's state table.

<a id="t13"></a>
## t13. Session: keep-alive, timeout, disconnect, slot reset

Status: pending

Scope: keep-alive generation, connection timeout from the token, the
redundant disconnect, slot reset and what a reset clears.

<a id="t14"></a>
## t14. Replay protection

Status: pending

Scope: the replay window, its minimum depth and configured depth, the check
placement against decryption, the optional kernel prefilter.

<a id="t15"></a>
## t15. Acknowledgement

Status: pending

Scope: the ack block opening every session packet, its encoding, elicitation,
the standalone ack, processing and the delivery notification RN-2 relies on.

<a id="t16"></a>
## t16. Fragmentation and reassembly

Status: pending

Scope: the fragment header, fragment count limits, reassembly slots,
staleness and abandonment; vectors.

<a id="t17"></a>
## t17. Timing and defaults

Status: pending

Scope: the timing rules (rates in elapsed time, never per update; fixed
retransmission with no backoff; deadlines), naming the default rows of
[t3 Constants and registries](#t3) without restating them.

<a id="t18"></a>
## t18. Error handling and counters

Status: pending

Scope: the counter set, one counter per refusal, and the observable
connection state.

<a id="t19"></a>
## t19. Security considerations

Status: pending

Scope: what RN-1 defends against and what it does not; the provisioning
trust anchor; amplification; disclosure of the prefilter magic.

<a id="t20"></a>
## t20. Conformance

Status: pending

Scope: what an implementation claims, the conformance classes (client,
server, backend), and the vectors each class reproduces.

<a id="ta"></a>
## ta. Appendix A: test vectors

This appendix indexes every vector of RN-1 by the section that defines it.
It is generated; the bytes are in the sections.

<!-- vector-index:begin -->
| Section | Vector |
|---|---|
| [t3 Constants and registries](#t3) | `t3.token-identifier` |
<!-- vector-index:end -->

<a id="tb"></a>
## tb. Appendix B: changes from netcode 1.02

Status: pending

Scope: a list of facts, one per difference on the wire or in processing.

<a id="tc"></a>
## tc. Appendix C (informative): implementation notes

Status: pending

Scope: informative notes for implementers; no requirements.
