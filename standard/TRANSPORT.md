# RN-1: RocketNet connection layer

Version: 1.0<br>
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
| server protocol_id | `0` | u64 | `ROCKETNET_SERVER_PROTOCOL_ID` | nonzero; equal to the connect token's protocol id |
| server max_clients | `1000` | integer | `ROCKETNET_SERVER_MAX_CLIENTS` | 1 to the server max_clients limit |
| server max_clients limit | `1000` | integer | none: a limit of this revision | |
| server packet_filter | `none` | bytes[4] | `ROCKETNET_SERVER_PACKET_FILTER` | 4 bytes; unset, the unkeyed filter value of [t5 Prefilter header](#t5) |
| server backend_public_key | `none` | bytes[32] | `ROCKETNET_SERVER_BACKEND_PUBLIC_KEY` | an Ed25519 public key |
| server max_payload_bytes | `16384` | integer | `ROCKETNET_SERVER_MAX_PAYLOAD_BYTES` | 0 (the default), or the payload capacity row of [t3 Constants and registries](#t3) to the fragmented payload maximum |
| server max_fragmented_messages | `8` | integer | `ROCKETNET_SERVER_MAX_FRAGMENTED_MESSAGES` | 0 (the default), or 1 to the fragments per payload maximum row of [t3 Constants and registries](#t3) |
| server transport | `"auto"` | string | `ROCKETNET_SERVER_TRANSPORT` | [t2.4 Transport selection](#t2.4) |
| server ipv4_address and ipv6_address, create arguments | none | string | `ROCKETNET_SERVER_IPV4_ADDRESS` and `ROCKETNET_SERVER_IPV6_ADDRESS` | an address and port of that family, or NULL; not both NULL |
| server max_timeout_seconds | `60` | integer | `ROCKETNET_SERVER_MAX_TIMEOUT_SECONDS` | 1 to 3600 |
| server handshake_deadline_seconds | `5` | integer | `ROCKETNET_SERVER_HANDSHAKE_DEADLINE_SECONDS` | 1 to 60 |
| client packet_filter | `none` | bytes[4] | `ROCKETNET_CLIENT_PACKET_FILTER` | 4 bytes; unset, the unkeyed filter value of [t5 Prefilter header](#t5) |
| client max_payload_bytes | `16384` | integer | `ROCKETNET_CLIENT_MAX_PAYLOAD_BYTES` | as the server field |
| client max_fragmented_messages | `8` | integer | `ROCKETNET_CLIENT_MAX_FRAGMENTED_MESSAGES` | as the server field |
| client transport | `"auto"` | string | `ROCKETNET_CLIENT_TRANSPORT` | [t2.4 Transport selection](#t2.4) |
| fragmented payload maximum | `361728` | bytes | none: a limit of this revision | |

The server's X25519 keypair has no configuration field and no environment
variable. The server generates it at create.

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
| ack debt threshold | `1` | packet | send on the next update after an eliciting packet | [t15 Acknowledgement](#t15) |
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
| timeout seconds | 28 | 4 | i32 | the connection timeout in seconds; zero and negative values are invalid and are refused at both ends | `-2147483648..2147483647` |
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

The token keys of [t6 Connect token](#t6) never seal a packet; every
connection derives four working keys, one for each phase and direction, and a
key never serves two roles.

Rules:

1. Both endpoints MUST derive the four keys of a connection from the same
   fixed-length inputs in the same order: the two token keys, the client's
   connection salt, the server's session nonce, the ephemeral exchange and the
   key derivation labels of [t3 Constants and registries](#t3).
2. The client MUST choose a fresh connection salt for every connection
   attempt, from a cryptographically secure random source, and MUST send it in
   the clear in the connection request. The salt is the connection salt row of
   [t3 Constants and registries](#t3).
3. The server MUST choose a fresh session nonce for every encryption mapping
   it creates, from a cryptographically secure random source, and MUST send it
   in the clear in the connection challenge. The nonce is the session nonce row
   of [t3 Constants and registries](#t3). A server MUST NOT reuse a session
   nonce across mappings.
4. Each endpoint MUST use one X25519 keypair for the attempt: the client MUST
   generate a fresh keypair for every attempt, and the server MUST generate one
   for every mapping or take the current keypair of a bounded rotation window.
   The client MUST send its ephemeral public key in the clear in the connection
   request. The server MUST send its ephemeral public key inside the sealed
   connection challenge. Both keys are X25519 public keys of
   [t3 Constants and registries](#t3).
5. Each endpoint MUST compute the shared secret with X25519 from its own
   ephemeral secret key and the peer's ephemeral public key. When the scalar
   multiplication reports a small-order peer public key, the endpoint MUST
   abandon the attempt, MUST NOT derive a session key, and MUST NOT create a
   mapping.
6. An endpoint MUST derive the handshake client-to-server key by keyed BLAKE2b
   (RFC 7693), with the token client-to-server key as the key and the handshake
   input of [t7.1 Handshake key input](#t7.1) as the message, and MUST derive
   the handshake server-to-client key by keyed BLAKE2b with the token
   server-to-client key as the key and the same input as the message.
7. An endpoint MUST derive the session client-to-server key by keyed BLAKE2b,
   with the token client-to-server key as the key and the session input of
   [t7.2 Session key input](#t7.2) as the message, and MUST derive the session
   server-to-client key by keyed BLAKE2b with the token server-to-client key as
   the key and the same input as the message.
8. The output length of every derivation is the derived key row of
   [t7.3 Derived key sizes](#t7.3), and the key length is the BLAKE2b key row
   of [t3 Constants and registries](#t3).
9. A verifier of a connect token MUST use strict Ed25519 verification
   (RFC 8032) over the signed connect data of
   [t3 Constants and registries](#t3) against the backend public key. It MUST
   reject a signature
   whose S component is not reduced modulo the group order, MUST reject an R
   component or public key that is not a canonical encoding or is of small
   order, MUST reject a public key that does not decode to a curve point, and
   MUST NOT accept two distinct signature byte strings for one message.
10. Both endpoints MUST erase the shared secret as soon as the four keys are
    derived, MUST erase an ephemeral secret key when no handshake that needs it
    remains, and MUST erase the four keys and the token keys when the attempt
    or connection that owns them ends.

<a id="t7.1"></a>
### t7.1. Handshake key input

The handshake input is the concatenation below, with the key derivation label
of the direction appended last. It holds no ephemeral key material.

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                                                               |
+                       connection salt                         +
|                                                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                                                               |
+                       session nonce                           +
|                                                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                                                               |
+                    key derivation label                       +
|                                                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

| Name | Offset | Size | Type | Meaning | Valid values |
|---|---|---|---|---|---|
| connection salt | 0 | 8 | bytes[8] | the client's salt for this attempt | any |
| session nonce | 8 | 8 | bytes[8] | the server's nonce for this mapping | any |
| key derivation label | 16 | 8 | bytes[8] | the direction's label row of [t3 Constants and registries](#t3) | `"RKTNHNDC"` or `"RKTNHNDS"` |

<a id="t7.2"></a>
### t7.2. Session key input

The session input is the concatenation below, with the key derivation label of
the direction appended last. The shared secret comes first; the two ephemeral
public keys follow in client then server order, whichever endpoint computes
the input.

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                                                               |
+                        shared secret                          +
|                                                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                                                               |
+                 client ephemeral public key                   +
|                                                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                                                               |
+                 server ephemeral public key                   +
|                                                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                                                               |
+                       connection salt                         +
|                                                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                                                               |
+                       session nonce                           +
|                                                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                                                               |
+                    key derivation label                       +
|                                                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

| Name | Offset | Size | Type | Meaning | Valid values |
|---|---|---|---|---|---|
| shared secret | 0 | 32 | bytes[32] | the X25519 output of rule 5 | any |
| client ephemeral public key | 32 | 32 | bytes[32] | the client's ephemeral public key | a canonical X25519 public key |
| server ephemeral public key | 64 | 32 | bytes[32] | the server's ephemeral public key | a canonical X25519 public key |
| connection salt | 96 | 8 | bytes[8] | the client's salt for this attempt | any |
| session nonce | 104 | 8 | bytes[8] | the server's nonce for this mapping | any |
| key derivation label | 112 | 8 | bytes[8] | the direction's label row of [t3 Constants and registries](#t3) | `"RKTNSESC"` or `"RKTNSESS"` |

<a id="t7.3"></a>
### t7.3. Derived key sizes

| Name | Value | Unit |
|---|---|---|
| derived key | `32` | bytes |
| shared secret | `32` | bytes |

<a id="t7.4"></a>
### t7.4. Vectors

The inputs are the same in every vector below: the token keys, salt, session
nonce and ephemeral keys of the frozen inputs, with the direction's label
appended. The small-order vector's single byte is the value the derivation
returns, zero for a refused derivation.

```vector t7.handshake-client-to-server
input: token client-to-server key = 0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20
input: token server-to-client key = a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebfc0
input: connection salt = 53414c5430303031
input: session nonce = 4e4f4e4345303031
input: key derivation label = "RKTNHNDC"
4b 4d 63 0c db 00 c8 7b 73 d4 6b 34 21 d8 91 c2
4c fd 0a b9 28 c9 87 51 5a ab bb d7 8b e2 73 50
```

```vector t7.handshake-server-to-client
input: token client-to-server key = 0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20
input: token server-to-client key = a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebfc0
input: connection salt = 53414c5430303031
input: session nonce = 4e4f4e4345303031
input: key derivation label = "RKTNHNDS"
86 04 5b 5f 8e 2d a9 4d bf 44 5a 65 80 86 d6 3d
50 b7 3b 1e 66 6c 76 c4 95 2f ab f9 98 c0 c0 e0
```

```vector t7.session-client-to-server
input: token client-to-server key = 0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20
input: token server-to-client key = a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebfc0
input: connection salt = 53414c5430303031
input: session nonce = 4e4f4e4345303031
input: client ephemeral secret key = c1c2c3c4c5c6c7c8c9cacbcccdcecfd0d1d2d3d4d5d6d7d8d9dadbdcdddedfe0
input: client ephemeral public key = 3a553d74792d727efa9b9a4cde3da1ad93f1a2d0c09cb639b1a3c0fda14cbe24
input: server ephemeral public key = 489cebba7a166c619835fe12eedc581dbfc1b36de10c830e4f0bc1c48734864b
input: key derivation label = "RKTNSESC"
ae dc 46 56 e3 f8 2b 7e 87 91 27 95 88 c7 f6 20
4e d2 c1 bc 0c 17 ba 97 79 c0 4e 3b 1c 9e f4 3c
```

```vector t7.session-server-to-client
input: token client-to-server key = 0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20
input: token server-to-client key = a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebfc0
input: connection salt = 53414c5430303031
input: session nonce = 4e4f4e4345303031
input: client ephemeral secret key = c1c2c3c4c5c6c7c8c9cacbcccdcecfd0d1d2d3d4d5d6d7d8d9dadbdcdddedfe0
input: client ephemeral public key = 3a553d74792d727efa9b9a4cde3da1ad93f1a2d0c09cb639b1a3c0fda14cbe24
input: server ephemeral public key = 489cebba7a166c619835fe12eedc581dbfc1b36de10c830e4f0bc1c48734864b
input: key derivation label = "RKTNSESS"
3c 63 7e 37 b1 01 b8 2c 9e da a9 a3 b5 5a 31 a8
a3 bc 10 8f 13 3b a0 5e 8a 41 6b 3c fb 73 0e f0
```

```vector t7.small-order-peer-refused
input: token client-to-server key = 0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20
input: token server-to-client key = a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebfc0
input: connection salt = 53414c5430303031
input: session nonce = 4e4f4e4345303031
input: client ephemeral secret key = c1c2c3c4c5c6c7c8c9cacbcccdcecfd0d1d2d3d4d5d6d7d8d9dadbdcdddedfe0
input: client ephemeral public key = 3a553d74792d727efa9b9a4cde3da1ad93f1a2d0c09cb639b1a3c0fda14cbe24
input: server ephemeral public key = 0000000000000000000000000000000000000000000000000000000000000000
input: key derivation label = "RKTNSESC"
00
```

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
| connection request packet wire size | `710` | bytes | 6 + 704 |
| connection denied packet wire size | `34` | bytes | 6 + 4 + 8 + 16 |
| connection challenge packet wire size | `66` | bytes | 6 + 4 + 8 + 32 + 16 |
| connection response packet wire size | `50` | bytes | 6 + 4 + 24 + 16 |
| connection keep-alive packet minimum wire size | `41` | bytes | 6 + 4 + 7 + 8 + 16 |
| connection payload packet minimum wire size | `34` | bytes | 6 + 4 + 7 + 1 + 16 |
| connection disconnect packet minimum wire size | `33` | bytes | 6 + 4 + 7 + 16 |
| connection cookie packet wire size | `38` | bytes | 6 + 8 + 8 + 16 |
| connection ack packet minimum wire size | `33` | bytes | 6 + 4 + 7 + 16 |
| connection fragment packet minimum wire size | `40` | bytes | 6 + 4 + 7 + 6 + 1 + 16 |

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
input: protocol id 0x1122334455667788
input: connection salt bytes 30 to 37
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
input: protocol id 0x1122334455667788
input: connection salt bytes 30 to 37
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
input: protocol id 0x1122334455667788
input: connection salt bytes 30 to 37
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
input: protocol id 0x1122334455667788
input: connection salt bytes 30 to 37
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
input: protocol id 0x1122334455667788
input: connection salt bytes 30 to 37
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
input: protocol id 0x1122334455667788
input: connection salt bytes 30 to 37
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
input: protocol id 0x1122334455667788
input: connection salt bytes 30 to 37
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
input: protocol id 0x1122334455667788
input: connection salt bytes 30 to 37
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

Every sealed packet carries its per-packet data and its tag encrypted and
authenticated with ChaCha20-Poly1305 (IETF) under one key of the connection.
The associated data binds the version, the application protocol id, the
packet type and the connection salt, and, for the two handshake types that
carry the ephemeral exchange, the client's ephemeral public key. It never
travels on the wire; both ends construct it. The sealed types are the
connection denied, connection challenge, connection response, connection
keep-alive, connection payload, connection disconnect, connection ack and
connection payload fragment packets of [t8 Packet formats](#t8); the
connection request and connection cookie are not sealed.

1. A sender MUST seal the per-packet data of every sealed type with
   `crypto_aead_chacha20poly1305_ietf_encrypt` under the type's key of the
   sender's direction and key phase, the nonce below, and the associated data
   below. The sealed output is the per-packet data followed by the AEAD tag.
2. A receiver MUST open the sealed data with
   `crypto_aead_chacha20poly1305_ietf_decrypt`, the same key, nonce and
   associated data. A receiver MUST reject a packet whose tag does not verify,
   before using any plaintext byte.
3. A sender MUST NOT seal with sequence zero. A receiver MUST reject sequence
   zero before opening the packet.
4. The nonce MUST be the AEAD nonce size of [t3 Constants and registries](#t3):
   four zero bytes followed by the 64-bit sequence as a little-endian integer.
5. The associated data MUST be exactly the fields and length of the table
   below. Its length MUST be the associated data row of
   [t3 Constants and registries](#t3) for types 1, 4, 5, 6, 8 and 9, and the
   associated data with ephemeral key row for types 2 and 3.
6. A sender MUST set the version field to the version row of
   [t3 Constants and registries](#t3), the protocol id to the connection's
   application protocol id, the packet type to the type being sealed, and the
   connection salt to the connection's salt. For types 2 and 3, and only those
   types, the sender MUST append the client's ephemeral public key. A receiver
   MUST construct the same bytes.
7. A receiver MUST reject a sealed packet whose associated data length does not
   match its type.
8. A sender and a receiver MUST each count, per connection and across all of
   that connection's keys, every sealed packet whose AEAD open fails. The count
   MUST NOT include a packet refused before the open for its wire size, its
   state, its sequence or its replay mark, and MUST NOT include a packet that
   opens and then fails a later structural check. On reaching the AEAD
   integrity limit below, the endpoint MUST end the connection and MUST
   increment the `INTEGRITY_LIMIT_REACHED` counter; the client enters the
   disconnected state, and the server removes the mapping and releases the
   slot it holds for it, if any.

The key that seals a sealed type:

| Packet type | Name | Key phase | Direction | Key |
|---|---|---|---|---|
| 1 | connection denied | handshake | server to client | handshake server-to-client |
| 2 | connection challenge | handshake | server to client | handshake server-to-client |
| 3 | connection response | handshake | client to server | handshake client-to-server |
| 4 | connection keep-alive | session | either; the sender's direction | session key of that direction |
| 5 | connection payload | session | either; the sender's direction | session key of that direction |
| 6 | connection disconnect | session | either; the sender's direction | session key of that direction |
| 8 | connection ack | session | either; the sender's direction | session key of that direction |
| 9 | connection payload fragment | session | either; the sender's direction | session key of that direction |

The four keys are derived as [t7 Key schedule](#t7) specifies.

<a id="t9.1"></a>
### t9.1. Associated data

    0                   1                   2                   3
    0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |                          version                              |
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |                        protocol id                            |
   |                                                               |
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |  packet type  |            connection salt                    |
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |               connection salt (continued)                     |
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |                                                               |
   |             client ephemeral public key (32 bytes)            |
   |                  types 2 and 3 only                           |
   |                                                               |
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

| Name | Offset | Size | Type | Meaning | Valid values |
|---|---|---|---|---|---|
| version | 0 | 4 | u32 | the protocol version | the version row of [t3 Constants and registries](#t3) |
| protocol id | 4 | 8 | u64 | the application protocol id | any; equal at both ends |
| packet type | 12 | 1 | u8 | the type of the sealed data | 1 to 6, 8 and 9 |
| connection salt | 13 | 8 | bytes[8] | the connection's salt | the connection's salt |
| client ephemeral public key | 21 | 32 | bytes[32] | the client's ephemeral public key | present for types 2 and 3; absent otherwise |

<a id="t9.2"></a>
### t9.2. Nonce

| Name | Offset | Size | Type | Meaning | Valid values |
|---|---|---|---|---|---|
| zero | 0 | 4 | zero[4] | four zero bytes | zero |
| sequence | 4 | 8 | u64 | the packet's sequence | not zero |

<a id="t9.3"></a>
### t9.3. Constants

| Name | Value | Unit | Kind | Defined in |
|---|---|---|---|---|
| AEAD integrity limit | `68719476736` | sealed open failures | protocol value | [t9 Packet protection](#t9) |

<a id="t9.4"></a>
### t9.4. Vectors

The vectors use these fixed inputs. The application protocol id is
`0x1122334455667788`. The connection salt is
`53 41 4c 54 30 30 30 31` and the session nonce is
`4e 4f 4e 43 45 30 30 31`. The client ephemeral public key is
`3a 55 3d 74 79 2d 72 7e fa 9b 9a 4c de 3d a1 ad 93 f1 a2 d0 c0 9c b6 39 b1 a3 c0 fd a1 4c be 24`
and the server ephemeral public key is
`48 9c eb ba 7a 16 6c 61 98 35 fe 12 ee dc 58 1d bf c1 b3 6d e1 0c 83 0e 4f 0b c1 c4 87 34 86 4b`.
The handshake client-to-server key is
`4b 4d 63 0c db 00 c8 7b 73 d4 6b 34 21 d8 91 c2 4c fd 0a b9 28 c9 87 51 5a ab bb d7 8b e2 73 50`;
the handshake server-to-client key is
`86 04 5b 5f 8e 2d a9 4d bf 44 5a 65 80 86 d6 3d 50 b7 3b 1e 66 6c 76 c4 95 2f ab f9 98 c0 c0 e0`;
the session client-to-server key is
`ae dc 46 56 e3 f8 2b 7e 87 91 27 95 88 c7 f6 20 4e d2 c1 bc 0c 17 ba 97 79 c0 4e 3b 1c 9e f4 3c`;
the session server-to-client key is
`3c 63 7e 37 b1 01 b8 2c 9e da a9 a3 b5 5a 31 a8 a3 bc 10 8f 13 3b a0 5e 8a 41 6b 3c fb 73 0e f0`.

```vector t9.challenge-ad
input: the fixed inputs, packet type 2, with the client ephemeral public key
93 1a 7c 4e 88 77 66 55 44 33 22 11 02 53 41 4c
54 30 30 30 31 3a 55 3d 74 79 2d 72 7e fa 9b 9a
4c de 3d a1 ad 93 f1 a2 d0 c0 9c b6 39 b1 a3 c0
fd a1 4c be 24
```

```vector t9.challenge-sealed
input: the challenge associated data, the handshake server-to-client key, sequence 1, plaintext the server ephemeral public key (32 bytes)
68 6f b7 09 43 5f f0 0e 1a c2 60 ee ea fb b2 91
20 20 10 86 c8 27 62 55 d7 d3 87 c3 92 32 94 7b
3d a1 3d dd 68 56 b0 94 15 f3 08 6c 59 f7 6f 88
```

```vector t9.denied-ad
input: the fixed inputs and packet type 1, with no client ephemeral public key
93 1a 7c 4e 88 77 66 55 44 33 22 11 01 53 41 4c
54 30 30 30 31
```

```vector t9.denied-sealed
input: the denied associated data, the handshake server-to-client key, sequence 2, empty plaintext
e7 12 b8 8e e2 36 cf d9 05 38 7c bc 0f 9d ee 13
```

```vector t9.response-ad
input: the fixed inputs, packet type 3, with the client ephemeral public key
93 1a 7c 4e 88 77 66 55 44 33 22 11 03 53 41 4c
54 30 30 30 31 3a 55 3d 74 79 2d 72 7e fa 9b 9a
4c de 3d a1 ad 93 f1 a2 d0 c0 9c b6 39 b1 a3 c0
fd a1 4c be 24
```

```vector t9.response-sealed
input: the response associated data, the handshake client-to-server key, sequence 1, plaintext 24 zero bytes
9c 08 60 98 96 b9 d4 05 d5 14 9e 1e d9 da e4 dc
2b d2 4a e4 cd a1 93 10 5c bf a9 ce 74 61 b3 97
ce 4b 50 c0 1b 2f 8f 7e
```

```vector t9.keepalive-ad
input: the fixed inputs and packet type 4, with no client ephemeral public key
93 1a 7c 4e 88 77 66 55 44 33 22 11 04 53 41 4c
54 30 30 30 31
```

```vector t9.keepalive-sealed
input: the keep-alive associated data, the session server-to-client key, sequence 1, plaintext the keep-alive block and fields
d2 48 d8 10 b0 69 78 85 27 a0 51 d0 6e bc ea da
25 a6 66 38 35 c4 71 ac 21 66 38 3b a8 42 1c
```

```vector t9.payload-ad
input: the fixed inputs and packet type 5, with no client ephemeral public key
93 1a 7c 4e 88 77 66 55 44 33 22 11 05 53 41 4c
54 30 30 30 31
```

```vector t9.payload-sealed
input: the payload associated data, the session client-to-server key, sequence 3, plaintext the ack block and the five bytes 68 65 6c 6c 6f
e3 e6 e1 ab c8 d7 33 fe f9 d5 e1 e2 af 0d f9 49
ae 6d 5e bc aa df f1 94 89 74 4c b4
```

```vector t9.disconnect-ad
input: the fixed inputs and packet type 6, with no client ephemeral public key
93 1a 7c 4e 88 77 66 55 44 33 22 11 06 53 41 4c
54 30 30 30 31
```

```vector t9.disconnect-sealed
input: the disconnect associated data, the session client-to-server key, sequence 4, plaintext seven zero bytes
e4 db 4c 3d 2d 40 30 39 31 ba 13 55 32 5b fd 8f
2f 3a 2b 31 75 bd a2
```

```vector t9.ack-ad
input: the fixed inputs and packet type 8, with no client ephemeral public key
93 1a 7c 4e 88 77 66 55 44 33 22 11 08 53 41 4c
54 30 30 30 31
```

```vector t9.ack-sealed
input: the ack associated data, the session client-to-server key, sequence 5, plaintext the ack block
cb ac a3 98 67 d9 e7 2a 0e 3f b6 04 85 51 28 e1
5b d4 b1 18 bd d5 96 38 f7 9a e2
```

```vector t9.fragment-ad
input: the fixed inputs and packet type 9, with no client ephemeral public key
93 1a 7c 4e 88 77 66 55 44 33 22 11 09 53 41 4c
54 30 30 30 31
```

```vector t9.fragment-sealed
input: the fragment associated data, the session client-to-server key, sequence 6, plaintext the ack block and fragment header and data
4d 11 93 54 55 cd 7b df e3 97 35 05 ee 20 42 14
33 37 a0 b1 eb a2 ce d3 53 f8 dd b3 78 a4 e5 ed
ff 7f
```

The tamper vectors below are the challenge associated data with one field
altered. No sealed vector opens under any of them.

```vector t9.version-tampered
input: the challenge associated data with the version low byte changed from 93 to 92
92 1a 7c 4e 88 77 66 55 44 33 22 11 02 53 41 4c
54 30 30 30 31 3a 55 3d 74 79 2d 72 7e fa 9b 9a
4c de 3d a1 ad 93 f1 a2 d0 c0 9c b6 39 b1 a3 c0
fd a1 4c be 24
```

```vector t9.protocol-id-tampered
input: the challenge associated data with the protocol id low byte changed from 88 to 89
93 1a 7c 4e 89 77 66 55 44 33 22 11 02 53 41 4c
54 30 30 30 31 3a 55 3d 74 79 2d 72 7e fa 9b 9a
4c de 3d a1 ad 93 f1 a2 d0 c0 9c b6 39 b1 a3 c0
fd a1 4c be 24
```

```vector t9.packet-type-tampered
input: the challenge associated data with the packet type changed from 2 to 3
93 1a 7c 4e 88 77 66 55 44 33 22 11 03 53 41 4c
54 30 30 30 31 3a 55 3d 74 79 2d 72 7e fa 9b 9a
4c de 3d a1 ad 93 f1 a2 d0 c0 9c b6 39 b1 a3 c0
fd a1 4c be 24
```

```vector t9.connection-salt-tampered
input: the challenge associated data with the connection salt first byte changed from 53 to 52
93 1a 7c 4e 88 77 66 55 44 33 22 11 02 52 41 4c
54 30 30 30 31 3a 55 3d 74 79 2d 72 7e fa 9b 9a
4c de 3d a1 ad 93 f1 a2 d0 c0 9c b6 39 b1 a3 c0
fd a1 4c be 24
```

```vector t9.client-ephemeral-tampered
input: the challenge associated data with the client ephemeral public key first byte changed from 3a to 3b
93 1a 7c 4e 88 77 66 55 44 33 22 11 02 53 41 4c
54 30 30 30 31 3b 55 3d 74 79 2d 72 7e fa 9b 9a
4c de 3d a1 ad 93 f1 a2 d0 c0 9c b6 39 b1 a3 c0
fd a1 4c be 24
```

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
12. A server MUST reject a session packet whose mapping has no assigned
    slot as `NOT_CONNECTED`, before decryption. The packet is not opened, so
    the drop says nothing of its authenticity: it MUST NOT increment the
    integrity-failure count or a `DID_NOT_OPEN` counter. The connection
    response is not a session packet and is not gated here.
13. A receiver MUST open the sealed data with the key for the type and the
    associated data of [t9 Packet protection](#t9). A packet that does not
    open, or whose expected handshake plaintext size is not exact, is
    `DID_NOT_OPEN`; this check alone increments the integrity-failure count
    that [t14 Replay protection](#t14) bounds.
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
| 12 | server slot gate | before decryption | `NOT_CONNECTED` |
| 13 | sealed open and expected handshake plaintext size | decryption | `DID_NOT_OPEN` |
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

The client is in exactly one of the states of the table below. A state number is signed: a negative state is how the last attempt or connection ended, and a non-negative state is a state the client acts in.

1. A client MUST be created in disconnected (0) and MUST hold no connect token at creation. A connect token MUST be accepted only in disconnected (0). In every other state, a connect token MUST be ignored and the state MUST NOT change.
2. A connect token accepted in disconnected (0) MUST be checked before any packet is sent. The token is invalid when its address flags name no family or set any bit above the two family flags, when its create timestamp is later than its expire timestamp, when its timeout seconds is zero or negative, or when its version is not the version of [t3 Constants and registries](#t3). An invalid token MUST put the client in invalid connect token (-6). A token whose expire timestamp is not later than the current time MUST put the client in connect token expired (-7). Neither check sends a packet.
3. An accepted token MUST select one server address for the whole attempt: the IPv6 address when its flag is set and the client's host can open an IPv6 socket; otherwise the IPv4 address when its flag is set; otherwise the IPv6 address. A host that cannot open a socket of the selected family MUST NOT send to it.
4. An attempt MUST begin with a fresh connection salt and a fresh ephemeral keypair. The handshake and session sending sequences MUST start at one and MUST NOT be reset within the attempt.
5. The client MUST enter requesting cookie (1), sending connection request (2), sending connection response (3) and connected (4) in that order, each at most once per attempt.
6. While the client is in requesting cookie (1), sending connection request (2) or sending connection response (3), it MUST abandon the attempt for connect token expired (-7) once the token's lifetime, the difference between its expire timestamp and its create timestamp, has elapsed since the attempt began. The check does not apply in connected (4).
7. In requesting cookie (1), sending connection request (2) and sending connection response (3), the client MUST retransmit its current packet at the send rate of [t3 Constants and registries](#t3) until a packet advances it or a deadline ends the attempt. A client MUST NOT send a packet because a packet arrived.
8. In a positive state below connected (4), a client that has accepted no authenticated packet for the token's timeout MUST move to the state whose number is one less than the negation of its current state: requesting cookie (1) to cookie request timed out (-2), sending connection request (2) to connection request timed out (-3), sending connection response (3) to connection response timed out (-4). In connected (4) the same silence MUST move the client to connection timed out (-5). An authenticated packet is one that opens under the attempt's keys: a connection challenge or a session packet. The timeout runs from the attempt's start until the first one, and a connection cookie never restarts it ([t11.1 Cookie processing](#t11.1)).
9. A negative state MUST persist until the client disconnects. A disconnect from any state MUST perform the per-connection reset of [t13 Session: keep-alive, timeout, disconnect, slot reset](#t13) and MUST return the client to disconnected (0). From disconnected (0) the client MAY connect again with a new token.

The state numbers:

| Name | Value | Meaning |
|---|---|---|
| connect token expired | `-7` | the attempt ended at the token's lifetime |
| invalid connect token | `-6` | the attempt never began: the token failed a pre-send check |
| connection timed out | `-5` | an established connection received nothing for the timeout |
| connection response timed out | `-4` | no answer to the response for the timeout |
| connection request timed out | `-3` | no answer to the request for the timeout |
| cookie request timed out | `-2` | no answer to the first request for the timeout |
| connection denied | `-1` | the server denied the attempt |
| disconnected | `0` | the client holds no connection and may connect |
| requesting cookie | `1` | the client waits for a cookie |
| sending connection request | `2` | the client waits for a challenge or a denial |
| sending connection response | `3` | the client waits for the server's first session packet |
| connected | `4` | the connection is established |

<a id="t11.1"></a>
### t11.1. Cookie processing

A connection cookie packet MUST be processed in requesting cookie (1) and sending connection request (2) and MUST be ignored in every other state. A client MUST refuse a packet whose size is not the cookie packet size of [t8 Packet formats](#t8), and MUST ignore one whose echoed connection salt is not the attempt's salt. A client that accepts a cookie MUST store its last 24 bytes, replacing any cookie already stored, and MUST send nothing in response. In requesting cookie (1) the accepted cookie MUST move the client to sending connection request (2); in sending connection request (2) the state MUST NOT change. The client trusts only the server: a cookie's salt rides in the clear, so anyone who sees the request can echo it, and an accepted cookie MUST NOT refresh the client's last-received time and MUST NOT be taken as a round-trip sample. Both come only from the connection challenge and the packets after it that open under the attempt's keys.

<a id="t11.2"></a>
### t11.2. Challenge and denial processing order

A connection challenge packet MUST be processed only in sending connection request (2); in every other state it MUST be ignored. A connection denied packet MUST be processed in sending connection request (2) and sending connection response (3); in every other state it MUST be ignored.

Each packet MUST be checked in this order:

1. Its size MUST be the packet's wire size of [t8 Packet formats](#t8). A client MUST refuse a wrong-size packet as WRONG_SIZE before any state, sequence or cryptographic check, MUST NOT count an integrity failure for it, and MUST NOT change its state.
2. Its sequence MUST be nonzero; a sequence of zero MUST be refused with no state change.
3. In sending connection request (2), a challenge MUST derive the handshake keys from the carried session nonce and open under them. If it does not open, the client MUST refuse it, MUST count an integrity failure, and MUST NOT change its state.
4. An accepted challenge MUST derive the four connection keys from the ephemeral exchange, MUST erase the ephemeral secret, MUST build the response once, and MUST move to sending connection response (3).
5. A denial in sending connection request (2) MUST open under handshake keys derived from its carried session nonce; a denial in sending connection response (3) MUST open under the handshake server-to-client key the client already holds. If it does not open, the client MUST refuse it, MUST count an integrity failure, and MUST NOT change its state.
6. An accepted denial MUST release the connection state and MUST move to connection denied (-1).

The response of step 4 MUST be retransmitted byte for byte; a later challenge MUST NOT produce a second response.

<a id="t11.3"></a>
### t11.3. Session processing before connected

A session packet opens under the session keys from sending connection response (3) on. Every opened session packet MUST refresh the last-received time and MUST have its ack block applied. A keep-alive received in sending connection response (3) MUST store the client index and the slot count it carries and MUST move the client to connected (4). A payload received in sending connection response (3) MUST be delivered to the application. A disconnect received in sending connection response (3) MUST release the connection state and MUST move the client to disconnected (0). An application message the client sends before connected (4) is dropped and counted, as [RN-2 m1.5 Sending on a connection that is not connected](MESSAGES.md#m1.5) states.

| State | Event | Guard | Action | Next state |
|---|---|---|---|---|
| disconnected (0) | connect | token invalid | do not begin an attempt | invalid connect token (-6) |
| disconnected (0) | connect | token expired | do not begin an attempt | connect token expired (-7) |
| disconnected (0) | connect | token valid | fresh salt and ephemeral keypair; sequences at one | requesting cookie (1) |
| disconnected (0) | any other event | | ignore | disconnected (0) |
| any negative state | disconnect | | per-connection reset | disconnected (0) |
| any negative state | connect | | ignore | unchanged |
| any negative state | any other event | | ignore | unchanged |
| requesting cookie (1) | update, send due | | send request with a zero return cookie | requesting cookie (1) |
| requesting cookie (1) | cookie | size correct, salt matches | store the last 24 bytes | sending connection request (2) |
| requesting cookie (1) | cookie | size wrong or salt differs | ignore | requesting cookie (1) |
| requesting cookie (1) | timeout | no authenticated packet for the token timeout since the attempt began | | cookie request timed out (-2) |
| requesting cookie (1) | total deadline | token lifetime elapsed | | connect token expired (-7) |
| sending connection request (2) | update, send due | | send request with the stored cookie | sending connection request (2) |
| sending connection request (2) | cookie | size correct, salt matches | replace the stored cookie | sending connection request (2) |
| sending connection request (2) | cookie | size wrong or salt differs | ignore | sending connection request (2) |
| sending connection request (2) | challenge | size correct, opens | derive keys; erase the ephemeral secret; build the response | sending connection response (3) |
| sending connection request (2) | challenge | size wrong | refuse as WRONG_SIZE | sending connection request (2) |
| sending connection request (2) | challenge | sequence zero or does not open | refuse; an unopened packet counts an integrity failure | sending connection request (2) |
| sending connection request (2) | denied | size correct, opens | release the connection state | connection denied (-1) |
| sending connection request (2) | denied | size wrong | refuse as WRONG_SIZE | sending connection request (2) |
| sending connection request (2) | denied | sequence zero or does not open | refuse; an unopened packet counts an integrity failure | sending connection request (2) |
| sending connection request (2) | timeout | no authenticated packet for the token timeout since the attempt began (a cookie does not count) | | connection request timed out (-3) |
| sending connection request (2) | total deadline | token lifetime elapsed | | connect token expired (-7) |
| sending connection response (3) | update, send due | | send the stored response byte for byte | sending connection response (3) |
| sending connection response (3) | keep-alive | opens | store the client index and the slot count | connected (4) |
| sending connection response (3) | payload | opens | deliver the payload | sending connection response (3) |
| sending connection response (3) | denied | size correct, opens | release the connection state | connection denied (-1) |
| sending connection response (3) | denied | size wrong | refuse as WRONG_SIZE | sending connection response (3) |
| sending connection response (3) | denied | sequence zero or does not open | refuse; an unopened packet counts an integrity failure | sending connection response (3) |
| sending connection response (3) | disconnect | opens | release the connection state | disconnected (0) |
| sending connection response (3) | timeout | no authenticated packet for the token timeout | | connection response timed out (-4) |
| sending connection response (3) | total deadline | token lifetime elapsed | | connect token expired (-7) |
| connected (4) | update, send due | | send a keep-alive with the client index and the slot count | connected (4) |
| connected (4) | session packet | opens | refresh the last-received time; deliver a payload; apply an ack block | connected (4) |
| connected (4) | disconnect | opens | release the connection state | disconnected (0) |
| connected (4) | timeout | no packet received for the token timeout | | connection timed out (-5) |

An event a state does not list is ignored in that state with no state change. A packet that fails a guard is refused and counted, with no state change except where the table states one.

```vector t11.client-transcript
input: the frozen connect token of 768 bytes and its protocol id
input: connection salt 53 41 4c 54 30 30 30 31
input: the published client ephemeral keypair and server ephemeral keypair
input: the published session nonce 4e 4f 4e 43 45 30 30 31
input: updates at 1000.0, 1000.1, 1000.2 and 1000.31 seconds; the challenge is lost
input: bytes 1 to 710 are the first request, 711 to 1420 the request with the cookie, 1421 to 2130 the request retransmitted after the lost challenge, 2131 to 2180 the response
state after packet 1: requesting cookie (1)
state after packet 2: sending connection request (2)
state after packet 3: sending connection request (2), the challenge lost
state after packet 4: sending connection response (3)
00 00 b7 59 01 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 53 41
4c 54 30 30 30 31 3a 55 3d 74 79 2d 72 7e fa 9b
9a 4c de 3d a1 ad 93 f1 a2 d0 c0 9c b6 39 b1 a3
c0 fd a1 4c be 24 93 1a 7c 4e 88 77 66 55 44 33
22 11 00 f9 ac 68 00 00 00 00 2c fa ac 68 00 00
00 00 0a 00 00 00 01 cb 00 71 0a 40 9c 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 3d
24 90 88 9d 37 4c 29 6f ab 1d 57 70 cb 71 47 6a
06 f7 4e f2 f2 54 0c f2 21 5f 76 14 f4 80 1c b3
10 fd d3 03 d6 aa d8 c7 90 0d fc 42 52 d9 4d 21
34 f1 e6 e1 73 20 f4 c5 b1 bd 4f 28 79 da 3c e2
f2 87 72 08 61 88 73 78 ed f0 1d e9 be fc 09 f1
bf 01 fd 3d f1 9e 2a 79 e6 08 59 e8 96 34 6e b6
0c 08 f6 86 72 1d 81 94 6e 78 1e 1a e2 c3 71 6a
46 58 80 a3 a5 f0 68 ed e0 b0 39 cf 52 81 a9 7c
d3 67 17 76 6b 5b b1 38 69 83 8e cb 33 3d 9e eb
10 42 bf 7a 7b 8f 62 f9 cb b0 20 dd bd a6 cb 5f
44 e4 de f4 36 66 64 2f f8 e6 f8 35 a5 f2 95 dd
8d 7b f2 26 86 84 b2 a9 3e c1 7a c8 fd c4 c4 f0
5e 5b 09 92 7b 6a fa c3 ec 0f 00 50 8e 9d 43 31
66 00 c6 b7 45 d6 db b9 c6 8f 12 d5 69 a1 95 73
d4 60 ba 34 e8 01 d7 44 d6 db 6a 0a 01 bd 35 b4
e0 8d 52 03 bb bd 44 36 7e f8 9b cb b3 82 7f d8
02 95 f2 b9 40 4c dd 29 79 e3 aa 17 51 3f f9 f4
2b 67 06 24 8e a7 62 28 20 28 d4 fe 0c f7 11 56
55 4f 26 bf 90 cb 2b fc fe f4 38 b5 e6 8a d2 50
1f 9e 3b e2 4b c0 b3 36 51 ec 75 6f 60 f4 23 f3
24 d9 03 23 ad c9 f6 c8 f1 76 2b 02 ba 31 c7 e7
96 26 06 7f e7 f1 09 59 4a 80 3d 33 05 02 fe b0
3f 61 e9 29 7e c1 1a ad eb cd 69 ba c0 bf d0 5c
1c 5c 14 4a e2 3b cd 11 4c 35 32 2e 79 78 43 e9
5c cb 8b c9 74 8f da 08 19 7c e6 d5 a1 0c 83 bc
b0 eb 7f f4 71 d7 38 a7 42 f6 8f a3 eb 77 df ad
d3 20 a1 1a 55 42 0e 3c cd 03 f1 5e fc 18 3a 5d
d3 5c f8 65 7f a7 12 b7 c9 74 96 00 0e 6a 54 68
d8 00 d4 9d 13 45 ce 18 5d c0 f7 bc 71 47 a9 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 6b 36 92 b4 9d 36 b7 a2 76 30
33 40 ab 83 99 b9 85 61 ac 68 81 fc 7a a2 bb 60
ff e0 77 32 ea 71 21 fe 50 44 eb b5 f5 3d be 76
8b 80 0c 30 d7 53 70 eb 9b eb 49 14 ae 13 3d a6
4e a6 b0 9e 76 0d 00 00 b7 59 01 00 2c fa ac 68
00 00 00 00 0f b2 b2 8f 32 db d6 c9 eb 36 4f 57
74 86 f3 e9 53 41 4c 54 30 30 30 31 3a 55 3d 74
79 2d 72 7e fa 9b 9a 4c de 3d a1 ad 93 f1 a2 d0
c0 9c b6 39 b1 a3 c0 fd a1 4c be 24 93 1a 7c 4e
88 77 66 55 44 33 22 11 00 f9 ac 68 00 00 00 00
2c fa ac 68 00 00 00 00 0a 00 00 00 01 cb 00 71
0a 40 9c 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 3d 24 90 88 9d 37 4c 29 6f ab 1d
57 70 cb 71 47 6a 06 f7 4e f2 f2 54 0c f2 21 5f
76 14 f4 80 1c b3 10 fd d3 03 d6 aa d8 c7 90 0d
fc 42 52 d9 4d 21 34 f1 e6 e1 73 20 f4 c5 b1 bd
4f 28 79 da 3c e2 f2 87 72 08 61 88 73 78 ed f0
1d e9 be fc 09 f1 bf 01 fd 3d f1 9e 2a 79 e6 08
59 e8 96 34 6e b6 0c 08 f6 86 72 1d 81 94 6e 78
1e 1a e2 c3 71 6a 46 58 80 a3 a5 f0 68 ed e0 b0
39 cf 52 81 a9 7c d3 67 17 76 6b 5b b1 38 69 83
8e cb 33 3d 9e eb 10 42 bf 7a 7b 8f 62 f9 cb b0
20 dd bd a6 cb 5f 44 e4 de f4 36 66 64 2f f8 e6
f8 35 a5 f2 95 dd 8d 7b f2 26 86 84 b2 a9 3e c1
7a c8 fd c4 c4 f0 5e 5b 09 92 7b 6a fa c3 ec 0f
00 50 8e 9d 43 31 66 00 c6 b7 45 d6 db b9 c6 8f
12 d5 69 a1 95 73 d4 60 ba 34 e8 01 d7 44 d6 db
6a 0a 01 bd 35 b4 e0 8d 52 03 bb bd 44 36 7e f8
9b cb b3 82 7f d8 02 95 f2 b9 40 4c dd 29 79 e3
aa 17 51 3f f9 f4 2b 67 06 24 8e a7 62 28 20 28
d4 fe 0c f7 11 56 55 4f 26 bf 90 cb 2b fc fe f4
38 b5 e6 8a d2 50 1f 9e 3b e2 4b c0 b3 36 51 ec
75 6f 60 f4 23 f3 24 d9 03 23 ad c9 f6 c8 f1 76
2b 02 ba 31 c7 e7 96 26 06 7f e7 f1 09 59 4a 80
3d 33 05 02 fe b0 3f 61 e9 29 7e c1 1a ad eb cd
69 ba c0 bf d0 5c 1c 5c 14 4a e2 3b cd 11 4c 35
32 2e 79 78 43 e9 5c cb 8b c9 74 8f da 08 19 7c
e6 d5 a1 0c 83 bc b0 eb 7f f4 71 d7 38 a7 42 f6
8f a3 eb 77 df ad d3 20 a1 1a 55 42 0e 3c cd 03
f1 5e fc 18 3a 5d d3 5c f8 65 7f a7 12 b7 c9 74
96 00 0e 6a 54 68 d8 00 d4 9d 13 45 ce 18 5d c0
f7 bc 71 47 a9 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 6b 36 92 b4
9d 36 b7 a2 76 30 33 40 ab 83 99 b9 85 61 ac 68
81 fc 7a a2 bb 60 ff e0 77 32 ea 71 21 fe 50 44
eb b5 f5 3d be 76 8b 80 0c 30 d7 53 70 eb 9b eb
49 14 ae 13 3d a6 4e a6 b0 9e 76 0d 00 00 b7 59
01 00 2c fa ac 68 00 00 00 00 0f b2 b2 8f 32 db
d6 c9 eb 36 4f 57 74 86 f3 e9 53 41 4c 54 30 30
30 31 3a 55 3d 74 79 2d 72 7e fa 9b 9a 4c de 3d
a1 ad 93 f1 a2 d0 c0 9c b6 39 b1 a3 c0 fd a1 4c
be 24 93 1a 7c 4e 88 77 66 55 44 33 22 11 00 f9
ac 68 00 00 00 00 2c fa ac 68 00 00 00 00 0a 00
00 00 01 cb 00 71 0a 40 9c 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 3d 24 90 88 9d
37 4c 29 6f ab 1d 57 70 cb 71 47 6a 06 f7 4e f2
f2 54 0c f2 21 5f 76 14 f4 80 1c b3 10 fd d3 03
d6 aa d8 c7 90 0d fc 42 52 d9 4d 21 34 f1 e6 e1
73 20 f4 c5 b1 bd 4f 28 79 da 3c e2 f2 87 72 08
61 88 73 78 ed f0 1d e9 be fc 09 f1 bf 01 fd 3d
f1 9e 2a 79 e6 08 59 e8 96 34 6e b6 0c 08 f6 86
72 1d 81 94 6e 78 1e 1a e2 c3 71 6a 46 58 80 a3
a5 f0 68 ed e0 b0 39 cf 52 81 a9 7c d3 67 17 76
6b 5b b1 38 69 83 8e cb 33 3d 9e eb 10 42 bf 7a
7b 8f 62 f9 cb b0 20 dd bd a6 cb 5f 44 e4 de f4
36 66 64 2f f8 e6 f8 35 a5 f2 95 dd 8d 7b f2 26
86 84 b2 a9 3e c1 7a c8 fd c4 c4 f0 5e 5b 09 92
7b 6a fa c3 ec 0f 00 50 8e 9d 43 31 66 00 c6 b7
45 d6 db b9 c6 8f 12 d5 69 a1 95 73 d4 60 ba 34
e8 01 d7 44 d6 db 6a 0a 01 bd 35 b4 e0 8d 52 03
bb bd 44 36 7e f8 9b cb b3 82 7f d8 02 95 f2 b9
40 4c dd 29 79 e3 aa 17 51 3f f9 f4 2b 67 06 24
8e a7 62 28 20 28 d4 fe 0c f7 11 56 55 4f 26 bf
90 cb 2b fc fe f4 38 b5 e6 8a d2 50 1f 9e 3b e2
4b c0 b3 36 51 ec 75 6f 60 f4 23 f3 24 d9 03 23
ad c9 f6 c8 f1 76 2b 02 ba 31 c7 e7 96 26 06 7f
e7 f1 09 59 4a 80 3d 33 05 02 fe b0 3f 61 e9 29
7e c1 1a ad eb cd 69 ba c0 bf d0 5c 1c 5c 14 4a
e2 3b cd 11 4c 35 32 2e 79 78 43 e9 5c cb 8b c9
74 8f da 08 19 7c e6 d5 a1 0c 83 bc b0 eb 7f f4
71 d7 38 a7 42 f6 8f a3 eb 77 df ad d3 20 a1 1a
55 42 0e 3c cd 03 f1 5e fc 18 3a 5d d3 5c f8 65
7f a7 12 b7 c9 74 96 00 0e 6a 54 68 d8 00 d4 9d
13 45 ce 18 5d c0 f7 bc 71 47 a9 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 6b 36 92 b4 9d 36 b7 a2 76 30 33 40 ab 83
99 b9 85 61 ac 68 81 fc 7a a2 bb 60 ff e0 77 32
ea 71 21 fe 50 44 eb b5 f5 3d be 76 8b 80 0c 30
d7 53 70 eb 9b eb 49 14 ae 13 3d a6 4e a6 b0 9e
76 0d 03 00 b7 59 01 00 01 00 00 00 9c 08 60 98
96 b9 d4 05 d5 14 9e 1e d9 da e4 dc 2b d2 4a e4
cd a1 93 10 5c bf a9 ce 74 61 b3 97 ce 4b 50 c0
1b 2f 8f 7e
```
The cookie moves the client from requesting cookie (1) to sending connection request (2); the lost challenge leaves it in sending connection request (2) until the retransmitted request draws the same challenge again; the accepted challenge moves it to sending connection response (3); the server's keep-alive moves it to connected (4).

<a id="t12"></a>
## t12. Handshake: server

The server turns a connection request into an encryption mapping, and a connection response into a client slot. It answers only traffic that has cleared the rung that provoked the answer.

1. On a connection request, a server MUST apply the checks below in the order given, and MUST stop at the first that refuses.
2. A server MUST ignore a request whose size is not the request wire size, counting `CONNECTION_REQUEST_WRONG_SIZE`.
3. A server MUST then ignore a request from a source address that is already connected, counting `CONNECTION_REQUEST_ALREADY_CONNECTED`. A request from a source address that holds a mapping without a connected slot MUST be answered with that mapping's stored connection challenge packet, byte-identical, regardless of the request's connection salt, counting `CONNECTION_REQUEST_CHALLENGE_RESENT`, and it MUST NOT create a second mapping or derive new keys for that address. Only a request whose connection salt matches the mapping's refreshes the mapping's half-open idle deadline ([t17 Timing and defaults](#t17) rule 4).
4. A server MUST then ignore a request whose version is not the protocol version, and then one whose protocol id is not the server's.
5. A server MUST then refuse a token whose expire timestamp is not later than the server's current time, counting `CONNECTION_REQUEST_TOKEN_EXPIRED`. When the configured maximum token lifetime is positive, it MUST also refuse a token whose create timestamp is more than the creation skew after the server's current time, counting `CONNECTION_REQUEST_TOKEN_CREATED_IN_FUTURE`, and one whose lifetime exceeds the configured maximum token lifetime, counting `CONNECTION_REQUEST_TOKEN_LIFETIME_TOO_LONG`. Such a token MUST NOT be answered.
6. A server MUST then ignore a request whose timeout is zero or negative, then one whose address flags name no family or set a bit above bit 1, then one whose token address of the family the request arrived on (a v4-mapped source is IPv4) is not one this server serves, the token's other-family address unread, and then one whose server public key is not this server's. It MUST NOT answer a request that rules 2, 4, 5 or 6 refuse.
7. A server MUST require a valid return cookie before any asymmetric work, and MUST answer a request without one, with a forged one, or with an expired one with a fresh connection cookie packet.
8. A server MUST refuse, unanswered, a request whose cookie is valid when the per-update handshake work budget is spent, counting `CONNECTION_REQUEST_HANDSHAKE_BUDGET_EXCEEDED`.
9. A server MUST refuse, unanswered, a request whose token identifier is bound to a different source address, and a request whose token identifier carries a live request claim held by a different source address, counting `CONNECTION_REQUEST_TOKEN_ALREADY_USED`. The first valid request records the claim for its source address before any signature, seal or keypair work; the claim ends at the handshake deadline or when a response binds the token ([t17 Timing and defaults](#t17) item 13). A server MUST also refuse, unanswered, a request that finds no free encryption mapping slot before the signature, seal and keypair rungs run, counting `CONNECTION_REQUEST_COULD_NOT_ADD_MAPPING` and `MAPPING_TABLE_FULL`. Once the failed-signature budget of the current window is spent, a server MUST refuse a request from a source with a recorded failure in that window, counting `CONNECTION_REQUEST_SIGNATURE_VERIFY_LIMIT`. A source with no recorded failure in the window is not refused by that budget ([t17 Timing and defaults](#t17) item 12).
10. A server MUST verify the signed connect data before opening the sealed private block, MUST open it only with the keypair its public key names, and MUST ignore a request whose sealed fields disagree with the signed fields or whose client id already holds a slot.
11. A server MUST deny, with a connection denied packet sealed under the handshake server-to-client key, a request that finds no free slot, and MUST NOT create a mapping for it.
12. A server MUST generate a fresh session nonce and ephemeral keypair for each mapping, MUST derive the connection keys, MUST erase the ephemeral secret, and MUST store the connection challenge in the mapping so that a repeat is byte-identical.
13. A connection response MUST be processed only after it has opened under the mapping's handshake client-to-server key. A server MUST bind the token identifier to the source address at that point, MUST ignore a response when the source address or the client id already holds a slot, MUST deny one that finds no free slot, and otherwise MUST assign a slot, mark the client unconfirmed, and send the first keep-alive.
14. A mapping without a connected slot MUST be removed at its handshake deadline, which is fixed at creation and is never refreshed, or at the half-open idle deadline, which a request carrying the mapping's connection salt or a response that opens under the mapping's handshake key refreshes; whichever comes first. A connected slot MUST be removed at its idle timeout. A server MUST count these removals apart.

An application message the server sends to a slot that is not connected is dropped and counted, as [RN-2 m1.5 Sending on a connection that is not connected](MESSAGES.md#m1.5) states.

The policy values of this section are below. A value of `0` for the maximum token lifetime selects no limit.

| Name | Value | Unit |
|---|---|---|
| token creation skew | `5` | seconds |
| maximum token lifetime | `0` | seconds |
| failed-signature source table | `64` | sources |

The cookie lifetime, the cookie key rotation period and the used-token history
capacity per slot are rows of [t3 Constants and registries](#t3). The handshake
deadline is the server configuration field of
[t2.3 Configuration surface](#t2.3). The handshake work budget, the
failed-signature budget and window, and the half-open idle deadline are rows of
[t17 Timing and defaults](#t17).

The states of one source address are below. An event that a state does not
list is ignored in that state.

| State | Event | Guard | Action | Next state |
|---|---|---|---|---|
| no mapping | request | size, version or protocol id check fails | ignore, count the reason | no mapping |
| no mapping | request | token expired | ignore, count | no mapping |
| no mapping | request | token created in the future | ignore, count | no mapping |
| no mapping | request | token lifetime too long | ignore, count | no mapping |
| no mapping | request | timeout, address flags, advertised address or server key check fails | ignore, count the reason | no mapping |
| no mapping | request | cookie absent, forged or expired | send a fresh cookie packet | no mapping |
| no mapping | request | handshake work budget spent | ignore, count | no mapping |
| no mapping | request | token used from another address | ignore, count | no mapping |
| no mapping | request | token claimed from another address until the handshake deadline | ignore, count | no mapping |
| no mapping | request | encryption mapping table full | ignore, count | no mapping |
| no mapping | request | failed-signature budget spent and the source has a recorded failure in the window | ignore, count | no mapping |
| no mapping | request | signature, seal or sealed-field check fails | ignore, count | no mapping |
| no mapping | request | client id connected | ignore, count | no mapping |
| no mapping | request | no free slot | send connection denied | no mapping |
| no mapping | request | all checks pass | create mapping, derive keys, send challenge | mapping |
| mapping | request | salt matches the mapping's | resend the stored challenge, refresh idle | mapping |
| mapping | request | salt differs from the mapping's | resend the stored challenge | mapping |
| mapping | response | mapping has no connected slot, source or client id already connected | ignore, count | mapping |
| mapping | response | no free slot | send connection denied | mapping |
| mapping | response | free slot | bind token, assign slot, send keep-alive | connected |
| mapping | handshake deadline reached | never refreshed | remove mapping, count | no mapping |
| mapping | idle deadline reached | no matching request inside the deadline | remove mapping, count | no mapping |
| connected | request | any | ignore, count | connected |
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




```vector t12.token-expired
input: the request of [t8 Packet formats](#t8)
input: signed create timestamp 1756166300, expire 1756166350, server time 1756166400
input: the return cookie is valid for the source and connection salt
00 00 b7 59 01 00 03 f9 ac 68 00 00 00 00 0f b2
b2 8f 32 db d6 c9 eb 36 4f 57 74 86 f3 e9 53 41
4c 54 30 30 30 31 3a 55 3d 74 79 2d 72 7e fa 9b
9a 4c de 3d a1 ad 93 f1 a2 d0 c0 9c b6 39 b1 a3
c0 fd a1 4c be 24 93 1a 7c 4e 88 77 66 55 44 33
22 11 9c f8 ac 68 00 00 00 00 ce f8 ac 68 00 00
00 00 0a 00 00 00 01 cb 00 71 0a 40 9c 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 3d
24 90 88 9d 37 4c 29 6f ab 1d 57 70 cb 71 47 6a
06 f7 4e f2 f2 54 0c f2 21 5f 76 14 f4 80 1c 34
e4 2d 4a f5 ef 94 a0 7a 3a 84 20 1b 88 9d 4c d1
a7 43 cb 27 b1 1b 6a 10 43 8a 8f eb 8e 58 47 08
df eb b3 86 fe 9d 32 82 32 59 14 c7 92 00 5b 3f
0c bd b6 0f 45 ee cb e3 8e a0 a7 2c ec 1b 6b a0
e1 4a fd 6e fb 43 6d 00 34 5c 40 4c 4d e3 92 57
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
00 00 00 00 00 00 b2 89 10 2e ed 08 cc 04 fa f6
3d b4 bc fe 50 bd 70 e8 ef 89 80 a3 75 9f 79 f5
5d 90 ef 7c 76 82 25 64 c4 01 68 1b 65 ff 37 81
c6 cb 4d 2b e9 6e 69 da dc 39 95 9d d6 7f 85 ea
f8 24 2b 06 9d 06
```


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

A keep-alive is a sealed session packet of type `connection keep-alive packet` ([t3 Constants and registries](#t3)). Its type data names the client
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
| client slot count | 4 | 4 | u32 | the server's configured number of client slots | 1 to the `maximum client slot count` row of [t3 Constants and registries](#t3) |

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
The server MUST refuse a connection request whose timeout is zero or
negative, and MUST clamp a positive timeout to the `server max_timeout_seconds`
row of [t2.3 Configuration surface](#t2.3). A client MUST refuse a token whose
timeout is zero or negative before it sends any packet
([t11 Handshake: client](#t11)). A positive timeout is the interval; no
timeout value disables it.

A server whose connection times out MUST reset its slot
([t13.4 Slot reset](#t13.4)) and MUST destroy the connection's encryption
mapping. A client whose connection times out MUST enter `connection timed out` and MUST release its connection state.

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

A client MUST send the burst only when it holds session keys: from `sending connection response` and `connected`. Outside those states it releases its
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

Each endpoint keeps one replay window per direction and key phase. A replay
window is a high-water mark and a ring of received bits, one bit per sequence.
The window depth is at least the `replay window depth minimum` and at most the
`replay window depth maximum` (both rows of
[t3 Constants and registries](#t3)). The depth is a count of sequences, never a
time span or a packet rate. The message layer reconstructs 24-bit numbers
against a reference and stays unambiguous only while a stale copy is bounded
by this window.

A receiver MUST perform these steps on every session packet, in this order:

1. Reconstruct the packet's 64-bit sequence from the 32-bit wire value and the
   window's high-water mark.
2. Refuse the packet without opening it when the high-water mark is at least
   one window depth and the sequence is at or below the high-water mark minus
   the window depth.
3. Refuse the packet without opening it when the sequence is at or below the
   high-water mark and its received bit is set.
4. Open the sealed packet.
5. Mark the sequence received only after the open succeeds; a packet that does
   not open MUST NOT change the window.

Marking a sequence ahead of the high-water mark clears every received bit
between the old high-water mark and the sequence, then sets the sequence's bit
and raises the high-water mark to it. When that forward jump is at least one
window depth, the receiver MUST clear the whole ring.

A receiver MUST apply the replay window to the five session packet types:
connection keep-alive, connection payload, connection disconnect, connection
ack and connection payload fragment. Handshake packet types are outside the
window. A server MUST NOT move the window of a session packet whose source
address has no assigned client slot: it ignores the packet before decryption,
as [t10 Receive processing](#t10) step 12 states, and does not mark it.

Each endpoint MUST count, per connection and across all of that connection's
keys, every sealed packet that fails to open, and MUST disconnect the
connection on reaching the `integrity limit`.

An implementation MAY drop session packets before decryption in a kernel
prefilter that holds, per source address and port, a high-water mark written
only by the endpoint after a packet opens. Such a prefilter MUST NOT drop a
packet fewer than the `kernel replay window` sequences behind that high-water
mark, and it MAY drop any packet at or beyond that distance. The prefilter
never advances the mark and never accepts a packet the window would refuse.

| Name | Value | Unit | Kind | Meaning |
|---|---|---|---|---|
| integrity limit | `68719476736` | sealed packets | protocol value | failed opens that end a connection |
| kernel replay window | `16` | sequences | protocol value | distance behind the kernel high-water mark that a prefilter may drop |

| State | Event | Guard | Action | Next state |
|---|---|---|---|---|
| fresh | sequence received | none | open; set the bit; high-water mark = sequence | tracking |
| tracking | sequence received | high-water mark at least one depth and sequence at or below high-water mark minus depth | refuse; no change | tracking |
| tracking | sequence received | received bit set | refuse; no change | tracking |
| tracking | sequence received | received bit clear and sequence at or below high-water mark | set the bit; no change to the high-water mark | tracking |
| tracking | sequence received | sequence above high-water mark | clear the skipped bits, the whole ring when the jump is at least one depth; set the bit; high-water mark = sequence | tracking |
| any | open fails | none | no mark; count one integrity failure | unchanged |

A replay vector's bytes are the window depth as a 4-byte little-endian
unsigned integer, the final high-water mark as an 8-byte little-endian
unsigned integer, then one byte per probe in input order: `01` when the probe
is accepted, `00` when it is refused. A probe is accepted when its sequence is
not behind the high-water mark and its received bit is clear.

```vector t14.in-window-duplicate
input: from an empty window, mark sequences 1000 and 1010 received
input: probe sequences 1000, 1005, 1010 and 1011 in that order
00 10 00 00 f2 03 00 00 00 00 00 00 00 01 00 01
```

```vector t14.behind-window
input: from an empty window, mark sequence 4140 received
input: probe sequences 44, 45 and 4140 in that order
00 10 00 00 2c 10 00 00 00 00 00 00 00 01 00
```

```vector t14.forward-jump-clears
input: from an empty window, mark sequences 1 and 4097 received
input: probe sequences 4097, 1 and 2 in that order
00 10 00 00 01 10 00 00 00 00 00 00 00 00 01
```

```vector t14.window-maximum
input: a window of depth 32768, from empty, mark sequences 1 and 32769 received
input: probe sequences 32769, 1 and 2 in that order
00 80 00 00 01 80 00 00 00 00 00 00 00 00 01
```

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

An endpoint MUST send an acknowledgement block on its next protocol update
whenever acknowledgement debt is nonzero. If no other session packet carries
the block on that update, it MUST send a standalone connection ack packet. A sender MUST clear
the debt when it sends any session packet that carries a block. A standalone
acknowledgement contains the block and no packet-type data. A type 8 packet
does not add debt, so acknowledgements do not elicit acknowledgements.

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
reassembly slots, the max_fragmented_messages field of
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
5. A client MUST NOT begin an attempt with a token whose expire timestamp is
   not later than its current time, and MUST send no packet for it. In
   requesting cookie (1), sending connection request (2) and sending connection
   response (3) the client MUST abandon the attempt for connect token expired
   (-7) once the token's lifetime, its expire timestamp less its create
   timestamp, has elapsed on the client's clock since the attempt began. The
   check does not apply in connected (4).
6. An endpoint MUST end an established connection when the connection timeout
   has elapsed with no packet received from the peer. The connection timeout is
   the timeout seconds field of the connect token
   ([t6 Connect token](#t6)); a token whose timeout is zero or negative is
   refused at both ends, so every connection times out
   ([t13.2 Timeout](#t13.2)).
7. A server MUST accept a connection cookie for the cookie lifetime of
   [t3 Constants and registries](#t3). After the cookie key rotates, the server
   MUST accept a cookie minted under the previous key for one cookie key
   rotation period of [t3 Constants and registries](#t3) and no longer.
8. An endpoint MUST send the redundant disconnect packet count of
   [t3 Constants and registries](#t3) on a deliberate disconnect.
9. An endpoint MUST send an acknowledgement block on its next update when
   ack debt is nonzero, using a standalone ack if no other session packet
   carries it, and MUST NOT wait for a packet threshold or keep-alive cadence.
10. A reliable-message sender MUST retransmit an unacknowledged message only
    when its adaptive resend delay expires. Before any RTT
    sample, the resend delay is 2 seconds by default. For the first RTT sample, the
    sender MUST set both its smoothed RTT and deviation to that sample. For
    each later sample, deviation MUST move 5 percent toward the absolute
    difference between the prior smoothed RTT and the sample, then smoothed
    RTT MUST move 5 percent toward the sample.
    The resend delay is 2 * smoothed_RTT + 4 * deviation + 30 ms, capped by the
    configured maximum (2 seconds by default) and raised to the configured
    minimum (0 by default). A nonzero minimum is an implementation option.
    The sender MUST NOT back off the resend delay after retransmission.
11. A server SHOULD enforce the handshake work budget after the cookie of a
    connection request validates and before any asymmetric operation. When it
    does, a request past the budget MUST be refused with no reply and counted
    as a refusal, and the budget MUST be reset at the start of each update.
12. A server MUST apply the failed-signature budget before the signature
    verification. A request from a source that has already
    failed in the current failed-signature window MUST be refused before the
    verification runs and counted as a refusal. The budget MUST NOT refuse a
    request from a source with no recorded failure in the window, even when
    the budget is spent and the server can record no further sources; the
    verifications such requests cost are bounded by the handshake work budget
    of item 11.
 13. A server MUST refuse a request whose token identifier is claimed by a
     different source address, and one that finds no free
     encryption mapping slot, before any signature, seal or keypair work. The
     first valid request for a token records its claim for the source address;
     the claim ends at the handshake deadline or when a response binds the
     token. A full mapping table refuses with no reply and is counted as a
     refusal. The request-claim table and the response-binding table are
     independent capacities: claim pressure cannot consume or evict a still
     live response binding.

The values this section introduces are:

| Name | Value | Unit | Meaning |
|---|---|---|---|
| half-open idle deadline | `10` | seconds | a half-open mapping with no matching request from its address for this long is discarded |
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
0a 00 00 00 00 00 00 00 01 00 00 00 00 00 00 00
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
| `CLIENT_SEND_DEFERRED` | `expected` | `none` |
| `XDP_KERNEL_DROPPED_REPLAYED` | `refusal` | `none` |
| `CLIENT_CHALLENGE_WRONG_SIZE` | `refusal` | `CLIENT_CHALLENGE_RECEIVED` |
| `CLIENT_DENIED_WRONG_SIZE` | `refusal` | `CLIENT_DENIED_RECEIVED` |
| `CONNECTION_REQUEST_TOKEN_CREATED_IN_FUTURE` | `refusal` | `CONNECTION_REQUEST_RECEIVED` |
| `CONNECTION_REQUEST_TOKEN_LIFETIME_TOO_LONG` | `refusal` | `CONNECTION_REQUEST_RECEIVED` |
| `SEND_NO_LINK_ADDRESS` | `refusal` | `none` |
| `MESSAGE_PROMOTED_UNRELIABLE` | `expected` | `none` |
| `MESSAGE_PROMOTED_UNRELIABLE_SEQUENCED` | `expected` | `none` |
| `MESSAGE_PROMOTED_UNRELIABLE_WITH_ACK_RECEIPT` | `expected` | `none` |
| `MESSAGE_UNRELIABLE_IN_PLACE_FALLBACK` | `expected` | `none` |
| `SEND_FAILED` | `refusal` | `none` |
| `XDP_KERNEL_PASSED_FRAGMENT` | `expected` | `none` |
| `XDP_KERNEL_DROPPED_FIRST_FRAGMENT` | `refusal` | `none` |
| `MESSAGE_SEND_NOT_CONNECTED` | `expected` | `none` |
| `MESSAGE_BAD_FORM` | `refusal` | `none` |
| `MESSAGE_TRUNCATED` | `refusal` | `none` |
| `MESSAGE_BAD_FLAGS` | `refusal` | `none` |
| `MESSAGE_BAD_LENGTH` | `refusal` | `none` |
| `MESSAGE_BAD_SIZE` | `refusal` | `none` |
| `MESSAGE_BAD_CONTROL` | `refusal` | `none` |
| `MESSAGE_NUMBER_OUTSIDE_WINDOW` | `refusal` | `none` |
| `MESSAGE_SLOT_COLLISION` | `refusal` | `none` |
| `MESSAGE_DUPLICATE_ORDERING` | `refusal` | `none` |
| `XDP_KERNEL_DROPPED_NO_SOCKET` | `refusal` | `none` |
| `CLIENT_RECEIVE_FOREIGN_SOURCE` | `refusal` | `none` |

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
00 c2 2f db 2c 00 21 00 6b c6 a4 41 02 00 00 88
ed b1 56 00 00 00 d6 92 db 95 00 00 00 d7 6a 4d
f9 00 00 00 eb 03 7f 49 00 00 00 11 1a 37 e1 00
00 00 9c 47 5e e7 00 00 00 8c 5c 05 26 01 00 00
44 5b fe 0f 00 00 00 ff 64 f7 fa 01 00 00 75 4f
7d 3a 00 00 00 b1 eb 07 8c 00 00 00 e9 53 9e 77
00 00 00 b9 17 2c 07 00 00 00 34 b4 85 d8 00 00
00 b0 cb 42 af 01 00 00 bd b0 38 5c 01 00 00 9f
8d 1d fd 00 00 00 be a9 a8 b8 00 4e 00 22 20 3f
b1 00 54 00 6a c0 70 be 00 08 00 17 6f 7f 78 00
08 00 8b 6d 57 dc 00 00 00 bf 7a 1a af 01 00 00
9b 86 1b 95 01 00 00 79 87 fe 4c 01 00 00 f9 84
f0 6e 01 00 00 25 11 95 dc 00 00 00 df ff a1 97
01 00 00 7e 01 4f a1 00 00 00 1d 39 f1 61 01 00
00 ab 9f e2 5c 00 00 00 7d 98 4f 4e 00 00 00 58
b6 0b 73 00 00 00 f9 25 ca 24 00 00 00 50 9a 50
f5 00 00 00 0a 04 26 b7 00 00 00 dd 1a 04 77 00
00 00 ee 4e 50 1b 00 00 00 37 af 52 db 00 00 00
7a 31 93 22 00 00 00 79 b8 15 e7 00 00 00
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

The published development key is a fixed backend signing key. Its public half
is `ROCKETNET_DEV_BACKEND_PUBLIC_KEY`. It is public: anyone who holds it can
forge a token a server accepts. It exists so a first wiring works without a
backend, and it MUST NOT be load-bearing in production. A server still
generates its own keypair, and a development token still needs a nonzero
protocol id.

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
| Token binding | A token is bound to a server address and an expiry. | A token is also bound to the first source address that presents a valid request, until the handshake deadline, and then to the source address whose connection response first opened under it; it admits one live connection ([t12 Handshake: server](#t12)). |
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

A server assumes a path MTU of at least 1500 bytes and no packet it accepts
needs IP fragmentation, so an IP fragment addressed to it is hostile or
misconfigured. A kernel path that sees IP fragments drops the first fragment
of a datagram addressed to the server's address and port (the fragment at
offset zero with more fragments to follow, carrying the UDP header) and counts
it in `XDP_KERNEL_DROPPED_FIRST_FRAGMENT`, before any size, epoch, filter or
replay check. Every other fragment, and every fragment whose UDP header it
cannot read, it hands to the host stack and counts in
`XDP_KERNEL_PASSED_FRAGMENT`: without its first fragment the host never
reassembles the datagram, and fragments for other services are untouched.

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


### Create-time maximum-resend delay message reserve

Each connection adds a local payload reserve to its configured message-region
headroom at CREATE. Let `R = ceil(send_kbps * 125 * message_resend_delay_max_ms / 1000)` bytes.
Client and server creation use their respective resolved per-connection budgets,
including an explicit creation override. Generic connection sizing conservatively
uses the larger configured budget. An unlimited budget (`ROCKETNET_SEND_KBPS_UNLIMITED`) has no finite reserve.

The descriptor pool adds `ceil(R / 128)` entries. The 128-, 512-, and 1152-byte
fixed pools add `ceil(R / 128)`, `ceil(R / 512)`, and `ceil(R / 1024)` chunks,
respectively, including each chunk's four-byte link and alignment. The send-span
arena adds R plus up to 15 padding bytes per 1153 payload bytes, rounded to 64.
Receive-ring and reliable-window wire parameters do not change. This extends the
base headroom formula and default sizes above; runtime sizing reports
the resulting total. All parts remain in the single allocator-hook allocation.

The capacity guarantee assumes typical payload sizes of 128/512/1024 bytes in
the corresponding fixed pools, or larger messages in the span arena. Arbitrarily
tiny records can exhaust bounded descriptors or padding capacity; they retain
MESSAGE_MEMORY_EXHAUSTED and disconnect behavior. Raising the runtime send budget
after CREATE does not enlarge this allocation; configure the intended maximum
budget before creation.
