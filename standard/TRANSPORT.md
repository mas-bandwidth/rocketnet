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
| transport | a selectable packet carrier of an implementation (for example `xdp`, `gso`, `msgx`, `portable`), chosen by name in the endpoint's configuration. |
| connect token | the fixed-size credential a backend gives a client ([t6 Connect token](#t6)). |
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
| cookie key | one server | BLAKE2b key | BLAKE2b key | one rotation period | never; never stored |
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
type, default and valid range. The fields of the connection layer are the
table below; the fields of the message layer, including its send budgets, are
declared in the RN-2 section that owns each one. These rules bind every field
of both configurations:

1. Every field MUST have an environment variable that overrides it, named
   `ROCKETNET_SERVER_` or `ROCKETNET_CLIENT_` followed by the field name in
   upper case (the server field `max_clients` is `ROCKETNET_SERVER_MAX_CLIENTS`).
   The only fields exempt are fields that hold a callback or a pointer, and
   each exempt field MUST be stated as exempt where it is declared.
2. Precedence is default, then configuration struct, then environment: a
   value from the environment replaces a value from the struct, which
   replaces the default.
3. An environment value is parsed by the field's type: an integer field as a
   decimal integer with an optional leading minus sign; an unsigned 64-bit
   field as a decimal integer; a bytes field as standard base64 (RFC 4648,
   section 4, with padding) that decodes to exactly the field's size; a string
   field as its bytes, at most the field's capacity. An endpoint MUST refuse to
   create, naming the variable, when a value does not parse. An empty value
   is treated as an unset variable, with one log line.
4. An endpoint MUST log each environment override at create, naming the
   variable and the value it displaced. A field that holds secret material
   MUST be logged, and refused, with its value redacted.
5. An endpoint MUST refuse to create, with an error naming the field, when any
   field holds a value outside its valid range, from the struct or from the
   environment alike. It MUST NOT clamp or otherwise correct a value.

The fields of the connection layer. A Value of `none` is a field with no
numeric default: a key the deployment sets, or a bytes field whose unset
value its range cell names. Where a range admits 0, a value of 0 selects the
field's default.

| Name | Value | Type | Environment variable | Valid range |
|---|---|---|---|---|
| server protocol_id | `0` | u64 | `ROCKETNET_SERVER_PROTOCOL_ID` | any; equal to the connect token's protocol id |
| server max_clients | `1000` | integer | `ROCKETNET_SERVER_MAX_CLIENTS` | 1 to the server max_clients limit |
| server max_clients limit | `1000` | integer | none: a limit of this revision | |
| server filter_value | `none` | bytes[4] | `ROCKETNET_SERVER_FILTER_VALUE` | 4 bytes; unset, the unkeyed filter value of [t5 Prefilter header](#t5) |
| server backend_public_key | `none` | bytes[32] | `ROCKETNET_SERVER_BACKEND_PUBLIC_KEY` | an Ed25519 public key |
| server server_public_key | `none` | bytes[32] | `ROCKETNET_SERVER_SERVER_PUBLIC_KEY` | an X25519 public key |
| server server_secret_key | `none` | bytes[32] | `ROCKETNET_SERVER_SERVER_SECRET_KEY` | an X25519 secret key; secret |
| server max_payload_bytes | `1416` | integer | `ROCKETNET_SERVER_MAX_PAYLOAD_BYTES` | 0 (the default), or the payload capacity row of [t3 Constants and registries](#t3) to the fragmented payload maximum |
| server num_reassembly_slots | `8` | integer | `ROCKETNET_SERVER_NUM_REASSEMBLY_SLOTS` | 0 (the default), or 1 to the fragments per payload maximum row of [t3 Constants and registries](#t3) |
| server transport | `"auto"` | string | `ROCKETNET_SERVER_TRANSPORT` | [t2.4 Transport selection](#t2.4) |
| server public_address | `""` | string | `ROCKETNET_SERVER_PUBLIC_ADDRESS` | an address and port, or empty |
| server max_timeout_seconds | `60` | integer | `ROCKETNET_SERVER_MAX_TIMEOUT_SECONDS` | 1 to 3600 |
| server handshake_deadline_seconds | `5` | integer | `ROCKETNET_SERVER_HANDSHAKE_DEADLINE_SECONDS` | 1 to 60 |
| client filter_value | `none` | bytes[4] | `ROCKETNET_CLIENT_FILTER_VALUE` | 4 bytes; unset, the unkeyed filter value of [t5 Prefilter header](#t5) |
| client max_payload_bytes | `1416` | integer | `ROCKETNET_CLIENT_MAX_PAYLOAD_BYTES` | as the server field |
| client num_reassembly_slots | `8` | integer | `ROCKETNET_CLIENT_NUM_REASSEMBLY_SLOTS` | as the server field |
| client transport | `"auto"` | string | `ROCKETNET_CLIENT_TRANSPORT` | [t2.4 Transport selection](#t2.4) |
| fragmented payload maximum | `361728` | bytes | none: a limit of this revision | |

An implementation MAY add fields of its own (for example a count of kernel
queues for one transport); each one keeps rules 1 to 5.

<a id="t2.4"></a>
### t2.4. Transport selection

The field `transport` selects the transport that carries the endpoint's
packets. A transport never changes a byte on the wire. A transport is
applicable to an endpoint when the implementation offers it for that
endpoint's role (client or server) on the platform it runs on. A transport
that the implementation documents as pin-only (for example an in-process
loopback for tests) is never part of an `auto` order and is applicable only
as the sole name of a list. An implementation with exactly one transport MAY
ignore rules 1 to 5 and keeps rule 6.

1. The value is `auto`, or empty, or an ordered, comma-separated list of
   transport names. `auto` and the empty value MUST expand to the
   implementation's documented order for that endpoint's role on that
   platform. `auto` is valid only as the whole value: a list that holds
   `auto` beside any other name, or holds an empty name, MUST fail the
   create, naming the fault.
2. A name the implementation does not know MUST fail the create, listing the
   valid transport names, whatever the length of the list.
3. A list of one name is a pin. The endpoint MUST create on that transport or
   fail the create naming the transport and why it refused or is not
   applicable. A pin never falls to another transport.
4. A list of two or more transport names MUST be tried left to right, nothing
   appended and nothing reordered; the first applicable transport that
   succeeds carries the endpoint. A name that is not applicable MUST be
   skipped with one log line. When no name in the list is applicable, the
   endpoint MUST fall to its `auto` order, with one log line saying so. When
   at least one name is applicable and every applicable name refuses, the
   endpoint MUST fail the create, naming each refusal apart from each skip.
5. A transport MUST NOT change machine state (kernel parameters, queue
   configuration, interrupt affinity) at create. It checks its conditions,
   and refuses by name when one is unmet.
6. An endpoint MUST report the name of the transport that carries it, and
   MUST log at create the chosen transport and the reason each earlier
   candidate was skipped or refused.

<a id="t3"></a>
## t3. Constants and registries

Every constant of RN-1 is a row of the table below, and is stated nowhere
else; every configuration field and its default is a row of the table in
[t2.3 Configuration surface](#t2.3). A section that uses a value names its
row. An implementation MUST use every row of this table exactly. A row of
kind `protocol value` is a rate, a count or a size the protocol fixes rather
than a wire format; it is not configurable in this revision.

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
| maximum packet size | `1452` | bytes | protocol value | [t8 Packet formats](#t8) |
| replay window depth minimum | `4096` | sequences | size | [t14 Replay protection](#t14) |
| replay window depth maximum | `32768` | sequences | size | [t14 Replay protection](#t14) |
| send rate | `10` | hertz | protocol value | [t17 Timing and defaults](#t17) |
| cookie lifetime | `3` | seconds | protocol value | [t12 Handshake: server](#t12) |
| cookie key rotation | `60` | seconds | protocol value | [t12 Handshake: server](#t12) |
| server redundant disconnect packets | `10` | packets | protocol value | [t13 Session: keep-alive, timeout, disconnect, slot reset](#t13) |
| client redundant disconnect packets | `10` | packets | protocol value | [t13 Session: keep-alive, timeout, disconnect, slot reset](#t13) |
| ack debt threshold | `64` | packets | protocol value | [t15 Acknowledgement](#t15) |
| used-token history per slot | `8` | entries | protocol value | [t12 Handshake: server](#t12) |

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

The server turns a connection request into an encryption mapping, and a connection response into a client slot. It answers only traffic that has cleared the rung that provoked the answer.

1. On a connection request, a server MUST apply the checks below in the order given, and MUST stop at the first that refuses.
2. A server MUST ignore a request whose size is not the request wire size, whose version is not the protocol version, whose protocol id is not the server's, whose timeout is zero or negative without permission, whose address flags name no family, whose advertised address is not one this server serves, or whose server public key is not this server's. It MUST NOT answer such a request.
3. A server MUST ignore a request from a source address that is already connected. A request from a source address that holds a mapping without a connected slot MUST be answered with that mapping's stored connection challenge packet, byte-identical, and it MUST NOT create a second mapping or derive new keys for that address. A request whose connection salt differs from the stored mapping's salt MUST be ignored.
4. A server MUST refuse a token whose expire timestamp is not later than the server's current time, whose create timestamp is more than the creation skew after the server's current time, or whose lifetime exceeds the configured maximum token lifetime. It MUST count the first refusal as `CONNECTION_REQUEST_TOKEN_EXPIRED`, the second as `CONNECTION_REQUEST_TOKEN_CREATED_IN_FUTURE`, and the third as `CONNECTION_REQUEST_TOKEN_LIFETIME_TOO_LONG`, and MUST NOT answer.
5. A server MUST require a valid return cookie before any asymmetric work, and MUST answer a request without one, with a forged one, or with an expired one with a fresh connection cookie packet.
6. A server MUST refuse, unanswered, a request whose cookie is valid when the per-update handshake work budget is spent, counting `CONNECTION_REQUEST_HANDSHAKE_BUDGET_EXCEEDED`.
7. A server MUST refuse, unanswered, a request whose token identifier is bound to a different source address, and a request from a source that has spent the failed-signature budget in the current window, counting `CONNECTION_REQUEST_SIGNATURE_VERIFY_LIMIT`.
8. A server MUST verify the signed connect data before opening the sealed private block, MUST open it only with the keypair its public key names, and MUST ignore a request whose sealed fields disagree with the signed fields or whose client id already holds a slot.
9. A server MUST deny, with a connection denied packet sealed under the handshake server-to-client key, a request that finds no free slot, and MUST NOT create a mapping for it.
10. A server MUST generate a fresh session nonce and ephemeral keypair for each mapping, MUST derive the connection keys, MUST erase the ephemeral secret, and MUST store the connection challenge in the mapping so that a repeat is byte-identical.
11. A connection response MUST be processed only after it has opened under the mapping's handshake client-to-server key. A server MUST bind the token identifier to the source address at that point, MUST ignore a response when the source address or the client id already holds a slot, MUST deny one that finds no free slot, and otherwise MUST assign a slot, mark the client unconfirmed, and send the first keep-alive.
12. A mapping without a connected slot MUST be removed at its handshake deadline, which is fixed at creation and is never refreshed, or at the half-open idle deadline, which traffic refreshes; whichever comes first. A connected slot MUST be removed at its idle timeout. A server MUST count these removals apart.

The policy values of this section are below. A value of `0` for the maximum token lifetime selects no limit.

| Name | Value | Unit |
|---|---|---|
| token creation skew | `5` | seconds |
| maximum token lifetime | `0` | seconds |
| handshake work budget per update | `1024` | requests |
| failed-signature budget | `90` | verifications |
| failed-signature window | `10` | seconds |
| failed-signature source table | `64` | sources |
| half-open idle timeout | `10` | seconds |

The cookie lifetime, the cookie key rotation period and the used-token history
capacity per slot are rows of [t3 Constants and registries](#t3). The handshake
deadline is the server configuration field of
[t2.3 Configuration surface](#t2.3).

The states of one source address are below. An event that a state does not
list is ignored in that state.

| State | Event | Guard | Action | Next state |
|---|---|---|---|---|
| no mapping | request | any fixed-offset check fails | ignore, count the reason | no mapping |
| no mapping | request | token expired | ignore, count | no mapping |
| no mapping | request | token created in the future | ignore, count | no mapping |
| no mapping | request | token lifetime too long | ignore, count | no mapping |
| no mapping | request | cookie absent, forged or expired | send a fresh cookie packet | no mapping |
| no mapping | request | handshake work budget spent | ignore, count | no mapping |
| no mapping | request | token used from another address | ignore, count | no mapping |
| no mapping | request | failed-signature budget spent | ignore, count | no mapping |
| no mapping | request | signature, seal or sealed-field check fails | ignore, count | no mapping |
| no mapping | request | client id connected | ignore, count | no mapping |
| no mapping | request | no free slot | send connection denied | no mapping |
| no mapping | request | all checks pass | create mapping, derive keys, send challenge | mapping |
| mapping | request | source connected, salt matches | resend the stored challenge | mapping |
| mapping | request | source connected, salt differs | ignore, count | mapping |
| mapping | request | source not connected, salt matches | resend the stored challenge, refresh idle | mapping |
| mapping | response | mapping has no connected slot, source or client id already connected | ignore, count | mapping |
| mapping | response | no free slot | send connection denied | mapping |
| mapping | response | free slot | bind token, assign slot, send keep-alive | connected |
| mapping | handshake deadline reached | never refreshed | remove mapping, count | no mapping |
| mapping | idle deadline reached | no traffic inside the timeout | remove mapping, count | no mapping |
| connected | session packet | any | refresh idle | connected |
| connected | disconnect | any | reset slot, remove mapping | no mapping |
| connected | idle timeout | any | reset slot, remove mapping | no mapping |

```vector t12.cookie
input: source 198.51.100.7:56789
input: connection salt 53414c5430303031, server time 1756166400, cookie lifetime 3
input: cookie key 102030405060708090a0b0c0d0e0f0000f1f2f3f4f5f6f7f8f9fafbfcfdfeff
07 00 b7 59 01 00 53 41 4c 54 30 30 30 31 03 f9
ac 68 00 00 00 00 0f b2 b2 8f 32 db d6 c9 eb 36
4f 57 74 86 f3 e9
```

```vector t12.stale-cookie
input: source 203.0.113.44:40001
input: connection salt 53414c5430303031, server time 1756166400
input: return cookie expire 1756166300, a valid mac for this address and salt
07 00 b7 59 01 00 53 41 4c 54 30 30 30 31 03 f9
ac 68 00 00 00 00 59 2e 1f 69 fa 21 b0 74 4b 92
09 f7 9a 33 94 fb
```

```vector t12.challenge
input: connection salt 53414c5430303031, session nonce 4e4f4e4345303031
input: token keys 0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20 and a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebfc0
input: server ephemeral public key 489cebba7a166c619835fe12eedc581dbfc1b36de10c830e4f0bc1c48734864b
input: client ephemeral public key 3a553d74792d727efa9b9a4cde3da1ad93f1a2d0c09cb639b1a3c0fda14cbe24
input: protocol id 1122334455667788, filter value b7590100, sequence 1
02 00 b7 59 01 00 01 00 00 00 4e 4f 4e 43 45 30
30 31 68 6f b7 09 43 5f f0 0e 1a c2 60 ee ea fb
b2 91 20 20 10 86 c8 27 62 55 d7 d3 87 c3 92 32
94 7b 3d a1 3d dd 68 56 b0 94 15 f3 08 6c 59 f7
6f 88
```

```vector t12.keep-alive
input: connection salt 53414c5430303031, session nonce 4e4f4e4345303031
input: the ephemeral keypairs whose public keys are 3a553d74792d727efa9b9a4cde3da1ad93f1a2d0c09cb639b1a3c0fda14cbe24 and 489cebba7a166c619835fe12eedc581dbfc1b36de10c830e4f0bc1c48734864b
input: client index 0, max_clients 4, protocol id 1122334455667788, filter value b7590100, sequence 1
input: an empty receive replay state, so the ack block is the minimum
04 00 b7 59 01 00 01 00 00 00 d2 48 d8 10 b0 69
78 86 27 a0 51 3c 6d bc ea 6d 28 2c 39 1b b9 28
8e fb 4c 5d ba 9c 70 72 4b
```

```vector t12.token-created-in-future
input: the request of [t8 Packet formats](#t8)
input: signed create timestamp 1756166406, expire 1756166700, server time 1756166400
input: the return cookie is valid for the source and connection salt
00 00 b7 59 01 00 03 f9 ac 68 00 00 00 00 0f b2
b2 8f 32 db d6 c9 eb 36 4f 57 74 86 f3 e9 53 41
4c 54 30 30 30 31 3a 55 3d 74 79 2d 72 7e fa 9b
9a 4c de 3d a1 ad 93 f1 a2 d0 c0 9c b6 39 b1 a3
c0 fd a1 4c be 24 93 1a 7c 4e 88 77 66 55 44 33
22 11 06 f9 ac 68 00 00 00 00 2c fa ac 68 00 00
00 00 0a 00 00 00 01 cb 00 71 0a 40 9c 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 3d
24 90 88 9d 37 4c 29 6f ab 1d 57 70 cb 71 47 6a
06 f7 4e f2 f2 54 0c f2 21 5f 76 14 f4 80 1c 34
e4 2d 4a f5 ef 94 a0 7a 3a 84 20 1b 88 9d 4c d1
a7 43 cb 27 b1 1b 6a 10 43 8a 8f eb 8e 58 47 d5
da 88 97 83 41 4a 57 bb 78 b0 11 46 db 8f 9a 3f
0c bd b6 0f 45 ee cb e3 8e a0 a7 2c ec 1b 6b 42
e3 4a fd 6e fb 43 6d 00 34 5c 40 4c 4d e3 92 57
86 d2 59 cc 06 b1 23 9e 25 9c a8 eb 13 7f 93 49
19 1a 21 1c a7 f0 17 04 ae 96 dd d5 2c 86 e1 69
34 78 9b 7c 3a 07 74 d0 88 e0 35 28 a4 af a2 da
c5 aa 04 d2 41 80 59 30 ca 24 ff 62 b9 41 7c 1e
b7 f3 62 48 71 75 57 ad 34 0a 1a 21 11 eb 1f 6b
5b 71 ab ab 8f 60 44 45 0e 18 00 58 98 b6 88 6f
8b aa 2d e5 bf 46 1e e4 f9 b3 26 38 73 2e 4f 4d
9e a8 37 61 de a2 a1 73 c2 e7 4f 13 83 bb 45 09
26 b3 cd 38 89 ef 3b 4d d3 d8 4f b8 5a ad 0a 12
e5 a7 7b e7 01 40 2d 52 07 05 78 41 e3 58 89 d9
9f 82 43 09 bf 6b 60 e2 40 3c 77 16 53 be ac 7b
23 68 00 70 fa f4 a9 19 5e 31 69 fb 4d d8 36 91
b7 7e 19 bf a6 51 03 42 21 6a 03 49 39 1e 2e 9c
c7 c9 68 b0 16 63 7d 9f 8a e4 9c 24 52 8a 35 75
42 ae c0 1e f8 32 4a 94 6a f6 ac d6 15 6b 9c 17
7f 06 cf 3b 82 9b fc e6 0e c0 09 d5 79 2d 16 2e
6d 9b 58 85 2d 11 b4 df 6c 62 3a 1d 39 3a e3 15
c2 46 ed 69 60 57 f4 f8 d7 b9 18 af 57 43 1a 7e
c5 0c dd 40 aa 72 bf ed 55 c0 cf 4a a8 c9 18 8b
d9 4c 05 63 fe 96 fe cc 32 27 7a bf cc 8b fb 18
d6 e3 db be b2 b5 84 65 f3 57 11 e6 2e 91 41 1c
4d 80 76 ed 82 74 32 f3 3d 0c f7 3b a8 81 af 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 11 9e b3 02 5c ab 63 a6 65 63
5d 30 d7 73 5c 6f 88 9e e2 1c eb 7d 8c 7e 1e 9f
53 f2 26 e8 5f b4 92 ed 6b d3 43 f8 51 92 b3 91
d2 fe ff 81 9e ea 88 f4 02 6e ba 4b cf a8 f0 1c
b7 a1 ab c4 74 09
```

```vector t12.token-lifetime-too-long
input: the request of [t8 Packet formats](#t8)
input: signed create timestamp 1756166300, expire 1756167100, server maximum token lifetime 600
input: the return cookie is valid for the source and connection salt
00 00 b7 59 01 00 03 f9 ac 68 00 00 00 00 0f b2
b2 8f 32 db d6 c9 eb 36 4f 57 74 86 f3 e9 53 41
4c 54 30 30 30 31 3a 55 3d 74 79 2d 72 7e fa 9b
9a 4c de 3d a1 ad 93 f1 a2 d0 c0 9c b6 39 b1 a3
c0 fd a1 4c be 24 93 1a 7c 4e 88 77 66 55 44 33
22 11 9c f8 ac 68 00 00 00 00 bc fb ac 68 00 00
00 00 0a 00 00 00 01 cb 00 71 0a 40 9c 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 3d
24 90 88 9d 37 4c 29 6f ab 1d 57 70 cb 71 47 6a
06 f7 4e f2 f2 54 0c f2 21 5f 76 14 f4 80 1c 34
e4 2d 4a f5 ef 94 a0 7a 3a 84 20 1b 88 9d 4c d1
a7 43 cb 27 b1 1b 6a 10 43 8a 8f eb 8e 58 47 cc
fb b6 a7 54 90 8b 0c e8 c7 d8 45 f4 91 33 be 3f
0c bd b6 0f 45 ee cb e3 8e a0 a7 2c ec 1b 6b d2
e2 4a fd 6e fb 43 6d 00 34 5c 40 4c 4d e3 92 57
86 d2 59 cc 06 b1 23 9e 25 9c a8 eb 13 7f 93 49
19 1a 21 1c a7 f0 17 04 ae 96 dd d5 2c 86 e1 69
34 78 9b 7c 3a 07 74 d0 88 e0 35 28 a4 af a2 da
c5 aa 04 d2 41 80 59 30 ca 24 ff 62 b9 41 7c 1e
b7 f3 62 48 71 75 57 ad 34 0a 1a 21 11 eb 1f 6b
5b 71 ab ab 8f 60 44 45 0e 18 00 58 98 b6 88 6f
8b aa 2d e5 bf 46 1e e4 f9 b3 26 38 73 2e 4f 4d
9e a8 37 61 de a2 a1 73 c2 e7 4f 13 83 bb 45 09
26 b3 cd 38 89 ef 3b 4d d3 d8 4f b8 5a ad 0a 12
e5 a7 7b e7 01 40 2d 52 07 05 78 41 e3 58 89 d9
9f 82 43 09 bf 6b 60 e2 40 3c 77 16 53 be ac 7b
23 68 00 70 fa f4 a9 19 5e 31 69 fb 4d d8 36 91
b7 7e 19 bf a6 51 03 42 21 6a 03 49 39 1e 2e 9c
c7 c9 68 b0 16 63 7d 9f 8a e4 9c 24 52 8a 35 75
42 ae c0 1e f8 32 4a 94 6a f6 ac d6 15 6b 9c 17
7f 06 cf 3b 82 9b fc e6 0e c0 09 d5 79 2d 16 2e
6d 9b 58 85 2d 11 b4 df 6c 62 3a 1d 39 3a e3 15
c2 46 ed 69 60 57 f4 f8 d7 b9 18 af 57 43 1a 7e
c5 0c dd 40 aa 72 bf ed 55 c0 cf 4a a8 c9 18 8b
d9 4c 05 63 fe 96 fe cc 32 27 7a bf cc 8b fb 18
d6 e3 db be b2 b5 84 65 f3 57 11 e6 2e 91 41 1c
4d 80 76 ed 82 74 32 f3 3d 0c f7 3b a8 81 af 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 6e a4 ba 25 9d 89 d7 0d 3c bd
35 1f 4d bf 00 83 89 dd 5d e9 0f 7a 6e e4 01 a6
3c 32 20 d0 3e d6 22 ec 66 a6 3e df a2 93 4a ef
1d b0 9d 03 06 5e 5c f5 5a 57 f8 4e a7 ba 39 b1
55 04 96 f2 aa 05
```

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
retransmission with no backoff; deadlines), naming the rows of
[t3 Constants and registries](#t3) and
[t2.3 Configuration surface](#t2.3) without restating them.

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
| [t12 Handshake: server](#t12) | `t12.challenge` |
| [t12 Handshake: server](#t12) | `t12.cookie` |
| [t12 Handshake: server](#t12) | `t12.keep-alive` |
| [t12 Handshake: server](#t12) | `t12.stale-cookie` |
| [t12 Handshake: server](#t12) | `t12.token-created-in-future` |
| [t12 Handshake: server](#t12) | `t12.token-lifetime-too-long` |
<!-- vector-index:end -->

<a id="tb"></a>
## tb. Appendix B: changes from netcode 1.02

Status: pending

Scope: a list of facts, one per difference on the wire or in processing.

<a id="tc"></a>
## tc. Appendix C (informative): implementation notes

Status: pending

Scope: informative notes for implementers; no requirements.
