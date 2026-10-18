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
3. A server MUST ignore a request from a source address that is already connected. A request from a source address that holds a mapping without a connected slot MUST be answered with that mapping's stored connection challenge packet, byte-identical, regardless of the request's connection salt, and it MUST NOT create a second mapping or derive new keys for that address.
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
| mapping | request | source connected | resend the stored challenge | mapping |
| mapping | request | source not connected | resend the stored challenge, refresh idle | mapping |
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
| [t12 Handshake: server](#t12) | `t12.challenge` |
| [t12 Handshake: server](#t12) | `t12.cookie` |
| [t12 Handshake: server](#t12) | `t12.keep-alive` |
| [t12 Handshake: server](#t12) | `t12.stale-cookie` |
| [t12 Handshake: server](#t12) | `t12.token-created-in-future` |
| [t12 Handshake: server](#t12) | `t12.token-lifetime-too-long` |
| [t16 Fragmentation and reassembly](#t16) | `t16.reassembly-trace` |
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
