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

1. A sender MUST write the packet sequence in the `sequence` field of
   [t3 Constants and registries](#t3) as the low half of the 64-bit sequence,
   least-significant byte first. A sender MUST NOT emit a packet whose
   sequence is zero.
2. A receiver MUST reconstruct the 64-bit sequence from that field before any
   replay check or AEAD open. Let `expected` be the highest sequence the
   receiver has successfully opened in that direction and key phase, plus
   one; when nothing has been opened, `expected` is one. Among all 64-bit
   values whose low half equals the wire field, the receiver MUST take the
   value nearest to `expected`, and on an exact tie it MUST take the larger.
3. A receiver MUST ignore a packet whose reconstructed sequence is zero.
   Zero is never a valid sequence and is never sent.
4. An address on the wire is one family byte, the address bytes and the port,
   packed with no padding. A sender MUST write family `1` with the four IPv4
   octets in order, or family `2` with the eight IPv6 groups as sixteen-bit
   values, each group little-endian, and MUST NOT write any other family. The
   port is a sixteen-bit value, little-endian.

The sequence field of a sealed packet:

       0                   1                   2                   3
       0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
      |                    sequence, low half (u32)                   |
      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

| Name | Offset | Size | Type | Meaning | Valid values |
|---|---|---|---|---|---|
| sequence | 0 | 4 | u32 | low half of the packet sequence | any; a reconstructed value of zero is ignored |

The address encoding:

       0                   1                   2                   3
       0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
      |    family     |                                               |
      +-+-+-+-+-+-+-+-+                                               +
      |                    address (4 or 16 bytes)                    |
      +                               +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
      |                               |             port              |
      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

| Name | Offset | Size | Type | Meaning | Valid values |
|---|---|---|---|---|---|
| family | 0 | 1 | u8 | address family | `1` (IPv4), `2` (IPv6) |
| address | 1 | 4 or 16 | bytes[4] or bytes[16] | four IPv4 octets in order, or eight IPv6 groups little-endian | 4 bytes for family `1`, 16 for family `2` |
| port | 5 or 17 | 2 | u16 | UDP port | any |

| Name | Value | Unit | Meaning |
|---|---|---|---|
| address family IPv4 | `1` | number | family byte of an IPv4 address |
| address family IPv6 | `2` | number | family byte of an IPv6 address |
| encoded IPv4 address | `7` | bytes | family byte, four address octets, port |
| encoded IPv6 address | `19` | bytes | family byte, eight address groups, port |

The sequence is reconstructed at the start of a key phase:

```vector t4.sequence-start
input: highest opened sequence 0, wire field 1
01 00 00 00 00 00 00 00
```

In steady state:

```vector t4.sequence-steady
input: highest opened sequence 999, wire field 1000
e8 03 00 00 00 00 00 00
```

At an exact tie of half the field's range in each direction, the larger wins:

```vector t4.sequence-tie
input: highest opened sequence 0x17fffffff, wire field 0
00 00 00 00 02 00 00 00
```

Across the field's wrap:

```vector t4.sequence-wrap
input: highest opened sequence 0xffffffff, wire field 0
00 00 00 00 01 00 00 00
```

A wire field of zero with nothing opened reconstructs to zero and is ignored:

```vector t4.sequence-zero
input: highest opened sequence 0, wire field 0
00 00 00 00 00 00 00 00
```

An IPv4 address, 198.51.100.7 port 56789:

```vector t4.address-ipv4
input: address 198.51.100.7, port 56789
01 c6 33 64 07 d5 dd
```

An IPv6 address, 2001:db8::7 port 56789:

```vector t4.address-ipv6
input: address 2001:db8::7, port 56789
02 01 20 b8 0d 00 00 00 00 00 00 00 00 00 00 07
00 d5 dd
```

<a id="t5"></a>
## t5. Prefilter header

Status: pending

Scope: the 6-byte header on every packet (type, epoch, filter value); the
unkeyed value at epoch 0; the keyed value at epochs 1 to 255 and the magic
generation wrap; structural checks; vectors.

<a id="t6"></a>
## t6. Connect token

A connect token is a fixed-size credential that a backend gives to a client. It names one server and carries the keys the client and that server use to establish a session. A server MUST accept a connection request only after it verifies the request's token as this section specifies.

1. A token MUST be exactly the `connect token` size of [t3 Constants and registries](#t3) long. Every byte of the token is part of the layout below; there is no variable-length field.
2. The version field MUST hold the `version` value of [t3 Constants and registries](#t3). The protocol performs no version negotiation and defines no mode by version. A server MUST ignore a connection request whose token carries any other version.
3. The backend MUST write the session keys twice: in the clear for the client, and inside the sealed block for the server. A client MUST NOT verify the signature; it trusts the clear copy because the secure channel delivered the token. A tampered clear copy causes the handshake to fail and nothing worse.
4. The token identifier MUST be the BLAKE2b hash, of the `token identifier` size of [t3 Constants and registries](#t3) and with no key, of the signed connect data. The identifier is content-determined, never the signature bytes.

### t6.1. Private connect token data

The private connect token data is the block that only the target server reads. Before sealing it is:

```
0                   1                   2                   3
0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                          client id                          |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                         protocol id                          |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                      expire timestamp                        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                   client to server key                       |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                   server to client key                       |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                        user data                             |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                         zero pad                             |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

| Name | Offset | Size | Type | Meaning | Valid values |
|---|---|---|---|---|---|
| client id | 0 | 8 | u64 | the identity the backend keys on | any |
| protocol id | 8 | 8 | u64 | the title this token belongs to | any; MUST equal the signed protocol id |
| expire timestamp | 16 | 8 | u64 | the time the token expires | any; MUST equal the signed expire timestamp |
| client to server key | 24 | 32 | bytes[32] | the session key from client to server | any |
| server to client key | 56 | 32 | bytes[32] | the session key from server to client | any |
| user data | 88 | 256 | bytes[256] | application-defined data bound to the session | any |
| zero pad | 344 | 40 | zero[40] | zero | zero |

A backend MUST seal this block with crypto_box_seal to the server's X25519 public key: draw an ephemeral X25519 key pair, write the ephemeral public key, then the ciphertext of crypto_box_easy under the nonce that is the BLAKE2b-192 hash of the ephemeral public key concatenated with the server public key. The sealed block is the `sealed block` size of [t3 Constants and registries](#t3): the private block plus the `sealed box overhead` size of [t3 Constants and registries](#t3). A server MUST open the sealed block with its own X25519 secret key.

### t6.2. Signed connect data

Everything the server checks is gathered into one fixed buffer of the `signed connect data` size of [t3 Constants and registries](#t3) and signed by the backend. The token carries this buffer and its signature verbatim, so the server verifies with no shared secret. The signature MUST be an Ed25519 detached signature over the whole buffer, including its zero padding.

| Name | Offset | Size | Type | Meaning | Valid values |
|---|---|---|---|---|---|
| version | 0 | 4 | u32 | the wire revision | the version constant |
| protocol id | 4 | 8 | u64 | the title; MUST equal the receiving server's configured protocol id | any |
| create timestamp | 12 | 8 | u64 | unix time the token was created | any |
| expire timestamp | 20 | 8 | u64 | unix time the token expires | any |
| timeout seconds | 28 | 4 | i32 | the connection timeout in seconds; a negative value disables the timeout (development only); zero is invalid | `-2147483648..2147483647` |
| address flags | 32 | 1 | u8 | bit 0 set means an IPv4 address is present, bit 1 an IPv6 address; bits 2 to 7 MUST be zero; at least one address MUST be present | `0x01`, `0x02`, `0x03` |
| ipv4 address | 33 | 6 | bytes[6] | four octets then the port little-endian; zero when bit 0 is clear | any |
| ipv6 address | 39 | 18 | bytes[18] | eight uint16 groups little-endian then the port little-endian; zero when bit 1 is clear | any |
| server public key | 57 | 32 | bytes[32] | the X25519 key the private block is sealed to | any |
| sealed private block | 89 | 432 | bytes[432] | the sealed block of [t6.1 Private connect token data](#t6.1) | any |
| zero pad | 521 | 55 | zero[55] | zero | zero |

| Name | Value | Unit | Meaning |
|---|---|---|---|
| address flag IPv4 | `0x01` | bit | bit 0: an IPv4 address is present |
| address flag IPv6 | `0x02` | bit | bit 1: an IPv6 address is present |
| address flags both | `0x03` | bit | both address families are present |

### t6.3. Connect token format

The full token is handed to the client over the secure channel. The client splits it into its parts without parsing any length:

```
0                   1                   2                   3
0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                signed connect data (576 bytes)               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                   signature (64 bytes)                       |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|             client to server key (32 bytes)                  |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|             server to client key (32 bytes)                  |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                       zero pad (64 bytes)                    |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

| Name | Offset | Size | Type | Meaning | Valid values |
|---|---|---|---|---|---|
| signed connect data | 0 | 576 | bytes[576] | the buffer of [t6.2 Signed connect data](#t6.2) | any |
| signature | 576 | 64 | bytes[64] | the detached signature over the signed connect data | any |
| client to server key | 640 | 32 | bytes[32] | the clear session key, for the client | any |
| server to client key | 672 | 32 | bytes[32] | the clear session key, for the client | any |
| zero pad | 704 | 64 | zero[64] | zero | zero |

### t6.4. Verification and refusal order

A server MUST check a token in this order and MUST stop at the first failure and refuse the request:

1. The version field holds the `version` value of [t3 Constants and registries](#t3); otherwise the request is ignored.
2. The address flags are valid: not zero, and no bit above bit 1 set.
3. The signed server public key equals the server's configured X25519 public key.
4. The signature verifies against the backend's Ed25519 public key.
5. The sealed block opens with the server's X25519 secret key.
6. The opened private protocol id and expire timestamp equal the signed ones.

Checks of the protocol id, the expiry, the timeout and the advertised address belong to [t12 Handshake: server](#t12) and do not change this order.

```vector t6.frozen-token
input: the minting inputs of the frozen token: IPv4 203.0.113.10, port 40000, empty user data
input: the signed connect data (576 bytes) then the signature, then the clear keys, then zero pad
93 1a 7c 4e 88 77 66 55 44 33 22 11 00 f9 ac 68
00 00 00 00 2c fa ac 68 00 00 00 00 0a 00 00 00
01 cb 00 71 0a 40 9c 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 3d 24 90 88 9d 37 4c
29 6f ab 1d 57 70 cb 71 47 6a 06 f7 4e f2 f2 54
0c f2 21 5f 76 14 f4 80 1c b3 10 fd d3 03 d6 aa
d8 c7 90 0d fc 42 52 d9 4d 21 34 f1 e6 e1 73 20
f4 c5 b1 bd 4f 28 79 da 3c e2 f2 87 72 08 61 88
73 78 ed f0 1d e9 be fc 09 f1 bf 01 fd 3d f1 9e
2a 79 e6 08 59 e8 96 34 6e b6 0c 08 f6 86 72 1d
81 94 6e 78 1e 1a e2 c3 71 6a 46 58 80 a3 a5 f0
68 ed e0 b0 39 cf 52 81 a9 7c d3 67 17 76 6b 5b
b1 38 69 83 8e cb 33 3d 9e eb 10 42 bf 7a 7b 8f
62 f9 cb b0 20 dd bd a6 cb 5f 44 e4 de f4 36 66
64 2f f8 e6 f8 35 a5 f2 95 dd 8d 7b f2 26 86 84
b2 a9 3e c1 7a c8 fd c4 c4 f0 5e 5b 09 92 7b 6a
fa c3 ec 0f 00 50 8e 9d 43 31 66 00 c6 b7 45 d6
db b9 c6 8f 12 d5 69 a1 95 73 d4 60 ba 34 e8 01
d7 44 d6 db 6a 0a 01 bd 35 b4 e0 8d 52 03 bb bd
44 36 7e f8 9b cb b3 82 7f d8 02 95 f2 b9 40 4c
dd 29 79 e3 aa 17 51 3f f9 f4 2b 67 06 24 8e a7
62 28 20 28 d4 fe 0c f7 11 56 55 4f 26 bf 90 cb
2b fc fe f4 38 b5 e6 8a d2 50 1f 9e 3b e2 4b c0
b3 36 51 ec 75 6f 60 f4 23 f3 24 d9 03 23 ad c9
f6 c8 f1 76 2b 02 ba 31 c7 e7 96 26 06 7f e7 f1
09 59 4a 80 3d 33 05 02 fe b0 3f 61 e9 29 7e c1
1a ad eb cd 69 ba c0 bf d0 5c 1c 5c 14 4a e2 3b
cd 11 4c 35 32 2e 79 78 43 e9 5c cb 8b c9 74 8f
da 08 19 7c e6 d5 a1 0c 83 bc b0 eb 7f f4 71 d7
38 a7 42 f6 8f a3 eb 77 df ad d3 20 a1 1a 55 42
0e 3c cd 03 f1 5e fc 18 3a 5d d3 5c f8 65 7f a7
12 b7 c9 74 96 00 0e 6a 54 68 d8 00 d4 9d 13 45
ce 18 5d c0 f7 bc 71 47 a9 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
6b 36 92 b4 9d 36 b7 a2 76 30 33 40 ab 83 99 b9
85 61 ac 68 81 fc 7a a2 bb 60 ff e0 77 32 ea 71
21 fe 50 44 eb b5 f5 3d be 76 8b 80 0c 30 d7 53
70 eb 9b eb 49 14 ae 13 3d a6 4e a6 b0 9e 76 0d
01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f 10
11 12 13 14 15 16 17 18 19 1a 1b 1c 1d 1e 1f 20
a1 a2 a3 a4 a5 a6 a7 a8 a9 aa ab ac ad ae af b0
b1 b2 b3 b4 b5 b6 b7 b8 b9 ba bb bc bd be bf c0
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
```

```vector t6.token-identifier
input: the signed connect data, bytes 0 to 575 of the frozen connect token
input: BLAKE2b, output 16 bytes, no key
f5 20 da 9d 95 fd 07 22 56 b4 7d 75 5f 81 0f 61
```

```vector t6.bad-address-flags
input: IPv4 127.0.0.1, port 40000, empty user data, address flags 0x05 (a bit above bit 1 set)
input: a token the server MUST refuse at the address-flags check, before any signature work
93 1a 7c 4e 88 77 66 55 44 33 22 11 00 f9 ac 68
00 00 00 00 2c fa ac 68 00 00 00 00 0a 00 00 00
05 7f 00 00 01 40 9c 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 3d 24 90 88 9d 37 4c
29 6f ab 1d 57 70 cb 71 47 6a 06 f7 4e f2 f2 54
0c f2 21 5f 76 14 f4 80 1c 39 2d 17 4a 38 b3 b1
be af af 1f e8 24 87 08 41 c5 fa 53 1b c6 ea fd
b6 40 2c 12 46 64 48 8c 1c a2 b9 57 f0 72 74 7e
f5 ea 9e 73 f1 a1 d1 c2 d1 18 60 72 cd 71 30 5c
22 e1 e4 84 55 14 6f 1b 4d 53 03 ab 2f 8c 0c f0
08 62 b2 7b b9 46 f0 e6 8d 71 10 d0 d0 2c 7d 32
a8 32 54 57 e1 98 48 28 35 3d 13 d8 84 01 34 99
64 8a 55 d5 1f 5d ad 4c 37 4d 1b d6 c2 40 e7 70
07 e1 55 11 3c bd a3 57 38 50 7b 96 40 1a a6 10
cf 2f 5c 42 e8 4a 92 93 90 bb af c5 da 0e a8 75
d2 61 3b 47 d3 a0 6c d0 e0 17 8d ee b7 5f ad 63
dd 86 6c 56 a5 09 15 ff 44 8c 3f 2f f1 4d c2 db
b0 60 e5 c5 fc 88 56 5a 00 c4 2a e4 a6 57 a3 38
7e 5d b9 97 a9 f9 62 27 57 33 d0 29 a8 f6 f1 f3
bb 09 a1 e3 e6 ad 15 5f cd 02 e4 69 42 c0 6a c8
75 97 ee 1e 14 ee df a4 b6 4c 43 1f 63 ef fc b2
4a ef 3e 22 ec 05 f2 ef b1 4a 59 bc 5a 80 a1 2e
cb d0 d4 c7 51 bc 21 62 35 37 d5 a5 a6 8b 98 3a
8b 69 4a 9b 9d ed 5e e7 d6 8c 90 c2 0d 0d e3 fb
88 45 5e bc c6 95 27 b6 52 e8 58 8c 90 98 a3 af
c7 61 5d 65 91 92 fe cc 28 29 06 63 e3 62 fc e1
b3 df f5 f0 74 bb 04 fa e3 73 d2 a4 b0 14 5c 12
34 a0 08 7c 44 73 12 21 79 68 2c 1f a1 36 c2 f1
c3 9f 18 b3 48 78 78 ef 05 53 db a3 e9 01 a6 a1
b3 58 a7 3c 8d 34 d2 fd e7 ff 83 ad 04 a1 c7 65
69 5f 9a b9 5f e8 2e 31 71 e6 a0 d5 15 da fd ff
12 b0 91 2a 4c ac 7e ec 77 14 10 19 83 4f 3e b8
af 6a 8d 23 bf ba 36 79 4a 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
80 6f 9f 78 d4 60 9e 47 5d ae 14 5d 4d 0c dc 9b
cc 3f 46 2c d8 83 10 2d d1 dd 1c 65 e6 8b 39 f3
fb 89 f8 82 50 47 65 0f a5 0d e6 b0 70 e0 01 1a
90 a5 2f 7c ac 91 39 a7 85 f2 b6 b6 52 fd 7f 07
01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f 10
11 12 13 14 15 16 17 18 19 1a 1b 1c 1d 1e 1f 20
a1 a2 a3 a4 a5 a6 a7 a8 a9 aa ab ac ad ae af b0
b1 b2 b3 b4 b5 b6 b7 b8 b9 ba bb bc bd be bf c0
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
```

```vector t6.wrong-server-key
input: IPv4 127.0.0.1, port 40000, empty user data, the signed server public key replaced by a fixed key that is not the target server's
input: a token the server MUST refuse at the server-public-key check
93 1a 7c 4e 88 77 66 55 44 33 22 11 00 f9 ac 68
00 00 00 00 2c fa ac 68 00 00 00 00 0a 00 00 00
01 7f 00 00 01 40 9c 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 11 22 33 44 55 66 77
88 99 aa bb cc dd ee ff 00 10 20 30 40 50 60 70
80 90 a0 b0 c0 d0 e0 f0 01 23 b7 bb 8c 91 ae 00
87 11 fb 12 84 67 80 bc df 1e 06 5f 82 1b df ec
49 f5 7e 7c 7d cd 4c 48 23 d2 10 cb 80 cd 26 ae
d6 3b 32 c3 03 f7 a1 c5 c2 35 1e 3b 77 87 5c e2
fa 40 20 77 ec e6 68 29 70 6f b8 b4 9c ed 85 0c
90 cc 84 bc e7 7d 3e 1c f7 53 62 ec 49 c0 23 6a
44 bf 63 7f bb cf aa d7 bb b1 59 83 63 d2 8a fb
57 c7 d5 59 f7 8e dc 84 96 95 46 eb 3f 9e 32 38
8a 4d 3b f9 a2 bd 23 59 4c 0b 92 af d5 a9 5b f1
7b 4e ee c3 14 40 d3 58 e6 39 20 52 c3 e2 b0 f2
cc 2d be a3 9f d1 48 d7 80 77 57 98 70 ea 94 f3
b5 17 6e e6 2f c4 ac 79 29 6b ba 3a 67 cd a2 c6
23 57 2c f8 bf ec 80 8b 3c 3e 9e e4 43 de cf 14
31 a7 c6 78 7c 03 2f d9 17 65 d5 63 42 da 1f 02
59 b0 31 3b 73 b5 5f be b9 1f 3a ef c0 2d 6f 87
91 31 97 11 76 bf 03 37 a9 a8 0e a1 39 a3 b7 92
2e 0b 80 de 84 27 a3 33 84 2d 31 16 08 56 30 78
4a 39 fa b9 6a d2 3e c9 27 60 77 be 6f ff 38 02
85 41 68 ef d3 69 72 89 3b ea ac c0 86 4f 4f 78
5c fe 0d 8a 04 23 f9 c7 9e ed 62 40 7a 78 d5 0b
41 21 ad ff 38 1a c9 0c e3 2c 0b 37 f8 ab 3f 9d
06 8e 02 3b bd 89 c0 1f d3 f4 cc 96 8e b1 d5 9c
94 5d fa 4f 7d c0 ee 5d 5c aa 4c 39 9b 3a 4f cf
83 24 c8 3e ac 3f 78 da 50 a1 d8 8d 47 c2 4a 43
e1 08 83 06 3e e1 89 f5 cd 3a 17 ea 20 c5 f8 7a
bf ae 42 a2 6c 75 d5 36 d1 05 90 0d 91 69 15 96
51 4a c8 c1 c1 6e 4c 92 7c a3 56 0d 59 c3 de 9e
86 0e e1 6f c3 6e 4a 40 2c 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
f7 cf 86 58 f0 9f 18 16 96 e5 99 c0 d2 09 d2 61
5c 8b 62 ed c5 d6 f1 6a 69 c7 59 b3 91 9a 37 80
d2 c7 64 0e bd 57 b2 79 ae df fe 74 32 cb 63 e4
3e 26 01 b2 c6 a0 9c 30 06 9f 32 1c 22 c3 e3 0b
01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f 10
11 12 13 14 15 16 17 18 19 1a 1b 1c 1d 1e 1f 20
a1 a2 a3 a4 a5 a6 a7 a8 a9 aa ab ac ad ae af b0
b1 b2 b3 b4 b5 b6 b7 b8 b9 ba bb bc bd be bf c0
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
```

```vector t6.sealed-mismatch
input: IPv4 127.0.0.1, port 40000, empty user data, the sealed private block's protocol id is the signed protocol id plus one
input: a token whose signature verifies and whose sealed block opens, but whose private fields disagree with the signed fields
93 1a 7c 4e 88 77 66 55 44 33 22 11 00 f9 ac 68
00 00 00 00 2c fa ac 68 00 00 00 00 0a 00 00 00
01 7f 00 00 01 40 9c 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 3d 24 90 88 9d 37 4c
29 6f ab 1d 57 70 cb 71 47 6a 06 f7 4e f2 f2 54
0c f2 21 5f 76 14 f4 80 1c 9f d7 ad 6d cf f4 29
8d d3 f9 6d 5b 1b 2a f9 10 a0 53 5b 14 88 d7 f8
fa bb 34 9a 98 28 80 b6 15 c9 ae 23 6b 18 ad 61
1e bf 16 e2 3c 9d cc 80 b2 0b 7b 3a 4a ad 40 f4
20 63 e2 ea 16 12 d1 4e 23 e6 34 58 b6 04 5c 44
32 5a 1f 38 a0 d6 d0 f4 d0 e3 74 5b 28 e4 9a 87
2f 30 e8 b9 2c d2 ec 71 a7 f1 7c be 71 ff 64 1e
76 20 2c 26 6e 24 30 3b d0 15 be f9 03 f5 7c ea
c4 dd 4d 2d 8f d6 8a 80 16 d4 fc 43 a2 0b f4 5d
5c f7 77 42 30 58 9f 98 7b 20 fa 74 64 ce 1c d0
f5 52 f1 eb a7 a2 87 f9 b9 f2 e1 41 d9 b6 50 68
74 79 78 ba 60 89 23 c0 13 6f 24 c9 57 a2 86 7c
04 ae fd 78 70 c1 cf 53 93 16 ef 9a 6f 1f ec 93
ab 6a 9e cf 66 45 de 72 91 e7 58 47 94 5e e9 1c
f4 f5 3c 36 de 56 d7 cb b5 17 53 30 11 5a 67 14
20 85 c4 7d 79 50 02 b6 29 79 fb 54 eb 33 f4 56
4a 34 58 52 b5 1f ea e8 4b 07 ae 42 6d 12 3f fd
42 80 2a 44 a4 1f b0 1f 03 0a 5f f3 1a c4 df 3f
d8 26 50 a2 43 64 89 9a bb a5 b3 0f f2 80 70 56
00 b0 1c ea d1 fb ac 12 f6 8d 13 d4 ef 19 48 57
42 42 b7 24 4e 18 42 85 0b 68 43 91 d8 dd 58 e9
85 19 9f 03 ec d5 f1 b2 28 b6 2e 6b 38 4b 18 b0
6f 9a 17 1a 39 a6 64 ef 49 6a 9f 87 11 93 fa 31
6d eb da de da ca 44 00 fa 44 09 c5 be 84 30 79
c0 70 2b 52 ee dd 59 5e 58 37 b8 9c e2 0a 27 19
74 eb b3 84 17 5f 54 dc b9 21 ea fb 64 65 be ff
fe e0 3d 07 71 7e 56 7e 80 0b 7b 22 2a 31 b0 11
7b 1d 4d fc 8e 66 89 14 3f 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
61 99 ef a8 e8 67 01 40 c3 1b 0e 6c ea b4 29 81
f7 7d 88 51 69 01 87 b2 15 69 9b b2 4d ea c5 52
5f c1 5a 46 22 f7 2e 68 f7 af 40 cf 2b d9 3e fd
5a 4d 79 ca fb 1e 9c d9 f5 40 fc 5a f3 f0 03 05
01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f 10
11 12 13 14 15 16 17 18 19 1a 1b 1c 1d 1e 1f 20
a1 a2 a3 a4 a5 a6 a7 a8 a9 aa ab ac ad ae af b0
b1 b2 b3 b4 b5 b6 b7 b8 b9 ba bb bc bd be bf c0
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
```

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
| [t4 Wire encoding primitives](#t4) | `t4.address-ipv4` |
| [t4 Wire encoding primitives](#t4) | `t4.address-ipv6` |
| [t4 Wire encoding primitives](#t4) | `t4.sequence-start` |
| [t4 Wire encoding primitives](#t4) | `t4.sequence-steady` |
| [t4 Wire encoding primitives](#t4) | `t4.sequence-tie` |
| [t4 Wire encoding primitives](#t4) | `t4.sequence-wrap` |
| [t4 Wire encoding primitives](#t4) | `t4.sequence-zero` |
| [t6 Connect token](#t6) | `t6.bad-address-flags` |
| [t6 Connect token](#t6) | `t6.frozen-token` |
| [t6 Connect token](#t6) | `t6.sealed-mismatch` |
| [t6 Connect token](#t6) | `t6.token-identifier` |
| [t6 Connect token](#t6) | `t6.wrong-server-key` |
<!-- vector-index:end -->

<a id="tb"></a>
## tb. Appendix B: changes from netcode 1.02

This appendix lists, as facts, the differences of RN-1 from netcode 1.02 in
the bytes on the wire and in the decisions a receiver makes. It states no
requirements; each difference is specified by the section named in its row.

| Area | netcode 1.02 | RN-1 |
|---|---|---|
| Backend trust | The backend and every server share a secret that authenticates connect tokens. | The backend holds an Ed25519 signing keypair and every server holds its public key; a server holds no secret shared with the backend ([t2.2 Key inventory](#t2.2)). |
| Token authenticity | A connect token carries an AEAD authenticator computed under the shared secret. | The token carries an Ed25519 signature over its signed connect data, and its private block is sealed to the server's X25519 public key ([t6 Connect token](#t6)). |
| Token scope | A token names a list of candidate servers. | A token names one server at one IPv4 address and one IPv6 address, each optional ([t2 Architecture and key inventory](#t2)). |
| Token binding | A token is bound to a server address and an expiry. | A token is also bound to the source address whose connection response first opened under it, and admits one live connection ([t12 Handshake: server](#t12)). |
| Packet keys | The session keys minted into the token are the packet keys. | The token keys key a derivation only; each attempt derives separate handshake and session keys from a client salt, a server session nonce and an ephemeral X25519 exchange ([t7 Key schedule](#t7)). |
| Forward secrecy | Recovering a token recovers the traffic sealed with its keys. | Recovering a token after a connection has ended does not recover that connection's traffic ([t7 Key schedule](#t7)). |
| IPv6 | Added after the first release. | Dual stack from the first release, with one address normalization rule ([t2 Architecture and key inventory](#t2)). |
| Packet prefix | No prefilter header precedes a packet. | Every packet begins with the prefilter header ([t5 Prefilter header](#t5)). |
| Payload to an unconfirmed client | A keep-alive prefixes every payload until the client is confirmed. | Session keys exist from the challenge, so payloads are delivered from that point with no prefix ([t13 Session: keep-alive, timeout, disconnect, slot reset](#t13)). |
| Fragmentation | No fragment packet type. | The fragment packet type carries a fragment, with reassembly slots and an abandonment rule ([t16 Fragmentation and reassembly](#t16)). |
| A zero timeout | A timeout of zero is accepted. | A timeout outside the field's valid range is refused, and zero is outside it ([t2.3 Configuration surface](#t2.3)). |

<a id="tc"></a>
## tc. Appendix C (informative): implementation notes

This appendix is informative. It records implementation choices that the wire
format does not require; a conforming implementation may choose otherwise.
Each note points to the section whose rule it serves.

### Transport selection

An implementation offers a set of transports for one endpoint role on one
platform, and `auto` expands to its documented order for that role and
platform. An explicit list is tried in order, nothing is appended or
reordered, and a list of one name is a pin that refuses rather than falls.
The endpoint reports the transport that carries it and logs each earlier
candidate's skip or refusal. A transport never changes a byte on the wire, so
transport selection is a deployment and diagnosis surface rather than a
protocol one. See [t2.4 Transport selection](#t2.4).

### Kernel prefilter and XDP

A Linux server may place the prefilter and an early replay check in an XDP
program so that traffic the connection layer would refuse is dropped before
it is queued to userspace. The kernel path may perform any prefix of the
receive checks; it rejects no more and no less than the full sequence, and the
userspace path still performs the checks on what survives. Traffic the kernel
does not see is not counted there, so the transport imports the kernel
counters beside its own. See [t10 Receive processing](#t10) and
[t14 Replay protection](#t14).

### Check placement

Every check this standard specifies is defined by what it rejects and in what
order, never by where it runs. A transport may perform any prefix of the
sequence as early as it can, including in a kernel program, and a transport
without such a path performs the same checks in userspace. Pushing work down
reduces the volume that reaches userspace rather than removing the check from
it, which keeps one implementation of the logic rather than one per transport.
See [t10 Receive processing](#t10).

### Configuration surface

Each configuration field is declared once with its type, default and valid
range, and the environment variable name derives from the field name. The
precedence is default, then configuration struct, then environment; every
override is logged with the value it displaced, and an out-of-range or
unparsable value refuses the create rather than being clamped or corrected.
The message layer's fields follow the same surface in the section that owns
each one. See [t2.3 Configuration surface](#t2.3).

### Counters per execution context

A multithreaded implementation keeps a counter block per execution context,
padded and aligned so that no two contexts share a cache line, and sums the
blocks at readout. In a kernel dataplane the context is a CPU; in userspace
it is a thread. Counters are monotonic and never reset, so a rate is derived
by differencing two snapshots and a zero still means that the event never
happened. See [t18 Error handling and counters](#t18).
