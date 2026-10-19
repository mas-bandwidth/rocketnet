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

Every packet a sender emits carries this header, and a receiver reads it
before any other field. Its size is the row named prefilter header in
[t3 Constants and registries](#t3).

1. A sender MUST begin every packet with the prefilter header.
2. A receiver MUST read the packet type from byte offset 0, the filter epoch
   from byte offset 1, and the filter value from byte offsets 2 through 5. A
   receiver MUST NOT read a prefilter field at any other offset.
3. A receiver MUST apply the checks of this section in the order given, before
   any per-type check and before any cryptographic operation.
4. A receiver MUST refuse a packet shorter than the prefilter header as too
   short.
5. A receiver MUST refuse a packet longer than the receive ceiling, the row
   named receive ceiling in [t3 Constants and registries](#t3), as too large.
6. A receiver MUST refuse a packet whose filter epoch is not the unkeyed
   filter epoch, the row named unkeyed filter epoch in
   [t3 Constants and registries](#t3), as a wrong epoch.
7. A receiver MUST refuse a packet whose filter value does not equal the
   receiver's unkeyed filter value as a wrong value.
8. A receiver MUST return the packet type byte of a packet that passes every
   check to the next processing step.
9. A refusal MUST be silent: the receiver MUST NOT send a response.
10. The unkeyed filter value MUST be a 32-bit per-title constant agreed by
    value. A receiver MUST NOT derive it from the packet, from an address or
    from a port.
11. An implementation MUST take the unkeyed filter value from the field of
    [t2.3 Configuration surface](#t2.3).
12. The unkeyed filter value of the embedded test title is the value carried
    by the accept vector below.

Fields:

    0                   1                   2                   3
    0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |  packet type  |  filter epoch |      filter value [0..1]      |
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |      filter value [2..3]      |
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

| Name | Offset | Size | Type | Meaning | Valid values |
|---|---|---|---|---|---|
| packet type | 0 | 1 | u8 | the type of the packet that follows | 0 to 9; the per-type checks are later |
| filter epoch | 1 | 1 | u8 | the rule that produced the filter value | 0, or 1 to 255 under [t5.1 Keyed epochs (optional extension)](#t5.1) |
| filter value | 2 | 4 | bytes[4] | the unkeyed tag or a keyed MAC | any 4 bytes |

Every constant this section states is a row of the table below.

| Name | Value | Unit | Kind |
|---|---|---|---|
| packet type field width | `1` | bytes | size |
| filter epoch field width | `1` | bytes | size |
| filter value field width | `4` | bytes | size |
| prefilter header width | `6` | bytes | size |
| first keyed filter epoch | `1` | | registry |
| last keyed filter epoch | `255` | | registry |
| keyed filter input width | `7` | bytes | size |

### t5.1. Keyed epochs (optional extension)

This subsection is an optional extension. It is not part of conformance: an
implementation that omits it conforms to this standard.

An implementation MAY accept filter epochs from the row named first keyed
filter epoch through the row named last keyed filter epoch in the table above.
A receiver that accepts them MUST hold a prefilter magic for each epoch it
accepts, and MUST compute the expected filter value as the low 4 bytes,
little-endian, of SipHash-2-4 under that magic over the keyed filter input:
the packet type, the packet length as a little-endian 16-bit value, then the
first 4 bytes after the prefilter header. The packet length is the length of
the whole packet in bytes. The 4 bytes after the prefilter header are read at
the same offset for every packet type.

A receiver that accepts keyed epochs MUST refuse a packet whose filter epoch
names no magic it holds, before computing any MAC, and MUST refuse a packet
shorter than the prefilter header plus the 4 bytes the keyed filter input
covers, before computing any MAC. It MUST compare the computed value with the
packet's filter value in constant time. It MUST NOT accept the unkeyed filter
epoch.

A sender that uses keyed epochs MUST use the generation most recently
distributed to it. Generations MUST advance by one from the first keyed filter
epoch and MUST wrap from the last keyed filter epoch back to the first; the
unkeyed filter epoch MUST NOT name a generation.

An implementation MUST NOT ship a default prefilter magic.

Vectors:

Each vector states the packet a receiver is given and the packet's bytes. The
accept vector is the header a sender writes, and a receiver returns packet
type 5 for it. The reject vector `t5.reject-short` is refused as too short,
`t5.reject-long` as too large, `t5.reject-epoch` as a wrong epoch and
`t5.reject-value` as a wrong value.

```vector t5.unkeyed-accept
packet type: 5
filter epoch: 0
filter value: the embedded test filter value
packet length: 6
05 00 b7 59 01 00
```

```vector t5.reject-short
packet type: 5
filter epoch: 0
filter value: the embedded test filter value
packet length: 5
05 00 b7 59 01
```

```vector t5.reject-long
packet type: 5
filter epoch: 0
filter value: the embedded test filter value
packet length: 1473
05 00 b7 59 01 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00
```

```vector t5.reject-epoch
packet type: 5
filter epoch: 1
filter value: the embedded test filter value
packet length: 6
05 01 b7 59 01 00
```

```vector t5.reject-value
packet type: 5
filter epoch: 0
filter value: 00 00 00 00
packet length: 6
05 00 00 00 00 00
```

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

A client MUST drop a datagram whose source is not the server it dialed
before applying this list. A client protocol call with insufficient output
capacity MUST be refused before this list and MUST increment
`RECEIVE_CURSOR_TOO_SMALL`; it does not increment `PACKETS_RECEIVED`.
A receiver MUST process every datagram that reaches this list through the
ordered checks below. Each check MUST reject exactly the packets named for
it and MUST increment exactly the counter named for it in
[t18 Error handling and counters](#t18). A receiver MUST NOT examine a packet
beyond the first check that rejects it. The list is the same for both ends;
a row marked server or client applies only at that end.

1. A receiver MUST count the datagram as `PACKETS_RECEIVED` on entry to this
   list, before any check.
2. A receiver MUST apply the prefilter of [t5 Prefilter header](#t5). A
   datagram shorter than the prefilter header is `PREFILTER_TOO_SHORT`; a
   datagram longer than the receive ceiling is `PREFILTER_TOO_LARGE`; an
   epoch this receiver does not hold is `PREFILTER_WRONG_EPOCH`; a filter
   value that does not compare is `PREFILTER_WRONG_VALUE`. The comparison is
   constant time when the epoch is nonzero.
3. A receiver MUST reject a packet type at or above the first reserved
   packet type as `RESERVED_TYPE`.
4. A server MUST reject the connection denied, connection challenge and
   connection cookie types as `WRONG_DIRECTION`. A client MUST reject the
   connection request and connection response types as `WRONG_DIRECTION`.
   These types travel in one direction only.
5. A server MUST pass a connection request to
   [t12 Handshake: server](#t12). A client MUST apply the connection cookie
   state gate before checking its exact wire size, then pass an accepted cookie
   to [t11 Handshake: client](#t11). A cookie in any other client state is
   `CLIENT_COOKIE_WRONG_STATE`; a cookie in an accepted state with the wrong
   exact size is `CLIENT_COOKIE_WRONG_SIZE`. Connection requests and cookies
   carry no sealed data.
6. For every other type with both checks, a receiver MUST apply the type's
   size check before its state gate and before any decryption. A packet that
   fails the size check MUST increment the type's `WRONG_SIZE` counter and
   MUST NOT increment the integrity-failure count or a `DID_NOT_OPEN` counter.
7. A receiver MUST apply each remaining type's state gate. A packet the
   current state does not process is dropped with that type's `WRONG_STATE`
   counter.
8. A server MUST look up the connection mapping for the source address. A
   packet with no mapping is `NO_MAPPING`. A client MUST drop a datagram
   whose source is not the server it dialed, before the list above.
9. A receiver MUST reconstruct the 64-bit sequence from the 32-bit wire
   field and the highest sequence already opened. A reconstructed sequence
   of zero is `ZERO_SEQUENCE`.
10. For the session types, a receiver MUST reject a sequence already marked
    received as `ALREADY_RECEIVED`, before decryption. The handshake types
    have no replay test.
11. For the connection denied and connection challenge types, the clear
    session nonce sits between the sequence and the sealed data; a client
    MUST derive handshake keys into temporary storage and MUST commit them
    only after the open succeeds.
12. A receiver MUST open the sealed data with the key for the type and the
    associated data of [t9 Packet protection](#t9). A packet that does not
    open, or whose expected handshake plaintext size is not exact, is
    `DID_NOT_OPEN`; this check alone increments the integrity-failure count
    that [t14 Replay protection](#t14) bounds.
13. A server MUST reject a session packet whose mapping has no assigned
    slot as `NOT_CONNECTED`, after the open and before marking the sequence.
14. For the session types, a receiver MUST mark the sequence received and
    then advance the most recent sequence.
15. For session types, a receiver MUST read and validate the ack block at
    the front of the opened data as [t15 Acknowledgement](#t15) specifies. A
    malformed block is `BAD_ACK_BLOCK`; the type data behind it is not
    examined. Handshake types have no ack block.
16. For session types, a receiver MUST check the opened size less the ack
    block against the type. A mismatch is `WRONG_OPENED_SIZE`.
17. Before applying the ack block, a client MUST validate the index and
    slot count in a keep-alive. The slot count MUST be from 1 through the
    maximum client slot count, and the index MUST be less than the slot count.
    An invalid value is `CLIENT_KEEP_ALIVE_WRONG_OPENED_SIZE`.
18. After the session checks pass, a receiver MUST apply the ack block's
    delivery information and refresh the connection's receive time, then
    perform the type-specific processing. A type-specific refusal increments
    its counter; a fragment refusal does not undo the ack information or
    receive-time refresh, as [t16 Fragmentation and reassembly](#t16)
    specifies. Handshake types follow their type-specific processing after
    their checks pass.

| Step | Check | Position | Refusal counter |
|---|---|---|---|
| 1 | packet counted | entry | |
| 2 | prefilter length, epoch, value | before decryption | `PREFILTER_TOO_SHORT`, `PREFILTER_TOO_LARGE`, `PREFILTER_WRONG_EPOCH`, `PREFILTER_WRONG_VALUE` |
| 3 | reserved type | before decryption | `RESERVED_TYPE` |
| 4 | direction | before decryption | `WRONG_DIRECTION` |
| 5 | client cookie state gate | before exact cookie size | `CLIENT_COOKIE_WRONG_STATE` |
| 5a | client cookie exact size | after cookie state gate | `CLIENT_COOKIE_WRONG_SIZE` |
| 6 | wire size for each other type | before its state gate and before decryption | the type's `WRONG_SIZE` |
| 7 | remaining state gates | before decryption | the type's `WRONG_STATE` |
| 8 | server mapping | before decryption | `NO_MAPPING` |
| 9 | sequence reconstruction | before decryption | `ZERO_SEQUENCE` |
| 10 | replay already received | before decryption | `ALREADY_RECEIVED` |
| 12 | sealed open and expected handshake plaintext size | decryption | `DID_NOT_OPEN` |
| 13 | server slot gate | after decryption, before the replay mark | `NOT_CONNECTED` |
| 15 | session ack block | after decryption | `BAD_ACK_BLOCK` |
| 16 | session opened size less the ack block | after decryption | `WRONG_OPENED_SIZE` |
| 17 | client keep-alive index and slot count | after decryption, before ack application | `CLIENT_KEEP_ALIVE_WRONG_OPENED_SIZE` |
| 18 | type-specific processing | after session ack application, or after handshake checks | the type's own counters |

The fixed sizes of the handshake types, the minimum wire sizes of the session
types, and the response padding are:

| Name | Value | Unit | Kind | Defined in |
|---|---|---|---|---|
| receive connection request wire size | `710` | bytes | size | [t8 Packet formats](#t8) |
| receive connection denied wire size | `34` | bytes | size | [t8 Packet formats](#t8) |
| receive connection challenge wire size | `66` | bytes | size | [t8 Packet formats](#t8) |
| receive connection response wire size | `50` | bytes | size | [t8 Packet formats](#t8) |
| receive connection cookie wire size | `38` | bytes | size | [t8 Packet formats](#t8) |
| receive connection keep-alive minimum wire size | `41` | bytes | size | [t8 Packet formats](#t8) |
| connection payload minimum wire size | `34` | bytes | size | [t8 Packet formats](#t8) |
| connection disconnect minimum wire size | `33` | bytes | size | [t8 Packet formats](#t8) |
| connection ack minimum wire size | `33` | bytes | size | [t8 Packet formats](#t8) |
| connection payload fragment minimum wire size | `40` | bytes | size | [t8 Packet formats](#t8) |
| connection response padding | `24` | bytes | size | [t8 Packet formats](#t8) |
| maximum client slot count | `1000` | slots | limit | [t3 Constants and registries](#t3) |

A session packet's total wire size is its minimum or larger, up to the
receive ceiling. The payload's opened data holds at least one payload byte
after the ack block.

The vectors below name one malformed packet per refusal and the counter it
fires. Each packet begins with the prefilter header of type, epoch and the
test filter value, and each is truncated or altered at the step it tests.

```vector t10.prefilter-too-short
input: a connection payload packet truncated one byte below the prefilter header
counter: PREFILTER_TOO_SHORT
05 00 b7 59 01
```

```vector t10.prefilter-wrong-epoch
input: a connection payload packet whose epoch is not a held generation
counter: PREFILTER_WRONG_EPOCH
05 01 b7 59 01 00
```

```vector t10.prefilter-wrong-value
input: a connection payload packet whose filter value is not the receiver's
counter: PREFILTER_WRONG_VALUE
05 00 00 00 00 00
```

```vector t10.reserved-type
input: a packet whose type is the first reserved packet type
counter: RESERVED_TYPE
0a 00 b7 59 01 00
```

```vector t10.server-wrong-direction
input: a connection denied packet arriving at a server
counter: WRONG_DIRECTION
01 00 b7 59 01 00
```

```vector t10.client-session-wrong-size
input: a connection keep-alive packet one byte below its minimum wire size
counter: CLIENT_KEEP_ALIVE_WRONG_SIZE
04 00 b7 59 01 00
```

```vector t10.prefilter-too-large
input: a connection payload packet one byte above the receive ceiling
counter: PREFILTER_TOO_LARGE
05 00 b7 59 01 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00
```

```vector t10.client-wrong-direction
input: a connection request packet arriving at a client
counter: WRONG_DIRECTION
00 00 b7 59 01 00
```

```vector t10.client-cookie-state-before-size
input: an undersized connection cookie at a client in a state that rejects cookies
counter: CLIENT_COOKIE_WRONG_STATE
07 00 b7 59 01 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00
```

```vector t10.client-wrong-state
input: a connection keep-alive packet at a client not in a session state
counter: CLIENT_KEEP_ALIVE_WRONG_STATE
04 00 b7 59 01 00 e8 03 00 00 1e 9e 15 64 b9 d8
a0 8f 69 ab 93 07 a5 3c d6 85 92 16 2d f7 28 3f
f2 cd e8 36 13 03 f3 45 c3
```

```vector t10.server-no-mapping
input: a connection keep-alive packet from an address with no mapping
counter: KEEP_ALIVE_NO_MAPPING
04 00 b7 59 01 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00
```

```vector t10.server-not-connected
input: a connection keep-alive packet for a mapping with no assigned slot
counter: KEEP_ALIVE_NOT_CONNECTED
04 00 b7 59 01 00 e8 03 00 00 2e 73 ac 3a 5d df
e7 a5 8a f0 4a f3 97 9d f3 b7 56 9c a5 e6 86 63
30 2e d1 bf d8 4e f9 ac 23
```

```vector t10.zero-sequence
input: a connection keep-alive packet whose reconstructed sequence is zero
counter: CLIENT_KEEP_ALIVE_ZERO_SEQUENCE
04 00 b7 59 01 00 00 00 00 00 1e 9e 15 64 b9 d8
a0 8f 69 ab 93 07 a5 3c d6 85 92 16 2d f7 28 3f
f2 cd e8 36 13 03 f3 45 c3
```

```vector t10.already-received
input: a connection keep-alive packet whose sequence is already marked received
counter: CLIENT_KEEP_ALIVE_ALREADY_RECEIVED
04 00 b7 59 01 00 e8 03 00 00 1e 9e 15 64 b9 d8
a0 8f 69 ab 93 07 a5 3c d6 85 92 16 2d f7 28 3f
f2 cd e8 36 13 03 f3 45 c3
```

```vector t10.did-not-open
input: a connection keep-alive packet sealed under a key the receiver does not hold
counter: CLIENT_KEEP_ALIVE_DID_NOT_OPEN
04 00 b7 59 01 00 e8 03 00 00 f8 c3 ad 9b 05 4e
a2 d3 b5 79 df 29 48 34 2f ad 2d ca b4 05 1d 60
e7 be f3 17 65 04 cf 34 2a
```

```vector t10.bad-ack-block
input: a connection keep-alive packet whose ack block is malformed
counter: CLIENT_KEEP_ALIVE_BAD_ACK_BLOCK
04 00 b7 59 01 00 e8 03 00 00 1b 9e 15 64 b9 d8
a0 8f 69 ab 93 03 a5 3c d6 c1 26 ae 3c ae 1b d1
78 c4 a3 c6 ca 6f 48 a0 17
```

```vector t10.wrong-opened-size
input: a connection keep-alive packet with nine type bytes after the ack block
counter: CLIENT_KEEP_ALIVE_WRONG_OPENED_SIZE
04 00 b7 59 01 00 e8 03 00 00 1e 9e 15 64 b9 d8
a0 8f 69 ab 93 03 a5 3c d6 c6 2e 2c 88 af c2 45
4f 6f 62 77 03 4b 30 f3 49 2a
```

```vector t10.keep-alive-invalid-index
input: a validly opened keep-alive whose client index equals its slot count
counter: CLIENT_KEEP_ALIVE_WRONG_OPENED_SIZE
04 00 b7 59 01 00 e8 03 00 00 1e 9e 15 64 b9 d8
a0 8b 69 ab 93 07 a5 3c d6 11 13 7c 3d 36 90 4e
d3 30 e5 ec f7 45 2e 8e 62
```

```vector t10.fragment-zero-payload-id
input: a connection payload fragment packet naming payload id zero
counter: FRAGMENT_ZERO_PAYLOAD_ID
09 00 b7 59 01 00 e8 03 00 00 1e 9e 15 64 b9 d8
a0 8f 69 ab 93 03 a4 d2 ec a3 b0 d7 eb 0e af 4b
9b b6 e3 94 bd 3b 5d 2c
```

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

A connection enters the session when the server assigns its slot and sends
the first keep-alive. It ends by a deliberate disconnect by either endpoint,
by either endpoint's timeout, or by the server's stop.

1. An endpoint MUST generate keep-alives as
   [t13.1 Keep-alive generation](#t13.1) says, paced by elapsed time at the
   `send rate` row of [t3 Constants and registries](#t3).
2. An endpoint MUST refresh the connection's idle clock when it accepts an
   opened session packet from the peer, whatever the packet type.
3. Every path that ends a connection MUST run exactly one reset on each side
   ([t13.4 Slot reset](#t13.4)).
4. An endpoint MUST ignore a session event in a state whose row of the state
   table does not list it.

The table is one endpoint's view. `sending connection response` is a client
state; a server holds a slot from the first keep-alive it sends.

| State | Event | Guard | Action | Next state |
|---|---|---|---|---|
| connected | keep-alive interval elapsed | this endpoint is a server, and no keep-alive it sent has been acknowledged | send a keep-alive and stamp the keep-alive clock | connected |
| connected | keep-alive interval elapsed | the interval has elapsed since the last send of anything, and this endpoint is a client or a keep-alive has been acknowledged | send a keep-alive | connected |
| connected | any session packet sent | | stamp the send clock | connected |
| connected | any session packet accepted from the peer | | refresh the idle clock | connected |
| connected | idle timeout elapsed | the timeout is positive, and no packet has been accepted for the timeout | reset | disconnected |
| connected | local disconnect | | send the redundant disconnect burst ([t13.3 Disconnect](#t13.3)) and reset | disconnected |
| connected | disconnect opened | | reset | disconnected |
| sending connection response | keep-alive opened | | store the client index and the client slot count, and refresh the idle clock | connected |
| sending connection response | disconnect opened | | reset | disconnected |
| sending connection response | local disconnect | | send the redundant disconnect burst ([t13.3 Disconnect](#t13.3)) and reset | disconnected |
| disconnected | connection established | | assign the slot and send the first keep-alive | connected |

<a id="t13.1"></a>
### t13.1. Keep-alive generation

A keep-alive is a sealed session packet of type `connection keep-alive
packet` ([t3 Constants and registries](#t3)). Its type data names the client
index and the client slot count.

1. A server MUST send a keep-alive whenever the send rate interval has
   elapsed since it last sent that peer a keep-alive, until a keep-alive it
   sent to that peer has been acknowledged. Ordinary traffic MUST NOT
   suppress this keep-alive.
2. A server MUST send a keep-alive whenever the send rate interval has
   elapsed since it last sent that peer anything, once a keep-alive it sent
   to that peer has been acknowledged. Ordinary traffic suppresses this
   keep-alive, which appears only while the connection is otherwise idle.
3. A client in `connected` MUST send a keep-alive whenever the send rate
   interval has elapsed since it last sent that peer anything. It keeps no
   acknowledgement latch, so its own traffic always suppresses its
   keep-alives. A client MUST NOT send a keep-alive in any other state.

A client that receives a keep-alive in `sending connection response` MUST
store the client index and the client slot count and MUST enter `connected`.
A client that receives a keep-alive in `connected` MUST refresh its idle
clock, as it does for any other accepted packet.

An acknowledgement of a keep-alive is the peer's claim to have received it.
A server MUST take the claim at face value and MUST NOT wait for any other
evidence before entering the regime of rule 2.

The type data of a keep-alive is:

     0                   1                   2                   3
     0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    |                          client index                         |
    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    |                       client slot count                       |
    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

| Name | Offset | Size | Type | Meaning | Valid values |
|---|---|---|---|---|---|
| client index | 0 | 4 | u32 | the slot the server assigned this connection | 0 to one less than the client slot count |
| client slot count | 4 | 4 | u32 | the server's configured number of client slots | 1 or more |

A keep-alive's type data is eight bytes and follows the ack block
([t15 Acknowledgement](#t15)). A keep-alive carries no application bytes.

The transcript below starts two clocks at 1000.0 with no keep-alive
acknowledged, sends a payload at 1000.10, checks the keep-alive rule at
1000.05, 1000.15, 1000.20 and 1000.30, acknowledges a keep-alive after
1000.30, sends a payload at 1000.40, and checks the rule again at 1000.35,
1000.45 and 1000.55. Each byte is one check: `01` sends a keep-alive, `00`
suppresses one. The payload before the acknowledgement does not suppress the
check at 1000.15; the payload after it suppresses the check at 1000.45.

```vector t13.keep-alive-suppression
input: send rate 10 hertz, both clocks at 1000.0, no keep-alive acknowledged
input: payload sent at 1000.10
input: due checks at 1000.05, 1000.15, 1000.20 and 1000.30
input: keep-alive acknowledged after 1000.30
input: payload sent at 1000.40
input: due checks at 1000.35, 1000.45 and 1000.55
input: one byte per due check, 01 sent and 00 suppressed
00 01 00 01 00 00 01
```

<a id="t13.2"></a>
### t13.2. Timeout

An established connection times out at either end when no packet has been
accepted from the peer for the connection timeout interval.

The interval is the token's `timeout seconds` field ([t6 Connect token](#t6)).
The server MUST refuse a connection request whose timeout is zero, MUST
refuse a negative timeout unless a development mode explicitly permits one,
and MUST clamp a positive timeout to the `server max_timeout_seconds` row of
[t2.3 Configuration surface](#t2.3). A positive timeout is the interval; a
negative timeout disables the timeout.

A server whose connection times out MUST reset its slot
([t13.4 Slot reset](#t13.4)) and MUST destroy the connection's encryption
mapping. A client whose connection times out MUST enter `connection timed
out` and MUST release its connection state.

The timeout stands behind every lost disconnect: a peer that receives no
disconnect still ends the connection when the interval has elapsed.

<a id="t13.3"></a>
### t13.3. Disconnect

An endpoint that ends a connection deliberately MUST send the number of
sealed disconnect packets named by the `server redundant disconnect packets`
or `client redundant disconnect packets` row of
[t3 Constants and registries](#t3), at consecutive session sequence numbers,
each carrying the current ack block. It MUST then release its state
immediately, without waiting for an acknowledgement.

A client MUST send the burst only when it holds session keys: from `sending
connection response` and `connected`. Outside those states it releases its
state with no packet.

A disconnect packet is never answered and never retransmitted. An endpoint
that accepts one MUST treat the connection as ended and MUST reset.

The type data of a disconnect is empty; its sealed plaintext holds only the
ack block.

<a id="t13.4"></a>
### t13.4. Slot reset

Every path that ends a connection MUST run exactly one slot reset on each
side.

A server slot reset MUST clear the slot's client id, user data, confirmed
flag and mapping reference. It MUST destroy the associated encryption
mapping, and with it both key phases, both send sequences and both replay
windows. It MUST zero all key material rather than merely mark the slot
free. The slot is immediately available for reuse, and a connection later
assigned to it MUST start with fresh keys and sequences.

A client reset MUST release its connection state and MUST zero its derived
keys, its token keys and its ephemeral secret key. It ends in `disconnected`.

A slot reset MUST keep the used-token history
([t12 Handshake: server](#t12)). It MUST keep the disconnect reason until
the slot's next connection is established, so an application can read why
the previous connection ended.

When a server stops it MUST disconnect every connected client with the
redundant burst of [t13.3 Disconnect](#t13.3) and reset every slot. A server
that starts again MUST begin with every slot free, no key or sequence from
the previous run, and a fresh used-token history.

<a id="t14"></a>
## t14. Replay protection

Status: pending

Scope: the replay window, its minimum depth and configured depth, the check
placement against decryption, the optional kernel prefilter.

<a id="t15"></a>
## t15. Acknowledgement

A sender MUST place an acknowledgement block carrying its current receive
state at the start of every session packet's sealed plaintext. An
acknowledgement block reports received session sequences; it does not report handshake sequences or make a
claim about any sequence below its last range.

<a id="t15.1"></a>
### t15.1. Acknowledgement block

The block is encoded in the fields below. Range entry index i starts at zero.

```
      0                   1                   2                   3
      0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     |             largest received (bytes 0-3)                     |
     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     | count | first range length (bytes 5-6) | first gap byte       |
     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     | first gap byte | first range entry's length (bytes 9-10) | next gap ...|
     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

| Name | Offset | Size | Type | Meaning | Valid values |
|---|---:|---:|---|---|---|
| largest received | 0 | 4 | u32 | Low 32 bits of the highest session sequence successfully opened from the peer | Any u32; zero denotes no opened session packet only after reconstruction |
| range count | 4 | 1 | u8 | Number of gap-and-received-range entries following the first range | Zero through [t15.2 Block limit](#t15.2) (range count maximum) |
| first range length | 5 | 2 | u16 | Length of the contiguous received run ending at largest received | Zero exactly when reconstructed largest received is zero; otherwise nonzero and no greater than largest received |
| gap | 7 + 4i | 2 | u16 | Number of unreceived sequences below the preceding received run, for entry i | Nonzero |
| range length | 9 + 4i | 2 | u16 | Length of the received run below entry i's gap | Nonzero |

The block length is the [t3 Constants and registries](#t3) (ack block minimum row) plus each declared range entry's gap and range-length fields. Bytes after that length belong to the packet type's
data, when present. The range count maximum is pinned below.

A sender MUST construct ranges from the opened session-sequence state it
maintains for the peer. The first range ends at largest received, inclusive.
Each later entry, in order from newest to oldest, describes a nonzero gap and
then a nonzero received run below the preceding run. The block is silent below
its last received run. A sender MUST NOT name a sequence that it did not open.

The largest received field is the low 32 bits of the highest opened 64-bit
session sequence. A receiver MUST reconstruct it as the nonnegative 64-bit value
with those low 32 bits that is nearest to its own highest sent session sequence;
the higher value wins an equidistant tie. The receiver MUST evaluate every
validity rule on that reconstructed value.

A receiver MUST reject the block when any of these conditions holds:

1. The opened plaintext does not contain the minimum block, or its declared
   range entries do not fit before the packet type's data.
2. Reconstructed largest received is zero and either first range length or
   range count is nonzero; or reconstructed largest received is nonzero and
   first range length is zero.
3. First range length exceeds reconstructed largest received.
4. Any gap or range length is zero.
5. The sum of first range length and every gap and range length exceeds
   reconstructed largest received, so the lowest described sequence would be
   below sequence one.

A valid block describes only its named runs. A sequence below the last run is
unspoken, not reported as lost. When a block is malformed, a receiver MUST NOT
apply its acknowledgements, examine the packet type data, refresh connection
activity or add acknowledgement debt. The packet's sequence remains replay
marked as specified by [t14 Replay protection](#t14).

<a id="t15.2"></a>
### t15.2. Block limit

| Name | Value | Unit | Meaning |
|---|---:|---|---|
| range count maximum | 255 | entries | Maximum number of gap-and-received-range entries in one block |

<a id="t15.3"></a>
### t15.3. Building and processing

A sender MUST build a block from the single session replay state defined in
[t14 Replay protection](#t14), shared by all opened session packet types. It
MUST list the newest received run first and may include older
runs while its range-count and packet-capacity limits permit. When capacity
omits entries, it MUST drop the oldest entries first and MUST NOT emit a
trailing gap without a received run. Every session packet MUST have room for
the [t3 Constants and registries](#t3) (ack block minimum row). For payload and fragment packets, the sender
reserves room for their type data before choosing the block's range count.

After a valid block is opened, a receiver MUST process its named session
sequences in sent-packet history. It MUST silently ignore sequences below its
retained sent-history window. For a named sequence within that window which it
never sent, it MUST apply no acknowledgment effect and count that sequence in
ACK_UNKNOWN_SEQUENCE. A receiver MUST bound this processing by its retained
history, not by the number of sequence values asserted by the block. A block
is the sender's assertion about its receive state; the receiver cannot verify
that assertion and MUST process an in-window sent sequence as acknowledged.

<a id="t15.4"></a>
### t15.4. Elicitation and standalone acknowledgement

All opened session packet types contribute to the receive state represented
by a block. Acknowledgement debt is the number of accepted eliciting packets
received since a session packet last carried a block. A receiver MUST add debt
for each accepted connection keep-alive, connection payload, connection
disconnect or connection payload fragment packet listed in [t3 Constants and registries](#t3).
A connection ack packet listed there does not elicit an acknowledgement. A
connection ack packet is opened, replay-marked and processed for its block like
any other session packet, but MUST NOT add acknowledgement debt. This rule
prevents acknowledgements from eliciting acknowledgements.

An endpoint SHOULD send a standalone connection ack packet when its
acknowledgement debt reaches the [t3 Constants and registries](#t3) (ack debt
threshold row), and MAY send one sooner when fresh delivery information is
useful. A sender MUST clear the debt when it sends any session packet that
carries a block. A standalone acknowledgement contains the block and no
packet-type data. It is an optimization for timely delivery notification;
correctness does not depend on sending it.

<a id="t15.5"></a>
### t15.5. Round-trip sampling

When a valid block newly acknowledges a sent sequence, the sender MUST take
one round-trip sample for that sequence: receive time minus its recorded send
time. It MUST NOT sample that sequence again from a later block. A negative
elapsed value produces no sample. The sample measures local elapsed time and
does not subtract peer processing or acknowledgement aggregation delay.
The reported RTT MUST be the minimum strictly positive retained first-ack
sample, in milliseconds. If no strictly positive first-ack sample is retained,
the reported RTT MUST be zero.

<a id="t15.6"></a>
### t15.6. Vectors

The all-zero block represents a fresh connection:

```vector t15.fresh
input: largest received = 0; range count = 0; first range length = 0
00 00 00 00 00 00 00
```

One contiguous received run through sequence 1000:

```vector t15.fully-received
input: largest received = 1000; range count = 0; first range length = 1000
e8 03 00 00 00 e8 03
```

A missing sequence separates two received runs:

```vector t15.one-loss
input: largest received = 1000; first range length = 10; gap = 1; range length = 90
e8 03 00 00 01 0a 00 01 00 5a 00
```

Two loss events exercise the full largest-received field:

```vector t15.two-losses
input: largest received = 70000; first range length = 100; ranges = (gap 3, length 500), (gap 1000, length 2000)
70 11 01 00 02 64 00 03 00 f4 01 e8 03 d0 07
```

A genuine sequence whose low 32 bits are zero is not the fresh-connection
block:

```vector t15.sequence-wrap
input: highest sent = 0xfffffff0; reconstructed largest received = 0x100000000; first range length = 16
00 00 00 00 00 10 00
```

The declared geometry is truncated before its range entry is complete:

```vector t15.invalid-short
input: six zero bytes; the minimum block does not fit
00 00 00 00 00 00
```

The range count declares an entry that does not fit in the opened plaintext:

```vector t15.invalid-geometry
input: largest received = 100; declared range count = 1; opened plaintext ends after 10 bytes
64 00 00 00 01 01 00 01 00 01
```

A nonzero largest received cannot have a zero first range:

```vector t15.invalid-zero-first
input: largest received = 100; first range length = 0
64 00 00 00 00 00 00
```

A zero largest received cannot have a nonzero first range:

```vector t15.invalid-fresh-first
input: largest received = 0; first range length = 1
00 00 00 00 00 01 00
```

A fresh block cannot declare a range entry:

```vector t15.invalid-fresh-range
input: largest received = 0; range count = 1; first range length = 0; gap = 1; range length = 1
00 00 00 00 01 00 00 01 00 01 00
```

The first range cannot exceed the reconstructed largest received:

```vector t15.invalid-first-too-long
input: largest received = 10; first range length = 11
0a 00 00 00 00 0b 00
```

A gap cannot be empty:

```vector t15.invalid-zero-gap
input: largest received = 100; first range length = 10; gap = 0; range length = 5
64 00 00 00 01 0a 00 00 00 05 00
```

A received range cannot be empty:

```vector t15.invalid-zero-length
input: largest received = 100; first range length = 10; gap = 5; range length = 0
64 00 00 00 01 0a 00 05 00 00 00
```

The range walk cannot describe a sequence below one:

```vector t15.invalid-walk
input: largest received = 100; first range length = 10; gap = 5; range length = 86
64 00 00 00 01 0a 00 05 00 56 00
```

When block capacity keeps only the newest range, the older range is omitted:

```vector t15.truncated-oldest
input: replay state received 1-5, 7-8 and 10-11; block capacity = 11 bytes
0b 00 00 00 01 02 00 01 00 02 00
```
<a id="t16"></a>
## t16. Fragmentation and reassembly

A payload larger than one type 5 packet's payload capacity is split into
fragments, each carried in its own sealed session packet, and reassembled per
connection by payload id. A payload that fits one packet is never a fragment.

<a id="t16.1"></a>
### t16.1. Sending

1. A sender MUST send a payload whose size is at most the payload capacity
   row of [t3 Constants and registries](#t3) as a type 5 packet
   ([t8 Packet formats](#t8)). A sender MUST split a payload larger than that
   capacity into fragments carried in type 9 packets. The fragments per
   payload maximum row of [t3 Constants and registries](#t3) bounds the
   fragment count.
2. A sender MUST assign each fragmented payload a payload id, per direction:
   an unsigned 32-bit counter that starts at 1, increments by 1 for each
   fragmented payload, and wraps to 1 instead of zero. A sender MUST NOT
   assign zero.
3. A sender MUST use the smallest fragment count of two or more that carries
   the payload with no fragment longer than the fragment stride row of
   [t3 Constants and registries](#t3). The stride is the payload size divided
   by that count, rounded up; every fragment except the last MUST be exactly
   the stride long, and the last MUST carry the remainder, which is between 1
   and the stride.
4. A sender MUST transmit each fragment in its own sealed type 9 packet with
   its own sequence, and MUST NOT retain a fragment for resending. A lost
   fragment loses the whole payload.

<a id="t16.2"></a>
### t16.2. Receiving

Reassembly is per connection, keyed by payload id in a configured number of
reassembly slots, the num_reassembly_slots field of
[t2.3 Configuration surface](#t2.3); the slot of a payload id is that id
modulo the slot count.

A receiver MUST accept a fragment only on a connected slot; a fragment that
arrives in any other state is dropped and counted before it reaches
reassembly ([t10 Receive processing](#t10),
[t13 Session: keep-alive, timeout, disconnect, slot reset](#t13)).

A receiver MUST apply the following checks in order and refuse the fragment
at the first that fails, incrementing the counter named in its row. A refused
fragment leaves its slot unchanged, except that a refusal at check 7 for a
staged final, or at check 8, clears the slot when the slot holds this payload
id.

| # | Check | Counter |
|---|---|---|
| 1 | the type data is shorter than the fragment header plus one byte, or longer than the fragment header plus the received fragment stride maximum row of [t3 Constants and registries](#t3) | size mismatch |
| 2 | the payload id is zero | zero payload id |
| 3 | the stale test of the table below refuses the payload id | stale |
| 4 | the fragment count is one | count invalid |
| 5 | the fragment index is not less than the fragment count | index out of range |
| 6 | the fragment count differs from the count pinned by this payload id | count mismatch |
| 7 | a non-final fragment is not the pinned stride; a final fragment is larger than the pinned stride; or a staged final is larger than the stride the first non-final fragment pins | size mismatch |
| 8 | the reconstructed payload size, the stride times one less than the fragment count plus the known final size, where an absent final contributes one byte, exceeds the configured maximum payload | payload too large |
| 9 | the fragment index is already received for this payload id | duplicate index |

The first fragment of a payload id pins its count, and the first non-final
fragment pins its stride. The final fragment MAY arrive before the stride is
pinned; a receiver MUST stage it and apply check 7 to it when the first
non-final fragment pins the stride. When every fragment of a payload id is
present, the receiver MUST deliver the assembled payload once, at the moment
it completes, and clear the slot. The assembled payload is the first count
minus one fragments of stride bytes each, followed by the final fragment. A
receiver MUST NOT keep a completed payload id beyond the newest completed
payload id that the stale test uses, so a repeated fragment of a completed
payload id starts a new reassembly.

The stale test uses two references.

| Reference | Guard | Outcome |
|---|---|---|
| the connection's newest completed payload id | the payload id differs from it by more than the reassembly reach row of the table below in either direction, compared as unsigned 32-bit differences | refuse, count stale |
| the payload id of the slot's occupant | the payload id is a different id not more than the reassembly behind row of the table below behind the occupant, compared as an unsigned 32-bit difference | refuse, count stale |
| the payload id of the slot's occupant | any other different id | accept the fragment; count the occupant abandoned if it is incomplete; the slot then holds the new payload id |

Only a completed payload advances the newest completed payload id.

| Name | Value | Unit |
|---|---|---|
| reassembly reach | `1073741824` | payload ids |
| reassembly behind | `256` | payload ids |

A reassembly slot MUST own one buffer of the configured maximum payload plus
one staging buffer of the received fragment stride maximum row of
[t3 Constants and registries](#t3), allocated once when the connection is
created, and MUST NOT grow, resize or share either. A connection MUST own one
newest completed payload id. A completed payload is copied out once at
completion.

<a id="t16.3"></a>
### t16.3. Fragment header

The fragment header lies inside the seal after the acknowledgement block of
[t15 Acknowledgement](#t15). The packet type distinguishes a fragment from an
unfragmented payload, and the type is part of the associated data
([t9 Packet protection](#t9)).

    0                   1                   2                   3
    0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |                       payload id (u32)                        |
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   | fragment index | fragment count - 1 |      fragment data ...
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

| Name | Offset | Size | Type | Meaning | Valid values |
|---|---|---|---|---|---|
| payload id | 0 | 4 | u32 | identifies the fragmented payload, per direction | any nonzero value |
| fragment index | 4 | 1 | u8 | index of this fragment | less than the fragment count |
| fragment count minus 1 | 5 | 1 | u8 | one less than the fragment count | at least 1 |

The fragment header is the fragment header row of
[t3 Constants and registries](#t3), the fragment count is between 2 and the
fragments per payload maximum row of [t3 Constants and registries](#t3), and
the stride MUST NOT exceed the fragment stride row of
[t3 Constants and registries](#t3). A receiver MUST refuse a fragment longer
than the received fragment stride maximum row of
[t3 Constants and registries](#t3), as check 1 of the receiving table states.

<a id="t16.4"></a>
### t16.4. Slot states

| State | Event | Guard | Action | Next state |
|---|---|---|---|---|
| empty | a fragment passes every check | — | place its bytes; pin the count and, for a non-final fragment, the stride | partial |
| partial | a fragment of the same payload id passes every check | — | place its bytes | partial |
| partial | the fragment that completes the payload passes every check | every fragment is present | deliver the assembled payload, clear the slot | empty |
| partial | a fragment of a different payload id passes every check | the stale test accepts the payload id | count the occupant abandoned if it is incomplete, clear the slot, start the new payload | partial |
| partial | any check fails | — | refuse; leave the slot as the receiving rule states | partial |

<a id="t16.5"></a>
### t16.5. Vectors

The vector is a reassembly trace on one connection with three slots, a
2000-byte maximum payload and a delivered arena of four payloads. Each
fragment line names the payload id, the fragment count, the fragment index
and the fragment bytes. The twelve bytes of the vector are the reassembly
counters in counter-table order, one byte each: zero payload id, stale, count
invalid, index out of range, count mismatch, size mismatch, payload too large,
duplicate index, accepted, reassembled, abandoned, batch full.

```vector t16.reassembly-trace
set: 1 connection, 3 reassembly slots, 2000-byte maximum payload, 4-capacity delivered arena
frag: payload 7 count 3, fragment 2 (final) first, five 0x41 bytes
frag: payload 7 count 3, fragment 0, eight 0x42 bytes
frag: payload 7 count 3, fragment 1, eight 0x43 bytes
frag: payload 9 count 3, fragment 0, eight 0x44 bytes
frag: payload 9 count 3, fragment 0 again, eight 0x45 bytes
frag: payload 10 count 4, fragment 0, eight 0x46 bytes
frag: payload 10 count 3, fragment 1, eight 0x47 bytes
frag: payload 16 count 2, fragment 0, eight 0x48 bytes
00 00 00 00 01 00 00 01 06 01 01 00
```

The trace stages the final fragment of payload 7 before its stride is known,
completes payload 7, refuses a repeated fragment of payload 9, refuses a
changed count for payload 10, and abandons incomplete payload 10 when payload
16 claims its slot.

<a id="t17"></a>
## t17. Timing and defaults

Every rate in RN-1 is fixed and measured in elapsed seconds. No rate adapts to
the round trip time, and no rate is expressed per application update. Time is
an input ([t1.4 Conventions](#t1.4)).

1. A sender MUST retransmit a handshake packet, and a server MUST generate a
   keep-alive, at the send rate of [t3 Constants and registries](#t3). The
   schedule is driven by elapsed time, not by the number of application
   updates.
2. A sender MUST keep the send rate constant for the duration of one attempt.
   It MUST NOT back off after a retransmission and MUST NOT derive the rate
   from a round-trip estimate.
3. A server MUST discard the mapping of a handshake that is not established
   when the handshake deadline of [t2.3 Configuration surface](#t2.3) has
   elapsed since the mapping was created. The deadline is fixed at creation and
   is not refreshed.
4. A server MUST discard a half-open mapping when the half-open idle deadline
   has elapsed since the last matching connection request from its address,
   unless the handshake deadline elapses first. A connection request whose
   connection salt matches the mapping refreshes the idle deadline.
5. A client MUST abandon an attempt when the connect token's expire timestamp
   has passed. An attempt MUST NOT run past that timestamp, whether or not its
   own lifetime has elapsed.
6. An endpoint MUST end an established connection when the connection timeout
   has elapsed with no packet received from the peer. The connection timeout is
   the timeout seconds field of the connect token
   ([t6 Connect token](#t6)); a connection whose timeout is zero does not time
   out ([t12 Handshake: server](#t12)).
7. A server MUST accept a connection cookie for the cookie lifetime of
   [t3 Constants and registries](#t3). After the cookie key rotates, the server
   MUST accept a cookie minted under the previous key for one cookie key
   rotation period of [t3 Constants and registries](#t3) and no longer.
8. An endpoint MUST send the redundant disconnect packet count of
   [t3 Constants and registries](#t3) on a deliberate disconnect.
9. An endpoint SHOULD send a standalone ack when its ack debt reaches the ack
   debt threshold of [t3 Constants and registries](#t3), and MUST NOT delay an
   ack past the next packet it sends.
10. A server SHOULD enforce the handshake work budget after the cookie of a
    connection request validates and before any asymmetric operation. When it
    does, a request past the budget MUST be refused with no reply and counted
    as a refusal, and the budget MUST be reset at the start of each update.
11. A server SHOULD apply the failed-signature budget before the signature
    verification. When it does, a request from a source that has already
    failed in the current failed-signature window MUST be refused before the
    verification runs and counted as a refusal.

The values this section introduces are:

| Name | Value | Unit | Meaning |
|---|---|---|---|
| half-open idle deadline | `10` | seconds | a half-open mapping with no request from its address for this long is discarded |
| handshake work budget | `1024` | requests per update | default post-cookie connection requests one server processes in one update |
| failed-signature budget | `90` | failed verifications per window | default failed Ed25519 verifications one server performs in one failed-signature window |
| failed-signature window | `10` | seconds | the period over which the failed-signature budget is counted |

The timing defaults of one implementation are, in this order, the send rate,
handshake deadline, half-open idle deadline, cookie lifetime, cookie key
rotation, server redundant disconnect packets, client redundant disconnect
packets, ack debt threshold, handshake work budget, failed-signature budget and
failed-signature window. Each default is written as an unsigned 64-bit
little-endian integer; a real default is truncated to its integer part. The
vector is their concatenation:

```vector t17.timing-defaults
input: the timing defaults above, in the order stated, each as a little-endian u64
0a 00 00 00 00 00 00 00 05 00 00 00 00 00 00 00
0a 00 00 00 00 00 00 00 03 00 00 00 00 00 00 00
3c 00 00 00 00 00 00 00 0a 00 00 00 00 00 00 00
0a 00 00 00 00 00 00 00 40 00 00 00 00 00 00 00
00 04 00 00 00 00 00 00 5a 00 00 00 00 00 00 00
0a 00 00 00 00 00 00 00
```

<a id="t18"></a>
## t18. Error handling and counters

1. An endpoint MUST increment the RECEIVED counter of a packet type once when a
   packet of that type enters processing, and then exactly one outcome counter
   for that packet. At a quiescent point, after an update returns, each RECEIVED
   counter MUST equal the sum of the counters whose family is that counter. A
   counter whose family is none is outside every family: an arrival or entry
   counter has no outcome, and a side counter moves without changing the
   packet's outcome.
2. An endpoint MUST maintain one counter per distinct reason a packet or a
   request is refused. It MUST NOT merge two reasons into one counter, rename a
   reason, or coin a synonym for a condition the registry below already names.
   A reason for one packet type and the same reason for another packet type are
   distinct counters, not one shared counter.
3. An endpoint MUST increment the outcome counter the observed condition
   names, and no other outcome counter for that packet. Storage MAY be per
   execution context and summed at readout; a counter MUST be monotonic and
   MUST NOT be reset.
4. An implementation that exposes counters as an array MUST assign each counter
   a unique index, MUST NOT renumber an index once assigned, and MUST give a new
   counter an unused index at the end of its group. Two counters MUST NOT share
   an index.
5. An endpoint MUST expose the state of each connection and the reason a
   connection ended. A counter or a log entry is not a substitute for the state.
6. An endpoint SHOULD log every refused packet or request. The counter MUST be
   present whether or not logging is compiled in.
7. An implementation MAY add counters. An implementation MUST implement every
   counter in the registry below, in the family stated, and MUST NOT omit a
   reason the registry names.

The registry of counters, in index order, is:

| Name | Class | Family |
|---|---|---|
| `PACKETS_RECEIVED` | `arrival` | `none` |
| `PREFILTER_TOO_SHORT` | `refusal` | `none` |
| `PREFILTER_TOO_LARGE` | `refusal` | `none` |
| `PREFILTER_WRONG_EPOCH` | `refusal` | `none` |
| `PREFILTER_WRONG_VALUE` | `refusal` | `none` |
| `RESERVED_TYPE` | `refusal` | `none` |
| `WRONG_DIRECTION` | `refusal` | `none` |
| `CONNECTION_REQUEST_RECEIVED` | `arrival` | `none` |
| `CONNECTION_REQUEST_WRONG_SIZE` | `refusal` | `CONNECTION_REQUEST_RECEIVED` |
| `CONNECTION_REQUEST_ALREADY_CONNECTED` | `expected` | `CONNECTION_REQUEST_RECEIVED` |
| `CONNECTION_REQUEST_CHALLENGE_RESENT` | `expected` | `CONNECTION_REQUEST_RECEIVED` |
| `CONNECTION_REQUEST_WRONG_VERSION` | `refusal` | `CONNECTION_REQUEST_RECEIVED` |
| `CONNECTION_REQUEST_WRONG_PROTOCOL_ID` | `refusal` | `CONNECTION_REQUEST_RECEIVED` |
| `CONNECTION_REQUEST_TOKEN_EXPIRED` | `refusal` | `CONNECTION_REQUEST_RECEIVED` |
| `CONNECTION_REQUEST_TIMEOUT_INVALID` | `refusal` | `CONNECTION_REQUEST_RECEIVED` |
| `CONNECTION_REQUEST_BAD_ADDRESS_FLAGS` | `refusal` | `CONNECTION_REQUEST_RECEIVED` |
| `CONNECTION_REQUEST_ADDRESS_NOT_ADVERTISED` | `refusal` | `CONNECTION_REQUEST_RECEIVED` |
| `CONNECTION_REQUEST_WRONG_SERVER_KEY` | `refusal` | `CONNECTION_REQUEST_RECEIVED` |
| `CONNECTION_REQUEST_NO_COOKIE` | `expected` | `CONNECTION_REQUEST_RECEIVED` |
| `CONNECTION_REQUEST_COOKIE_DID_NOT_VERIFY` | `refusal` | `CONNECTION_REQUEST_RECEIVED` |
| `CONNECTION_REQUEST_COOKIE_EXPIRED` | `refusal` | `CONNECTION_REQUEST_RECEIVED` |
| `CONNECTION_REQUEST_TOKEN_ALREADY_USED` | `refusal` | `CONNECTION_REQUEST_RECEIVED` |
| `CONNECTION_REQUEST_SIGNATURE_DID_NOT_VERIFY` | `refusal` | `CONNECTION_REQUEST_RECEIVED` |
| `CONNECTION_REQUEST_COULD_NOT_OPEN_SEALED_BLOCK` | `refusal` | `CONNECTION_REQUEST_RECEIVED` |
| `CONNECTION_REQUEST_SEALED_MISMATCH` | `refusal` | `CONNECTION_REQUEST_RECEIVED` |
| `CONNECTION_REQUEST_CLIENT_ID_ALREADY_CONNECTED` | `refusal` | `CONNECTION_REQUEST_RECEIVED` |
| `CONNECTION_REQUEST_TOKEN_ALREADY_CONNECTED` | `refusal` | `CONNECTION_REQUEST_RECEIVED` |
| `CONNECTION_REQUEST_SMALL_ORDER_KEY` | `refusal` | `CONNECTION_REQUEST_RECEIVED` |
| `CONNECTION_REQUEST_NO_SLOTS` | `refusal` | `CONNECTION_REQUEST_RECEIVED` |
| `CONNECTION_REQUEST_COULD_NOT_ADD_MAPPING` | `refusal` | `CONNECTION_REQUEST_RECEIVED` |
| `CONNECTION_REQUEST_SENT_CHALLENGE` | `expected` | `CONNECTION_REQUEST_RECEIVED` |
| `CONNECTION_REQUEST_INTERNAL_ERROR` | `refusal` | `CONNECTION_REQUEST_RECEIVED` |
| `CONNECTION_RESPONSE_RECEIVED` | `arrival` | `none` |
| `CONNECTION_RESPONSE_WRONG_SIZE` | `refusal` | `CONNECTION_RESPONSE_RECEIVED` |
| `CONNECTION_RESPONSE_NO_MAPPING` | `refusal` | `CONNECTION_RESPONSE_RECEIVED` |
| `CONNECTION_RESPONSE_ZERO_SEQUENCE` | `refusal` | `CONNECTION_RESPONSE_RECEIVED` |
| `CONNECTION_RESPONSE_DID_NOT_OPEN` | `refusal` | `CONNECTION_RESPONSE_RECEIVED` |
| `CONNECTION_RESPONSE_ALREADY_CONNECTED` | `expected` | `CONNECTION_RESPONSE_RECEIVED` |
| `CONNECTION_RESPONSE_CLIENT_ID_ALREADY_CONNECTED` | `refusal` | `CONNECTION_RESPONSE_RECEIVED` |
| `CONNECTION_RESPONSE_ADMITTED_SET_FULL` | `refusal` | `CONNECTION_RESPONSE_RECEIVED` |
| `CONNECTION_RESPONSE_NO_SLOTS` | `refusal` | `CONNECTION_RESPONSE_RECEIVED` |
| `CONNECTION_RESPONSE_CONNECTED` | `expected` | `CONNECTION_RESPONSE_RECEIVED` |
| `KEEP_ALIVE_RECEIVED` | `arrival` | `none` |
| `KEEP_ALIVE_NO_MAPPING` | `expected` | `KEEP_ALIVE_RECEIVED` |
| `KEEP_ALIVE_ZERO_SEQUENCE` | `refusal` | `KEEP_ALIVE_RECEIVED` |
| `KEEP_ALIVE_ALREADY_RECEIVED` | `refusal` | `KEEP_ALIVE_RECEIVED` |
| `KEEP_ALIVE_DID_NOT_OPEN` | `refusal` | `KEEP_ALIVE_RECEIVED` |
| `KEEP_ALIVE_NOT_CONNECTED` | `refusal` | `KEEP_ALIVE_RECEIVED` |
| `KEEP_ALIVE_ACCEPTED` | `expected` | `KEEP_ALIVE_RECEIVED` |
| `PAYLOAD_RECEIVED` | `arrival` | `none` |
| `PAYLOAD_NO_MAPPING` | `expected` | `PAYLOAD_RECEIVED` |
| `PAYLOAD_ZERO_SEQUENCE` | `refusal` | `PAYLOAD_RECEIVED` |
| `PAYLOAD_ALREADY_RECEIVED` | `refusal` | `PAYLOAD_RECEIVED` |
| `PAYLOAD_DID_NOT_OPEN` | `refusal` | `PAYLOAD_RECEIVED` |
| `PAYLOAD_NOT_CONNECTED` | `refusal` | `PAYLOAD_RECEIVED` |
| `PAYLOAD_DELIVERED` | `expected` | `PAYLOAD_RECEIVED` |
| `DISCONNECT_RECEIVED` | `arrival` | `none` |
| `DISCONNECT_NO_MAPPING` | `expected` | `DISCONNECT_RECEIVED` |
| `DISCONNECT_ZERO_SEQUENCE` | `refusal` | `DISCONNECT_RECEIVED` |
| `DISCONNECT_ALREADY_RECEIVED` | `refusal` | `DISCONNECT_RECEIVED` |
| `DISCONNECT_DID_NOT_OPEN` | `refusal` | `DISCONNECT_RECEIVED` |
| `DISCONNECT_NOT_CONNECTED` | `refusal` | `DISCONNECT_RECEIVED` |
| `DISCONNECT_DISCONNECTED` | `expected` | `DISCONNECT_RECEIVED` |
| `MAPPING_CREATED` | `expected` | `none` |
| `MAPPING_IDLE_TIMEOUT` | `expected` | `none` |
| `MAPPING_HANDSHAKE_DEADLINE_EXPIRED` | `expected` | `none` |
| `MAPPING_TABLE_FULL` | `refusal` | `none` |
| `CLIENT_CONNECTED` | `expected` | `none` |
| `SEND_RING_FULL` | `refusal` | `none` |
| `SLOT_WITHOUT_MAPPING` | `refusal` | `none` |
| `CLIENT_DISCONNECTED` | `expected` | `none` |
| `SERVER_DISCONNECTED_CLIENT` | `expected` | `none` |
| `CLIENT_TIMED_OUT` | `expected` | `none` |
| `CLIENT_COOKIE_RECEIVED` | `arrival` | `none` |
| `CLIENT_COOKIE_WRONG_STATE` | `expected` | `CLIENT_COOKIE_RECEIVED` |
| `CLIENT_COOKIE_SALT_MISMATCH` | `refusal` | `CLIENT_COOKIE_RECEIVED` |
| `CLIENT_COOKIE_STORED` | `expected` | `CLIENT_COOKIE_RECEIVED` |
| `CLIENT_CHALLENGE_RECEIVED` | `arrival` | `none` |
| `CLIENT_CHALLENGE_WRONG_STATE` | `expected` | `CLIENT_CHALLENGE_RECEIVED` |
| `CLIENT_CHALLENGE_ZERO_SEQUENCE` | `refusal` | `CLIENT_CHALLENGE_RECEIVED` |
| `CLIENT_CHALLENGE_DID_NOT_OPEN` | `refusal` | `CLIENT_CHALLENGE_RECEIVED` |
| `CLIENT_CHALLENGE_SMALL_ORDER_KEY` | `refusal` | `CLIENT_CHALLENGE_RECEIVED` |
| `CLIENT_CHALLENGE_ACCEPTED` | `expected` | `CLIENT_CHALLENGE_RECEIVED` |
| `CLIENT_DENIED_RECEIVED` | `arrival` | `none` |
| `CLIENT_DENIED_WRONG_STATE` | `refusal` | `CLIENT_DENIED_RECEIVED` |
| `CLIENT_DENIED_ZERO_SEQUENCE` | `refusal` | `CLIENT_DENIED_RECEIVED` |
| `CLIENT_DENIED_DID_NOT_OPEN` | `refusal` | `CLIENT_DENIED_RECEIVED` |
| `CLIENT_DENIED_ACCEPTED` | `refusal` | `CLIENT_DENIED_RECEIVED` |
| `CLIENT_KEEP_ALIVE_RECEIVED` | `arrival` | `none` |
| `CLIENT_KEEP_ALIVE_WRONG_STATE` | `expected` | `CLIENT_KEEP_ALIVE_RECEIVED` |
| `CLIENT_KEEP_ALIVE_ZERO_SEQUENCE` | `refusal` | `CLIENT_KEEP_ALIVE_RECEIVED` |
| `CLIENT_KEEP_ALIVE_ALREADY_RECEIVED` | `refusal` | `CLIENT_KEEP_ALIVE_RECEIVED` |
| `CLIENT_KEEP_ALIVE_DID_NOT_OPEN` | `refusal` | `CLIENT_KEEP_ALIVE_RECEIVED` |
| `CLIENT_KEEP_ALIVE_ACCEPTED` | `expected` | `CLIENT_KEEP_ALIVE_RECEIVED` |
| `CLIENT_PAYLOAD_RECEIVED` | `arrival` | `none` |
| `CLIENT_PAYLOAD_WRONG_STATE` | `expected` | `CLIENT_PAYLOAD_RECEIVED` |
| `CLIENT_PAYLOAD_ZERO_SEQUENCE` | `refusal` | `CLIENT_PAYLOAD_RECEIVED` |
| `CLIENT_PAYLOAD_ALREADY_RECEIVED` | `refusal` | `CLIENT_PAYLOAD_RECEIVED` |
| `CLIENT_PAYLOAD_DID_NOT_OPEN` | `refusal` | `CLIENT_PAYLOAD_RECEIVED` |
| `CLIENT_PAYLOAD_DELIVERED` | `expected` | `CLIENT_PAYLOAD_RECEIVED` |
| `CLIENT_DISCONNECT_RECEIVED` | `arrival` | `none` |
| `CLIENT_DISCONNECT_WRONG_STATE` | `expected` | `CLIENT_DISCONNECT_RECEIVED` |
| `CLIENT_DISCONNECT_ZERO_SEQUENCE` | `refusal` | `CLIENT_DISCONNECT_RECEIVED` |
| `CLIENT_DISCONNECT_ALREADY_RECEIVED` | `refusal` | `CLIENT_DISCONNECT_RECEIVED` |
| `CLIENT_DISCONNECT_DID_NOT_OPEN` | `refusal` | `CLIENT_DISCONNECT_RECEIVED` |
| `CLIENT_DISCONNECT_ACCEPTED` | `expected` | `CLIENT_DISCONNECT_RECEIVED` |
| `KEEP_ALIVE_WRONG_SIZE` | `refusal` | `KEEP_ALIVE_RECEIVED` |
| `KEEP_ALIVE_BAD_ACK_BLOCK` | `refusal` | `KEEP_ALIVE_RECEIVED` |
| `PAYLOAD_WRONG_SIZE` | `refusal` | `PAYLOAD_RECEIVED` |
| `PAYLOAD_BAD_ACK_BLOCK` | `refusal` | `PAYLOAD_RECEIVED` |
| `DISCONNECT_WRONG_SIZE` | `refusal` | `DISCONNECT_RECEIVED` |
| `DISCONNECT_BAD_ACK_BLOCK` | `refusal` | `DISCONNECT_RECEIVED` |
| `ACK_RECEIVED` | `arrival` | `none` |
| `ACK_WRONG_SIZE` | `refusal` | `ACK_RECEIVED` |
| `ACK_NO_MAPPING` | `expected` | `ACK_RECEIVED` |
| `ACK_ZERO_SEQUENCE` | `refusal` | `ACK_RECEIVED` |
| `ACK_ALREADY_RECEIVED` | `refusal` | `ACK_RECEIVED` |
| `ACK_DID_NOT_OPEN` | `refusal` | `ACK_RECEIVED` |
| `ACK_NOT_CONNECTED` | `refusal` | `ACK_RECEIVED` |
| `ACK_BAD_ACK_BLOCK` | `refusal` | `ACK_RECEIVED` |
| `ACK_ACCEPTED` | `expected` | `ACK_RECEIVED` |
| `ACK_UNKNOWN_SEQUENCE` | `expected` | `none` |
| `ACKED_BATCH_FULL` | `refusal` | `none` |
| `CLIENT_KEEP_ALIVE_WRONG_SIZE` | `refusal` | `CLIENT_KEEP_ALIVE_RECEIVED` |
| `CLIENT_KEEP_ALIVE_BAD_ACK_BLOCK` | `refusal` | `CLIENT_KEEP_ALIVE_RECEIVED` |
| `CLIENT_PAYLOAD_WRONG_SIZE` | `refusal` | `CLIENT_PAYLOAD_RECEIVED` |
| `CLIENT_PAYLOAD_BAD_ACK_BLOCK` | `refusal` | `CLIENT_PAYLOAD_RECEIVED` |
| `CLIENT_DISCONNECT_WRONG_SIZE` | `refusal` | `CLIENT_DISCONNECT_RECEIVED` |
| `CLIENT_DISCONNECT_BAD_ACK_BLOCK` | `refusal` | `CLIENT_DISCONNECT_RECEIVED` |
| `CLIENT_ACK_RECEIVED` | `arrival` | `none` |
| `CLIENT_ACK_WRONG_STATE` | `expected` | `CLIENT_ACK_RECEIVED` |
| `CLIENT_ACK_WRONG_SIZE` | `refusal` | `CLIENT_ACK_RECEIVED` |
| `CLIENT_ACK_ZERO_SEQUENCE` | `refusal` | `CLIENT_ACK_RECEIVED` |
| `CLIENT_ACK_ALREADY_RECEIVED` | `refusal` | `CLIENT_ACK_RECEIVED` |
| `CLIENT_ACK_DID_NOT_OPEN` | `refusal` | `CLIENT_ACK_RECEIVED` |
| `CLIENT_ACK_BAD_ACK_BLOCK` | `refusal` | `CLIENT_ACK_RECEIVED` |
| `CLIENT_ACK_ACCEPTED` | `expected` | `CLIENT_ACK_RECEIVED` |
| `CLIENT_ACK_UNKNOWN_SEQUENCE` | `expected` | `none` |
| `CLIENT_ACKED_BATCH_FULL` | `refusal` | `none` |
| `KEEP_ALIVE_WRONG_OPENED_SIZE` | `refusal` | `KEEP_ALIVE_RECEIVED` |
| `PAYLOAD_WRONG_OPENED_SIZE` | `refusal` | `PAYLOAD_RECEIVED` |
| `DISCONNECT_WRONG_OPENED_SIZE` | `refusal` | `DISCONNECT_RECEIVED` |
| `ACK_WRONG_OPENED_SIZE` | `refusal` | `ACK_RECEIVED` |
| `CLIENT_KEEP_ALIVE_WRONG_OPENED_SIZE` | `refusal` | `CLIENT_KEEP_ALIVE_RECEIVED` |
| `CLIENT_PAYLOAD_WRONG_OPENED_SIZE` | `refusal` | `CLIENT_PAYLOAD_RECEIVED` |
| `CLIENT_DISCONNECT_WRONG_OPENED_SIZE` | `refusal` | `CLIENT_DISCONNECT_RECEIVED` |
| `CLIENT_ACK_WRONG_OPENED_SIZE` | `refusal` | `CLIENT_ACK_RECEIVED` |
| `FRAGMENT_RECEIVED` | `arrival` | `none` |
| `FRAGMENT_WRONG_SIZE` | `refusal` | `FRAGMENT_RECEIVED` |
| `FRAGMENT_NO_MAPPING` | `expected` | `FRAGMENT_RECEIVED` |
| `FRAGMENT_ZERO_SEQUENCE` | `refusal` | `FRAGMENT_RECEIVED` |
| `FRAGMENT_ALREADY_RECEIVED` | `refusal` | `FRAGMENT_RECEIVED` |
| `FRAGMENT_DID_NOT_OPEN` | `refusal` | `FRAGMENT_RECEIVED` |
| `FRAGMENT_NOT_CONNECTED` | `refusal` | `FRAGMENT_RECEIVED` |
| `FRAGMENT_BAD_ACK_BLOCK` | `refusal` | `FRAGMENT_RECEIVED` |
| `FRAGMENT_WRONG_OPENED_SIZE` | `refusal` | `FRAGMENT_RECEIVED` |
| `FRAGMENT_ZERO_PAYLOAD_ID` | `refusal` | `FRAGMENT_RECEIVED` |
| `FRAGMENT_STALE` | `path_loss` | `FRAGMENT_RECEIVED` |
| `FRAGMENT_COUNT_INVALID` | `refusal` | `FRAGMENT_RECEIVED` |
| `FRAGMENT_INDEX_OUT_OF_RANGE` | `refusal` | `FRAGMENT_RECEIVED` |
| `FRAGMENT_COUNT_MISMATCH` | `refusal` | `FRAGMENT_RECEIVED` |
| `FRAGMENT_SIZE_MISMATCH` | `refusal` | `FRAGMENT_RECEIVED` |
| `FRAGMENT_PAYLOAD_TOO_LARGE` | `refusal` | `FRAGMENT_RECEIVED` |
| `FRAGMENT_DUPLICATE_INDEX` | `refusal` | `FRAGMENT_RECEIVED` |
| `FRAGMENT_ACCEPTED` | `expected` | `FRAGMENT_RECEIVED` |
| `FRAGMENT_PAYLOAD_REASSEMBLED` | `expected` | `none` |
| `FRAGMENT_PAYLOAD_ABANDONED` | `path_loss` | `none` |
| `FRAGMENT_PAYLOAD_BATCH_FULL` | `refusal` | `none` |
| `CLIENT_FRAGMENT_RECEIVED` | `arrival` | `none` |
| `CLIENT_FRAGMENT_WRONG_STATE` | `expected` | `CLIENT_FRAGMENT_RECEIVED` |
| `CLIENT_FRAGMENT_WRONG_SIZE` | `refusal` | `CLIENT_FRAGMENT_RECEIVED` |
| `CLIENT_FRAGMENT_ZERO_SEQUENCE` | `refusal` | `CLIENT_FRAGMENT_RECEIVED` |
| `CLIENT_FRAGMENT_ALREADY_RECEIVED` | `refusal` | `CLIENT_FRAGMENT_RECEIVED` |
| `CLIENT_FRAGMENT_DID_NOT_OPEN` | `refusal` | `CLIENT_FRAGMENT_RECEIVED` |
| `CLIENT_FRAGMENT_BAD_ACK_BLOCK` | `refusal` | `CLIENT_FRAGMENT_RECEIVED` |
| `CLIENT_FRAGMENT_WRONG_OPENED_SIZE` | `refusal` | `CLIENT_FRAGMENT_RECEIVED` |
| `CLIENT_FRAGMENT_ZERO_PAYLOAD_ID` | `refusal` | `CLIENT_FRAGMENT_RECEIVED` |
| `CLIENT_FRAGMENT_STALE` | `path_loss` | `CLIENT_FRAGMENT_RECEIVED` |
| `CLIENT_FRAGMENT_COUNT_INVALID` | `refusal` | `CLIENT_FRAGMENT_RECEIVED` |
| `CLIENT_FRAGMENT_INDEX_OUT_OF_RANGE` | `refusal` | `CLIENT_FRAGMENT_RECEIVED` |
| `CLIENT_FRAGMENT_COUNT_MISMATCH` | `refusal` | `CLIENT_FRAGMENT_RECEIVED` |
| `CLIENT_FRAGMENT_SIZE_MISMATCH` | `refusal` | `CLIENT_FRAGMENT_RECEIVED` |
| `CLIENT_FRAGMENT_PAYLOAD_TOO_LARGE` | `refusal` | `CLIENT_FRAGMENT_RECEIVED` |
| `CLIENT_FRAGMENT_DUPLICATE_INDEX` | `refusal` | `CLIENT_FRAGMENT_RECEIVED` |
| `CLIENT_FRAGMENT_ACCEPTED` | `expected` | `CLIENT_FRAGMENT_RECEIVED` |
| `CLIENT_FRAGMENT_PAYLOAD_REASSEMBLED` | `expected` | `none` |
| `CLIENT_FRAGMENT_PAYLOAD_ABANDONED` | `path_loss` | `none` |
| `CLIENT_FRAGMENT_PAYLOAD_BATCH_FULL` | `refusal` | `none` |
| `SEND_ARENA_FULL` | `refusal` | `none` |
| `RECEIVE_CURSOR_TOO_SMALL` | `refusal` | `none` |
| `SEND_HISTORY_OVERFLOW` | `refusal` | `none` |
| `SEND_LEASE_FULL` | `refusal` | `none` |
| `INTEGRITY_LIMIT_REACHED` | `refusal` | `none` |
| `CLIENT_COOKIE_WRONG_SIZE` | `refusal` | `CLIENT_COOKIE_RECEIVED` |
| `MESSAGE_SEND_STORE_FULL` | `refusal` | `none` |
| `MESSAGE_SEND_QUEUE_FULL` | `refusal` | `none` |
| `CONNECTION_REQUEST_HANDSHAKE_BUDGET_EXCEEDED` | `refusal` | `CONNECTION_REQUEST_RECEIVED` |
| `CONNECTION_REQUEST_SIGNATURE_VERIFY_LIMIT` | `refusal` | `CONNECTION_REQUEST_RECEIVED` |
| `MESSAGE_UNRELIABLE_DELIVERY_FULL` | `refusal` | `none` |
| `MESSAGE_UNRELIABLE_SEND_FAILED` | `refusal` | `none` |
| `MESSAGE_DUPLICATE` | `path_loss` | `none` |
| `MESSAGE_WINDOW_STALL` | `refusal` | `none` |
| `MESSAGE_SENT_MAP_EVICTED` | `refusal` | `none` |
| `MESSAGE_RESENT` | `path_loss` | `none` |
| `MESSAGE_MEMORY_EXHAUSTED` | `refusal` | `none` |
| `MESSAGE_SEND_TYPE_NOT_ENABLED` | `refusal` | `none` |
| `MESSAGE_TYPE_NOT_ENABLED` | `refusal` | `none` |
| `MESSAGE_UNRELIABLE_OVER_BUDGET` | `refusal` | `none` |
| `MESSAGE_ORDERING_BEHIND` | `refusal` | `none` |
| `MESSAGE_SEQUENCED_STALE` | `path_loss` | `none` |
| `MESSAGE_SEQUENCED_SUPERSEDED` | `path_loss` | `none` |
| `MESSAGE_SEQUENCED_HOLD_FULL` | `refusal` | `none` |
| `MESSAGE_RECEIPT_RING_FULL` | `refusal` | `none` |
| `MESSAGE_RECEIPT_ACKED` | `expected` | `none` |
| `MESSAGE_RECEIPT_LOSS` | `path_loss` | `none` |
| `CONNECTION_RESPONSE_INTERNAL_ERROR` | `refusal` | `CONNECTION_RESPONSE_RECEIVED` |
| `CONNECTION_REQUEST_MAPPING_SALT_MISMATCH` | `refusal` | `CONNECTION_REQUEST_RECEIVED` |
| `SEND_HISTORY_LOST` | `path_loss` | `none` |
| `MESSAGE_BAD_CHANNEL` | `refusal` | `none` |
| `MESSAGE_BAD_BLOCK` | `refusal` | `none` |
| `MESSAGE_BAD_RING` | `refusal` | `none` |
| `MESSAGE_ORDERING_AHEAD` | `refusal` | `none` |
| `MESSAGE_PROTOCOL_VIOLATION` | `refusal` | `none` |
| `MESSAGE_SEND_REFUSED` | `refusal` | `none` |
| `MESSAGE_STALE_CONNECTION_ENTRY` | `expected` | `none` |
| `MESSAGE_NOT_RELEASED` | `refusal` | `none` |
| `XDP_KERNEL_REDIRECTED` | `expected` | `none` |
| `XDP_KERNEL_DROPPED_TOO_SMALL` | `refusal` | `none` |
| `XDP_KERNEL_DROPPED_TOO_LARGE` | `refusal` | `none` |
| `XDP_KERNEL_DROPPED_WRONG_FILTER` | `refusal` | `none` |
| `XDP_KERNEL_DROPPED_INVALID_TYPE` | `refusal` | `none` |
| `XDP_KERNEL_DROPPED_WRONG_EPOCH` | `refusal` | `none` |
| `XDP_KERNEL_PASSED_IPV6_EXTENSION` | `expected` | `none` |
| `CONNECTION_RESPONSE_TOKEN_ALREADY_CONNECTED` | `refusal` | `CONNECTION_RESPONSE_RECEIVED` |
| `CLIENT_SEND_DEFERRED` | `expected` | `none` |
| `XDP_KERNEL_DROPPED_REPLAYED` | `refusal` | `none` |
| `CLIENT_CHALLENGE_WRONG_SIZE` | `refusal` | `CLIENT_CHALLENGE_RECEIVED` |
| `CLIENT_DENIED_WRONG_SIZE` | `refusal` | `CLIENT_DENIED_RECEIVED` |

The vector below encodes the registry in index order. For each counter it holds
the little-endian u32 FNV-1a hash of the ASCII name, the class as one byte
(`0` refusal, `1` expected, `2` path loss, `3` arrival), and the family index
plus one as a little-endian u16, where zero means no family. The counters
appear in the order of the table above.

```vector t18.counter-registry
input: every counter of the registry above, in index order, encoded as stated
84 72 d4 02 03 00 00 64 f9 15 d0 00 00 00 ed 4b
cc 7f 00 00 00 d0 a5 f5 ee 00 00 00 a0 f3 31 3d
00 00 00 b6 f1 b5 1a 00 00 00 c4 cd 1c f7 00 00
00 7b 0a 16 79 03 00 00 9f 6c 53 a8 00 08 00 f4
31 8e a9 01 08 00 e9 bb 27 a7 01 08 00 e2 b4 41
47 00 08 00 a6 f4 da 6d 00 08 00 8f be 18 58 00
08 00 07 8a 62 f3 00 08 00 38 1e 6d 9d 00 08 00
74 ad 3d 54 00 08 00 d9 13 3a 06 00 08 00 86 7d
97 ae 01 08 00 f2 c0 a8 31 00 08 00 b0 54 ce 7a
00 08 00 76 5c 65 f4 00 08 00 c6 3a 5e 6b 00 08
00 83 ac 97 18 00 08 00 39 2d b3 8a 00 08 00 ca
b3 52 27 00 08 00 22 c3 04 a2 00 08 00 d4 48 33
55 00 08 00 2d 71 85 d4 00 08 00 9e 68 81 e9 00
08 00 bc 8f a1 e8 01 08 00 3c 90 b3 08 00 08 00
8d 40 41 06 03 00 00 dd a2 ee 3d 00 21 00 78 5d
38 66 00 21 00 44 c9 d6 31 00 21 00 0e 50 9b 52
00 21 00 12 66 6f 95 01 21 00 10 4e 2a f9 00 21
00 e9 b6 43 44 00 21 00 c3 8d 49 3f 00 21 00 2d
cb 1d e3 01 21 00 14 2d 62 74 03 00 00 dd 09 38
74 01 2b 00 a7 18 c3 61 00 2b 00 7d 7e 14 78 00
2b 00 23 9f e3 9f 00 2b 00 e4 d5 7b 22 00 2b 00
0e ad 34 f4 01 2b 00 ed c0 b2 22 03 00 00 58 82
f2 00 01 32 00 e4 5a 4f d7 00 32 00 a0 c9 d1 a9
00 32 00 ee d1 5a 44 00 32 00 03 62 93 d9 00 32
00 32 36 a2 08 01 32 00 0f 0d 2e 81 03 00 00 ea
c2 5d fe 01 39 00 a2 a8 d3 d1 00 39 00 32 33 cd
4e 00 39 00 ac 22 92 17 00 39 00 45 68 ca 46 00
39 00 b7 e2 2f b2 01 39 00 aa 41 97 7f 01 00 00
e8 16 dd d1 01 00 00 ce 68 d7 b2 01 00 00 82 b4
9c 08 00 00 00 46 2e a2 74 01 00 00 aa 0c f9 d4
00 00 00 83 bb 10 c2 00 00 00 90 a2 72 92 01 00
00 4a 5a 6e f5 01 00 00 41 14 e7 e9 01 00 00 05
ef 7a fd 03 00 00 41 c7 d8 79 01 4a 00 93 c7 db
0b 00 4a 00 39 0d a1 71 01 4a 00 4e 97 94 f8 03
00 00 c8 f0 ed d4 01 4e 00 d5 c9 a8 e0 00 4e 00
d1 50 6e b9 00 4e 00 ef 25 50 d4 00 4e 00 e4 bc
59 85 01 4e 00 4a 27 35 c2 03 00 00 14 27 17 91
00 54 00 81 ec df 6c 00 54 00 85 60 5f 2c 00 54
00 28 d5 7e 91 00 54 00 02 c6 94 a6 03 00 00 9c
f4 48 b7 01 59 00 59 0f 01 f8 00 59 00 f3 6f 06
86 00 59 00 3d 94 80 96 00 59 00 90 0e 20 d4 01
59 00 57 9e e8 34 03 00 00 af 3f fc 5c 01 5f 00
0a f3 97 98 00 5f 00 2a aa 48 00 00 5f 00 d4 c6
3f 68 00 5f 00 ac 22 c8 ca 01 5f 00 39 ab 6d 3b
03 00 00 e5 6b 74 99 01 65 00 80 f2 50 98 00 65
00 bc fd cd 35 00 65 00 e2 f8 90 b5 00 65 00 93
ae 2e 5f 01 65 00 70 4a ee ba 00 2b 00 ae 73 6c
4b 00 2b 00 bd bd d0 30 00 32 00 29 8b cc 0a 00
32 00 bb af 99 b0 00 39 00 7f 4a 29 11 00 39 00
48 12 55 58 03 00 00 ec 98 a5 af 00 71 00 e9 82
82 0e 01 71 00 4b dd b8 35 00 71 00 11 fb 6f a7
00 71 00 af ca d6 4a 00 71 00 c0 d9 e3 57 00 71
00 1a 2f 7e 36 00 71 00 ca 98 66 b0 01 71 00 ed
f3 7c 75 01 00 00 50 0c 6a de 00 00 00 fa 02 ec
a4 00 59 00 c8 4b e0 1e 00 59 00 83 6c 60 12 00
5f 00 07 32 aa 00 00 5f 00 81 24 96 67 00 65 00
4d 51 b5 f9 00 65 00 be 45 d6 10 03 00 00 58 52
df 2f 01 82 00 0e dc 53 ed 00 82 00 a5 1c a6 87
00 82 00 ef 8c 72 ea 00 82 00 01 8c 3c 77 00 82
00 3c 00 08 3b 00 82 00 54 9b f7 7f 01 82 00 9f
c8 27 da 01 00 00 32 bd 59 34 00 00 00 20 8d 29
eb 00 2b 00 1f 85 bd 3d 00 32 00 95 2e 8f a2 00
39 00 14 6d 25 e0 00 71 00 da 2f c9 1f 00 59 00
fd 2f af ea 00 5f 00 9b d7 db 8e 00 65 00 36 4c
15 f1 00 82 00 15 85 fa 9b 03 00 00 45 d5 c2 e0
00 94 00 10 e1 ec 54 01 94 00 6c 08 41 6f 00 94
00 78 e8 d1 7f 00 94 00 f6 89 25 b4 00 94 00 cb
1b 1e 15 00 94 00 d1 30 e1 f6 00 94 00 e7 94 0e
7e 00 94 00 27 51 97 bb 00 94 00 53 4a 56 eb 02
94 00 4d 53 72 c5 00 94 00 21 3d 3a f3 00 94 00
52 4f ed 8a 00 94 00 14 4b f7 f8 00 94 00 49 6d
8a 2f 00 94 00 84 d7 ac 2f 00 94 00 d7 0f 30 4a
01 94 00 48 b0 cf 54 01 00 00 33 4b 1d 7f 02 00
00 c5 1b 4a 3b 00 00 00 87 7f a4 35 03 00 00 ff
b4 7e 10 01 a9 00 13 3b 8f b1 00 a9 00 da d1 d8
0c 00 a9 00 3a 47 73 6e 00 a9 00 04 ce 96 ae 00
a9 00 97 78 a8 bd 00 a9 00 ed 64 90 13 00 a9 00
b1 25 a1 12 00 a9 00 51 c3 b4 49 02 a9 00 1f 07
08 67 00 a9 00 bf f6 bf 6e 00 a9 00 80 4b f1 6e
00 a9 00 1a 79 66 45 00 a9 00 c7 81 a5 eb 00 a9
00 72 61 03 19 00 a9 00 ed 19 7b ce 01 a9 00 5a
f5 d9 f5 01 00 00 15 46 6d 2c 02 00 00 db 57 70
09 00 00 00 35 2c d6 68 00 00 00 b8 11 00 f3 00
00 00 3b c4 a0 c5 00 00 00 7e d1 94 cb 00 00 00
c9 35 a9 56 00 00 00 95 84 42 e9 00 4a 00 87 ed
87 5f 00 00 00 61 30 e8 50 00 00 00 c9 57 7e 86
00 08 00 72 48 0e 32 00 08 00 e3 c7 97 df 00 00
00 41 af 4d ad 00 00 00 12 05 76 0d 02 00 00 5c
34 ae 7c 00 00 00 dd 1a c8 5c 00 00 00 ea 22 62
4a 02 00 00 6e 2d 52 bb 00 00 00 68 59 e8 28 00
00 00 a1 b5 72 18 00 00 00 55 a4 ca 83 00 00 00
d2 8a a7 48 00 00 00 fe a3 8f b7 02 00 00 fd 92
bd 5f 02 00 00 bc 11 09 05 00 00 00 2a 62 77 0c
00 00 00 28 65 77 12 01 00 00 9f ad 65 96 02 00
00 c2 2f db 2c 00 21 00 30 f6 52 fc 00 08 00 6b
c6 a4 41 02 00 00 88 ed b1 56 00 00 00 d6 92 db
95 00 00 00 d7 6a 4d f9 00 00 00 eb 03 7f 49 00
00 00 11 1a 37 e1 00 00 00 9c 47 5e e7 00 00 00
8c 5c 05 26 01 00 00 44 5b fe 0f 00 00 00 ff 64
f7 fa 01 00 00 75 4f 7d 3a 00 00 00 b1 eb 07 8c
00 00 00 e9 53 9e 77 00 00 00 b9 17 2c 07 00 00
00 34 b4 85 d8 00 00 00 b0 cb 42 af 01 00 00 00
d9 df bc 00 21 00 bd b0 38 5c 01 00 00 9f 8d 1d
fd 00 00 00 be a9 a8 b8 00 4e 00 22 20 3f b1 00
54 00
```
<a id="t19"></a>
## t19. Security considerations

An implementation MUST provide the defences the table below names, each by the
mechanism its row names. A change to any mechanism MUST be re-checked against
the property the row it carries.

| Property | Mechanism |
|---|---|
| Only backend-authorized clients connect | Ed25519 signature, verified before the sealed block is opened |
| Session keys confidential in transit | sealed to the server's public key |
| Client identity hidden from observers | client id inside the sealed block |
| Token fields unmodifiable | signature over the whole signed buffer |
| No asymmetric work for a spoofed source | MAC cookie before any signature verification |
| No slot for a client that is not there | challenge answered from the claimed address before any slot |
| Packet type and salt unmodifiable | AEAD associated data |
| Packet replay refused | per-phase, per-direction sequence window; the sequence is the nonce |
| Token reuse from another address refused | used-token history keyed on a hash of the signed data |
| A key or nonce never outlives one connection | keys derived per connection from a fresh salt and a fresh session nonce |
| No response larger than what provoked it | the amplification table, re-checked whenever any size changes |
| Recorded traffic stays closed after any later compromise | ephemeral X25519 exchange; secrets erased at derivation |
| A compromised server mints nothing | servers hold no signing key |

<a id="t19.1"></a>
### t19.1. Amplification

A server MUST NOT send a response larger than the packet that provoked it. Each
row of the table below names a provoking packet and the response it can elicit,
at most; a change to any packet size MUST be re-checked against every row.

| Provoking packet | Response |
|---|---|
| connection request | connection cookie |
| connection request | connection challenge, fresh or resent |
| connection request | connection denied |
| connection challenge | connection response |
| connection response | connection keep-alive, minimum |
| connection response | connection denied |
| connection cookie | nothing |

The sizes that keep every row true are the pinned rows below. The keep-alive row
uses its minimum fresh-block size; a keep-alive grows only when the peer's own
session packets added ranges for it to report, so a larger keep-alive is always
answered traffic, never an amplifier.

| Name | Value | Unit | Meaning |
|---|---|---|---|
| connection request wire size | `710` | bytes | the whole connection request packet |
| connection cookie wire size | `38` | bytes | the only answer to a rejected request |
| connection challenge wire size | `66` | bytes | a fresh or resent connection challenge |
| connection response wire size | `50` | bytes | the whole connection response packet |
| connection keep-alive minimum wire size | `41` | bytes | a first keep-alive with a fresh block |
| connection denied wire size | `34` | bytes | the whole connection denied packet |

Two rules keep the table true rather than merely accurate. Padding used to
satisfy a row MUST be checked on read, for length and content. A server MUST NOT
answer a request it has rejected; the connection cookie is the single exception
and is smaller than the request.

```vector t19.amplification
input: each row's packet type (u8) and wire size (u16), in the rows' order
00 c6 02 07 26 00 02 42 00 03 32 00 04 29 00 01
22 00
```

<a id="t19.2"></a>
### t19.2. Development keys

The published development keys are a fixed backend signing key, a fixed server
public key and its secret half, and a fixed development protocol id. They are
public: anyone who holds them can forge a token a server accepts and open every
session. They exist so a first wiring works without a backend, and they MUST NOT
be load-bearing in production.

1. A development key MUST NOT be used unless a deployment configured it
   explicitly. An endpoint whose configuration supplies no key MUST NOT inherit
   a development credential.
2. An implementation MUST refuse to create an endpoint that would use a
   development key when it is compiled without the development flag, and MUST
   NOT allow configuration alone to override that refusal.
3. Every start that uses a development key MUST log a warning, at warn level or
   louder, saying the keys are public and not for production. A server logs it
   from its own keys; a client logs it when its token's server key is a
   development key.
4. An implementation MUST NOT accept the development backend public key when a
   deployment supplies its own backend public key.
5. Nothing else is relaxed. A development connection runs the same handshake,
   cookie exchange, address validation, sealing, key derivation, replay
   protection and erasure as a production connection.

<a id="t19.3"></a>
### t19.3. What this section does not defend

RN-1 does not defend against a volumetric flood large enough to fill a link:
once the link is saturated, nothing a server does with a packet matters, and
that is answered by capacity and by upstream scrubbing. Nor does RN-1 defend
against an attacker holding many genuinely issued connect tokens; such an
attacker is past every bar this protocol has by definition. Both are the
backend's to answer, and a deployment's denial-of-service posture lives in how
its backend issues tokens, not in how its servers parse packets.

<a id="t20"></a>
## t20. Conformance

An implementation conforms to RN-1 when it produces and accepts the exact binary
formats the sections specify, performs the processing steps in the order given,
and holds the key inventory of [t2 Architecture and key inventory](#t2); in
particular, no secret shared between a backend and a server beyond the keyed
prefilter magic of [t5 Prefilter header](#t5), and signature verification before
the sealed block is trusted.

<a id="t20.1"></a>
### t20.1. Conformance classes

Conformance is claimed per role: client, server, or backend. A claim names the
role and the revision of RN-1 it meets.

* A client implementation MUST reproduce the client handshake, the sealed
  packets, replay protection, acknowledgement and fragmentation, and the
  client's receive checks.
* A server implementation MUST reproduce the server handshake, cookie issue and
  validation, slot assignment, session teardown, and the server's receive
  checks.
* A backend implementation MUST mint connect tokens whose bytes match
  [t6 Connect token](#t6), signed so that signature verification precedes trust
  in the sealed block.

<a id="t20.2"></a>
### t20.2. Demonstrating conformance

A conforming implementation MUST publish, and check itself against, fixed
known-answer vectors covering: a connect token built from fixed keys, the four
derived per-connection keys, one sealed packet of each type with its exact
associated data bytes, and one cookie MAC. Self-consistency proves an
implementation agrees with itself; only known-answer vectors prove that two
implementations agree with each other.

A negative test of a receive check MUST assert two counters: the reason counter,
and the packet type's received counter. The reason alone does not show that a
packet reached the check. A positive test MUST assert the success counter and
independently observe the result: the bytes on the wire, the state transition, or
the slot assigned. A counter with no test that names it is an untested branch;
the counter list and the test list MUST be the same list.

<a id="ta"></a>
## ta. Appendix A: test vectors

The index of the vectors of RN-1 by section is printed by
`python3 tools/conformance/check_rn_standard.py --print-index`; the bytes are
in the sections.

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
