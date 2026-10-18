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

Every packet begins with the 6-byte prefilter header of [t5 Prefilter header](#t5): the packet type at offset 0, the filter epoch at offset 1, and the filter value at offsets 2 to 5. A sender MUST set the packet type byte to one of the ten types below. A receiver MUST drop a packet whose type is 10 or above.

A sender MUST emit a packet of the size its type and fields give, and MUST NOT emit a packet larger than the send budget. A receiver MUST drop a packet whose size is below its type's minimum or above the receive ceiling before it examines any field inside the packet ([t10 Receive processing](#t10)).

Types 0 (connection request) and 7 (connection cookie) are not sealed. Types 1 to 6, 8 and 9 are sealed: after the prefilter header and the 4-byte sequence, their bytes are the sealed data of [t9 Packet protection](#t9). Types 1 and 2 place the 8-byte session nonce in the clear between the sequence and the sealed data. The session types 4, 5, 6, 8 and 9 open their sealed plaintext with the ack block of [t15 Acknowledgement](#t15), and the type data follows it.

Every integer is little-endian, every structure is packed with no padding, and every size is in bytes ([t1.4 Conventions](#t1.4)). The offsets below count from the first byte of the packet, the packet type.

<a id="t8.1"></a>
### t8.1. Connection request packet (type 0)

The connection request is not sealed. It carries the client's per-attempt salt and ephemeral key, and the signed connect data and signature taken verbatim from the connect token ([t6 Connect token](#t6)).

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|  packet type  | filter epoch  |        filter value           |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                    return cookie (bytes[24])                  |
|                              ...                              |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|              connection salt (bytes[8])                       |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|         client ephemeral public key (bytes[32])               |
|                              ...                              |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|            signed connect data (bytes[576])                   |
|                              ...                              |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                 signature (bytes[64])                         |
|                              ...                              |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

| Name | Offset | Size | Type | Meaning | Valid values |
|---|---|---|---|---|---|
| packet type | 0 | 1 | u8 | selects this layout | `0` |
| filter epoch | 1 | 1 | u8 | the epoch of the filter value | `0` |
| filter value | 2 | 4 | bytes[4] | the value of [t5 Prefilter header](#t5) | any |
| return cookie | 6 | 24 | bytes[24] | the cookie expire timestamp followed by the cookie MAC, echoed from a type 7 packet | all zero on the first request of an attempt |
| connection salt | 30 | 8 | bytes[8] | the client's salt for this attempt | any |
| client ephemeral public key | 38 | 32 | bytes[32] | the client's X25519 ephemeral public key | any |
| signed connect data | 70 | 576 | bytes[576] | bytes 0 to 575 of the connect token | any |
| signature | 646 | 64 | bytes[64] | the token's Ed25519 signature | any |

<a id="t8.2"></a>
### t8.2. Connection denied packet (type 1)

The connection denied packet is sealed and carries no type data.

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|  packet type  | filter epoch  |        filter value           |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                       sequence (u32)                          |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                   session nonce (bytes[8])                    |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                     AEAD tag (bytes[16])                      |
|                              ...                              |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

| Name | Offset | Size | Type | Meaning | Valid values |
|---|---|---|---|---|---|
| packet type | 0 | 1 | u8 | selects this layout | `1` |
| filter epoch | 1 | 1 | u8 | the epoch of the filter value | `0` |
| filter value | 2 | 4 | bytes[4] | the value of [t5 Prefilter header](#t5) | any |
| sequence | 6 | 4 | u32 | the sender's handshake sequence | nonzero |
| session nonce | 10 | 8 | bytes[8] | the server's session nonce for this mapping, in the clear | any |
| AEAD tag | 18 | 16 | bytes[16] | the tag over an empty plaintext | any |

<a id="t8.3"></a>
### t8.3. Connection challenge packet (type 2)

The connection challenge is sealed. Its cleartext session nonce lets a client that has not yet derived handshake keys open it.

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|  packet type  | filter epoch  |        filter value           |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                       sequence (u32)                          |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                   session nonce (bytes[8])                    |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|          server ephemeral public key (bytes[32])              |
|                              ...                              |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                     AEAD tag (bytes[16])                      |
|                              ...                              |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

| Name | Offset | Size | Type | Meaning | Valid values |
|---|---|---|---|---|---|
| packet type | 0 | 1 | u8 | selects this layout | `2` |
| filter epoch | 1 | 1 | u8 | the epoch of the filter value | `0` |
| filter value | 2 | 4 | bytes[4] | the value of [t5 Prefilter header](#t5) | any |
| sequence | 6 | 4 | u32 | the sender's handshake sequence | nonzero |
| session nonce | 10 | 8 | bytes[8] | the server's session nonce for this mapping, in the clear | any |
| server ephemeral public key | 18 | 32 | bytes[32] | the server's X25519 ephemeral public key, sealed | any |
| AEAD tag | 50 | 16 | bytes[16] | the tag over the server ephemeral public key | any |

<a id="t8.4"></a>
### t8.4. Connection response packet (type 3)

The connection response is sealed and carries 24 zero bytes of padding.

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|  packet type  | filter epoch  |        filter value           |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                       sequence (u32)                          |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                     padding (zero[24])                        |
|                              ...                              |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                     AEAD tag (bytes[16])                      |
|                              ...                              |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

| Name | Offset | Size | Type | Meaning | Valid values |
|---|---|---|---|---|---|
| packet type | 0 | 1 | u8 | selects this layout | `3` |
| filter epoch | 1 | 1 | u8 | the epoch of the filter value | `0` |
| filter value | 2 | 4 | bytes[4] | the value of [t5 Prefilter header](#t5) | any |
| sequence | 6 | 4 | u32 | the sender's handshake sequence | nonzero |
| padding | 10 | 24 | zero[24] | zero bytes, sealed | all zero |
| AEAD tag | 34 | 16 | bytes[16] | the tag over the padding | any |

<a id="t8.5"></a>
### t8.5. Connection keep-alive packet (type 4)

The keep-alive is a session packet. Its sealed plaintext is the ack block followed by the keep-alive fields.

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|  packet type  | filter epoch  |        filter value           |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                       sequence (u32)                          |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|               ack block (bytes[7] to bytes[1027])            |
|                              ...                              |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                      client index (u32)                       |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                      max clients (u32)                        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                     AEAD tag (bytes[16])                      |
|                              ...                              |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

| Name | Offset | Size | Type | Meaning | Valid values |
|---|---|---|---|---|---|
| packet type | 0 | 1 | u8 | selects this layout | `4` |
| filter epoch | 1 | 1 | u8 | the epoch of the filter value | `0` |
| filter value | 2 | 4 | bytes[4] | the value of [t5 Prefilter header](#t5) | any |
| sequence | 6 | 4 | u32 | the sender's session sequence | nonzero |
| ack block | 10 | 7 to 1027 | bytes[n] | the ack block of [t15 Acknowledgement](#t15), sealed | a well-formed block |
| client index | 10 plus the ack block size | 4 | u32 | the slot that holds this connection | below max clients |
| max clients | 14 plus the ack block size | 4 | u32 | the server's slot count | any |
| AEAD tag | 18 plus the ack block size | 16 | bytes[16] | the tag over the ack block and the keep-alive fields | any |

<a id="t8.6"></a>
### t8.6. Connection payload packet (type 5)

The payload packet is a session packet. Its sealed plaintext is the ack block followed by one payload.

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|  packet type  | filter epoch  |        filter value           |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                       sequence (u32)                          |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|               ack block (bytes[7] to bytes[1027])            |
|                              ...                              |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                      payload data (bytes[n])                  |
|                              ...                              |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                     AEAD tag (bytes[16])                      |
|                              ...                              |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

| Name | Offset | Size | Type | Meaning | Valid values |
|---|---|---|---|---|---|
| packet type | 0 | 1 | u8 | selects this layout | `5` |
| filter epoch | 1 | 1 | u8 | the epoch of the filter value | `0` |
| filter value | 2 | 4 | bytes[4] | the value of [t5 Prefilter header](#t5) | any |
| sequence | 6 | 4 | u32 | the sender's session sequence | nonzero |
| ack block | 10 | 7 to 1027 | bytes[n] | the ack block of [t15 Acknowledgement](#t15), sealed | a well-formed block |
| payload data | 10 plus the ack block size | 1 to the send budget less 26 and the ack block size | bytes[n] | one payload of the message layer | at least 1 byte |
| AEAD tag | 16 plus the ack block size and the payload size | 16 | bytes[16] | the tag over the ack block and the payload | any |

<a id="t8.7"></a>
### t8.7. Connection disconnect packet (type 6)

The disconnect packet is a session packet with no type data.

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|  packet type  | filter epoch  |        filter value           |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                       sequence (u32)                          |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|               ack block (bytes[7] to bytes[1027])            |
|                              ...                              |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                     AEAD tag (bytes[16])                      |
|                              ...                              |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

| Name | Offset | Size | Type | Meaning | Valid values |
|---|---|---|---|---|---|
| packet type | 0 | 1 | u8 | selects this layout | `6` |
| filter epoch | 1 | 1 | u8 | the epoch of the filter value | `0` |
| filter value | 2 | 4 | bytes[4] | the value of [t5 Prefilter header](#t5) | any |
| sequence | 6 | 4 | u32 | the sender's session sequence | nonzero |
| ack block | 10 | 7 to 1027 | bytes[n] | the ack block of [t15 Acknowledgement](#t15), sealed | a well-formed block |
| AEAD tag | 10 plus the ack block size | 16 | bytes[16] | the tag over the ack block | any |

<a id="t8.8"></a>
### t8.8. Connection cookie packet (type 7)

The connection cookie is not sealed.

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|  packet type  | filter epoch  |        filter value           |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|              connection salt (bytes[8])                       |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                  cookie expire timestamp (u64)                |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                    cookie MAC (bytes[16])                     |
|                              ...                              |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

| Name | Offset | Size | Type | Meaning | Valid values |
|---|---|---|---|---|---|
| packet type | 0 | 1 | u8 | selects this layout | `7` |
| filter epoch | 1 | 1 | u8 | the epoch of the filter value | `0` |
| filter value | 2 | 4 | bytes[4] | the value of [t5 Prefilter header](#t5) | any |
| connection salt | 6 | 8 | bytes[8] | the salt echoed from the request | any |
| cookie expire timestamp | 14 | 8 | u64 | the time at which the cookie expires | any |
| cookie MAC | 22 | 16 | bytes[16] | the MAC over the client address, the salt and the expire timestamp | any |

<a id="t8.9"></a>
### t8.9. Connection ack packet (type 8)

The standalone ack packet is a session packet whose ack block is its whole type data.

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|  packet type  | filter epoch  |        filter value           |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                       sequence (u32)                          |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|               ack block (bytes[7] to bytes[1027])            |
|                              ...                              |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                     AEAD tag (bytes[16])                      |
|                              ...                              |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

| Name | Offset | Size | Type | Meaning | Valid values |
|---|---|---|---|---|---|
| packet type | 0 | 1 | u8 | selects this layout | `8` |
| filter epoch | 1 | 1 | u8 | the epoch of the filter value | `0` |
| filter value | 2 | 4 | bytes[4] | the value of [t5 Prefilter header](#t5) | any |
| sequence | 6 | 4 | u32 | the sender's session sequence | nonzero |
| ack block | 10 | 7 to 1027 | bytes[n] | the ack block of [t15 Acknowledgement](#t15), sealed | a well-formed block |
| AEAD tag | 10 plus the ack block size | 16 | bytes[16] | the tag over the ack block | any |

<a id="t8.10"></a>
### t8.10. Connection payload fragment packet (type 9)

The fragment packet is a session packet. Its sealed plaintext is the ack block, the fragment header, and the fragment data ([t16 Fragmentation and reassembly](#t16)).

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|  packet type  | filter epoch  |        filter value           |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                       sequence (u32)                          |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|               ack block (bytes[7] to bytes[1027])            |
|                              ...                              |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                      payload id (u32)                         |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
| fragment index|fragment count |      fragment data ...        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                    fragment data (bytes[n])                   |
|                              ...                              |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                     AEAD tag (bytes[16])                      |
|                              ...                              |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

| Name | Offset | Size | Type | Meaning | Valid values |
|---|---|---|---|---|---|
| packet type | 0 | 1 | u8 | selects this layout | `9` |
| filter epoch | 1 | 1 | u8 | the epoch of the filter value | `0` |
| filter value | 2 | 4 | bytes[4] | the value of [t5 Prefilter header](#t5) | any |
| sequence | 6 | 4 | u32 | the sender's session sequence | nonzero |
| ack block | 10 | 7 to 1027 | bytes[n] | the ack block of [t15 Acknowledgement](#t15), sealed | a well-formed block |
| payload id | 10 plus the ack block size | 4 | u32 | the fragmented payload's identifier | nonzero |
| fragment index | 14 plus the ack block size | 1 | u8 | this fragment's index | below the fragment count |
| fragment count minus one | 15 plus the ack block size | 1 | u8 | the fragment count less one | 1 to 255 |
| fragment data | 16 plus the ack block size | 1 to the send budget less 32 and the ack block size | bytes[n] | this fragment's bytes | at least 1 byte |
| AEAD tag | 16 plus the ack block size and the fragment size | 16 | bytes[16] | the tag over the ack block, the header and the fragment data | any |

<a id="t8.11"></a>
### t8.11. Packet sizes

The wire size of each type is the sum of its fields. The handshake types have a fixed size; the session types have a minimum and a receive maximum, because the ack block and the type data vary.

| Name | Value | Unit | Composition |
|---|---|---|---|
| connection request wire size | `710` | bytes | 6 + 704 |
| connection denied wire size | `34` | bytes | 6 + 4 + 8 + 16 |
| connection challenge wire size | `66` | bytes | 6 + 4 + 8 + 32 + 16 |
| connection response wire size | `50` | bytes | 6 + 4 + 24 + 16 |
| connection keep-alive minimum wire size | `41` | bytes | 6 + 4 + 7 + 8 + 16 |
| connection payload minimum wire size | `34` | bytes | 6 + 4 + 7 + 1 + 16 |
| connection disconnect minimum wire size | `33` | bytes | 6 + 4 + 7 + 16 |
| connection cookie wire size | `38` | bytes | 6 + 8 + 8 + 16 |
| connection ack minimum wire size | `33` | bytes | 6 + 4 + 7 + 16 |
| connection payload fragment minimum wire size | `40` | bytes | 6 + 4 + 7 + 6 + 1 + 16 |

A session packet MUST NOT exceed the receive ceiling, and a sender MUST NOT emit one larger than its send budget. The unfragmented payload capacity, the receive payload maximum and the fragment stride are rows of [t3 Constants and registries](#t3).

<a id="t8.12"></a>
### t8.12. Vectors

Each vector below is one assembled packet, from its packet type byte to its last tag byte.

```vector t8.request
input: packet type 0, filter value b7 59 01 00
input: return cookie 24 zero bytes
input: connection salt bytes 30 31 32 33 34 35 36 37
input: client ephemeral public key bytes 20 to 3f
input: signed connect data byte i is i, for i 0 to 575
input: signature byte i is ff minus i, for i 0 to 63
00 00 b7 59 01 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 30 31
32 33 34 35 36 37 20 21 22 23 24 25 26 27 28 29
2a 2b 2c 2d 2e 2f 30 31 32 33 34 35 36 37 38 39
3a 3b 3c 3d 3e 3f 00 01 02 03 04 05 06 07 08 09
0a 0b 0c 0d 0e 0f 10 11 12 13 14 15 16 17 18 19
1a 1b 1c 1d 1e 1f 20 21 22 23 24 25 26 27 28 29
2a 2b 2c 2d 2e 2f 30 31 32 33 34 35 36 37 38 39
3a 3b 3c 3d 3e 3f 40 41 42 43 44 45 46 47 48 49
4a 4b 4c 4d 4e 4f 50 51 52 53 54 55 56 57 58 59
5a 5b 5c 5d 5e 5f 60 61 62 63 64 65 66 67 68 69
6a 6b 6c 6d 6e 6f 70 71 72 73 74 75 76 77 78 79
7a 7b 7c 7d 7e 7f 80 81 82 83 84 85 86 87 88 89
8a 8b 8c 8d 8e 8f 90 91 92 93 94 95 96 97 98 99
9a 9b 9c 9d 9e 9f a0 a1 a2 a3 a4 a5 a6 a7 a8 a9
aa ab ac ad ae af b0 b1 b2 b3 b4 b5 b6 b7 b8 b9
ba bb bc bd be bf c0 c1 c2 c3 c4 c5 c6 c7 c8 c9
ca cb cc cd ce cf d0 d1 d2 d3 d4 d5 d6 d7 d8 d9
da db dc dd de df e0 e1 e2 e3 e4 e5 e6 e7 e8 e9
ea eb ec ed ee ef f0 f1 f2 f3 f4 f5 f6 f7 f8 f9
fa fb fc fd fe ff 00 01 02 03 04 05 06 07 08 09
0a 0b 0c 0d 0e 0f 10 11 12 13 14 15 16 17 18 19
1a 1b 1c 1d 1e 1f 20 21 22 23 24 25 26 27 28 29
2a 2b 2c 2d 2e 2f 30 31 32 33 34 35 36 37 38 39
3a 3b 3c 3d 3e 3f 40 41 42 43 44 45 46 47 48 49
4a 4b 4c 4d 4e 4f 50 51 52 53 54 55 56 57 58 59
5a 5b 5c 5d 5e 5f 60 61 62 63 64 65 66 67 68 69
6a 6b 6c 6d 6e 6f 70 71 72 73 74 75 76 77 78 79
7a 7b 7c 7d 7e 7f 80 81 82 83 84 85 86 87 88 89
8a 8b 8c 8d 8e 8f 90 91 92 93 94 95 96 97 98 99
9a 9b 9c 9d 9e 9f a0 a1 a2 a3 a4 a5 a6 a7 a8 a9
aa ab ac ad ae af b0 b1 b2 b3 b4 b5 b6 b7 b8 b9
ba bb bc bd be bf c0 c1 c2 c3 c4 c5 c6 c7 c8 c9
ca cb cc cd ce cf d0 d1 d2 d3 d4 d5 d6 d7 d8 d9
da db dc dd de df e0 e1 e2 e3 e4 e5 e6 e7 e8 e9
ea eb ec ed ee ef f0 f1 f2 f3 f4 f5 f6 f7 f8 f9
fa fb fc fd fe ff 00 01 02 03 04 05 06 07 08 09
0a 0b 0c 0d 0e 0f 10 11 12 13 14 15 16 17 18 19
1a 1b 1c 1d 1e 1f 20 21 22 23 24 25 26 27 28 29
2a 2b 2c 2d 2e 2f 30 31 32 33 34 35 36 37 38 39
3a 3b 3c 3d 3e 3f ff fe fd fc fb fa f9 f8 f7 f6
f5 f4 f3 f2 f1 f0 ef ee ed ec eb ea e9 e8 e7 e6
e5 e4 e3 e2 e1 e0 df de dd dc db da d9 d8 d7 d6
d5 d4 d3 d2 d1 d0 cf ce cd cc cb ca c9 c8 c7 c6
c5 c4 c3 c2 c1 c0
```

```vector t8.denied
input: packet type 1, filter value b7 59 01 00
input: sequence 1
input: session nonce bytes 40 to 47
input: handshake server-to-client key bytes 80 to 9f
input: sealed data empty
01 00 b7 59 01 00 01 00 00 00 40 41 42 43 44 45
46 47 9c c7 b0 41 7c 4d 93 52 f9 a1 98 ef 49 ad
6f cb
```

```vector t8.challenge
input: packet type 2, filter value b7 59 01 00
input: sequence 1
input: session nonce bytes 40 to 47
input: server ephemeral public key bytes 40 to 5f
input: client ephemeral public key bytes 20 to 3f
input: handshake server-to-client key bytes 80 to 9f
02 00 b7 59 01 00 01 00 00 00 40 41 42 43 44 45
46 47 a3 50 36 4e 3a 3a 61 a1 2a 4c 90 5d c6 39
c8 a5 49 1e 7e 8d 4b 7c 58 51 98 cd 4b 94 4e e9
d1 90 f4 2a d8 21 0f ab 19 e4 1a 3b 96 dc 06 08
3d 31
```

```vector t8.response
input: packet type 3, filter value b7 59 01 00
input: sequence 1
input: padding 24 zero bytes
input: client ephemeral public key bytes 20 to 3f
input: handshake client-to-server key bytes 60 to 7f
03 00 b7 59 01 00 01 00 00 00 f6 0f fd 9b d3 d8
bd e8 c3 8b e5 af 01 f3 eb a1 47 55 ea 1a 6a ff
0c 04 71 79 8c f7 4a 64 66 7d bc d6 ea 5f 4c a5
b9 97
```

```vector t8.keep-alive
input: packet type 4, filter value b7 59 01 00
input: sequence 1
input: ack block 00 00 00 00 00 00 00
input: client index 3, max clients 1000
input: session server-to-client key bytes c0 to df
04 00 b7 59 01 00 01 00 00 00 93 f3 ae 3f 05 4a
38 d0 41 ec 2f 98 38 7a ff bf 5e 31 4c df 50 0e
03 e1 28 04 ec 28 00 1b 99
```

```vector t8.payload
input: packet type 5, filter value b7 59 01 00
input: sequence 3
input: ack block 00 00 00 00 00 00 00
input: payload bytes 68 65 6c 6c 6f
input: session client-to-server key bytes a0 to bf
05 00 b7 59 01 00 03 00 00 00 d2 ca 22 7d 0d e1
9a dc 82 7d ea d4 4a 38 fd 84 26 82 9a 5d b6 7b
80 e5 85 fe 54 0c
```

```vector t8.disconnect
input: packet type 6, filter value b7 59 01 00
input: sequence 4
input: ack block 00 00 00 00 00 00 00
input: no type data
input: session client-to-server key bytes a0 to bf
06 00 b7 59 01 00 04 00 00 00 17 e7 37 e8 c4 f6
f2 25 6f b7 76 70 25 43 e9 59 d3 09 27 85 70 ed
91
```

```vector t8.cookie
input: packet type 7, filter value b7 59 01 00
input: connection salt bytes 30 31 32 33 34 35 36 37
input: cookie expire 0x0102030405060708, little-endian on the wire
input: cookie mac bytes d0 to df
07 00 b7 59 01 00 30 31 32 33 34 35 36 37 08 07
06 05 04 03 02 01 d0 d1 d2 d3 d4 d5 d6 d7 d8 d9
da db dc dd de df
```

```vector t8.ack
input: packet type 8, filter value b7 59 01 00
input: sequence 5
input: ack block 00 00 00 00 00 00 00
input: no type data
input: session client-to-server key bytes a0 to bf
08 00 b7 59 01 00 05 00 00 00 18 11 d7 e6 43 54
2c 6e a6 b4 9e e8 de 56 b3 3d ba 0d 4f 61 7e 85
b5
```

```vector t8.fragment
input: packet type 9, filter value b7 59 01 00
input: sequence 6
input: ack block 00 00 00 00 00 00 00
input: payload id 1, fragment index 0, fragment count 2
input: fragment data bytes 68 65 6c 6c 6f
input: session client-to-server key bytes a0 to bf
09 00 b7 59 01 00 06 00 00 00 c2 9e 52 06 da d5
b3 e8 ab a8 f1 4c cb 35 88 29 3a 74 2f 39 e4 00
ad c5 bd df c7 8f aa 2a 94 fb 2b 78
```

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
| [t8 Packet formats](#t8) | `t8.ack` |
| [t8 Packet formats](#t8) | `t8.challenge` |
| [t8 Packet formats](#t8) | `t8.cookie` |
| [t8 Packet formats](#t8) | `t8.denied` |
| [t8 Packet formats](#t8) | `t8.disconnect` |
| [t8 Packet formats](#t8) | `t8.fragment` |
| [t8 Packet formats](#t8) | `t8.keep-alive` |
| [t8 Packet formats](#t8) | `t8.payload` |
| [t8 Packet formats](#t8) | `t8.request` |
| [t8 Packet formats](#t8) | `t8.response` |
<!-- vector-index:end -->

<a id="tb"></a>
## tb. Appendix B: changes from netcode 1.02

Status: pending

Scope: a list of facts, one per difference on the wire or in processing.

<a id="tc"></a>
## tc. Appendix C (informative): implementation notes

Status: pending

Scope: informative notes for implementers; no requirements.
