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

The client is in exactly one of the states of the table below. A state number is signed: a negative state is how the last attempt or connection ended, and a non-negative state is a state the client acts in.

1. A client MUST be created in disconnected (0) and MUST hold no connect token at creation. A connect token MUST be accepted only in disconnected (0). In every other state, a connect token MUST be ignored and the state MUST NOT change.
2. A connect token accepted in disconnected (0) MUST be checked before any packet is sent. The token is invalid when its address flags name no family or set any bit above the two family flags, when its create timestamp is later than its expire timestamp, or when its version is not the version of [t3 Constants and registries](#t3). An invalid token MUST put the client in invalid connect token (-6). A token whose expire timestamp is not later than the current time MUST put the client in connect token expired (-7). Neither check sends a packet.
3. An accepted token MUST select one server address for the whole attempt: the IPv6 address when its flag is set, otherwise the IPv4 address.
4. An attempt MUST begin with a fresh connection salt and a fresh ephemeral keypair. The handshake and session sending sequences MUST start at one and MUST NOT be reset within the attempt.
5. The client MUST enter requesting cookie (1), sending connection request (2), sending connection response (3) and connected (4) in that order, each at most once per attempt.
6. While the client is in requesting cookie (1), sending connection request (2) or sending connection response (3), it MUST abandon the attempt for connect token expired (-7) once the token's lifetime, the difference between its expire timestamp and its create timestamp, has elapsed since the attempt began. The check does not apply in connected (4).
7. In requesting cookie (1), sending connection request (2) and sending connection response (3), the client MUST retransmit its current packet at the send rate of [t3 Constants and registries](#t3) until a packet advances it or a deadline ends the attempt. A client MUST NOT send a packet because a packet arrived.
8. In a positive state below connected (4), a client that has received no packet for the token's timeout MUST move to the state whose number is one less than the negation of its current state: requesting cookie (1) to cookie request timed out (-2), sending connection request (2) to connection request timed out (-3), sending connection response (3) to connection response timed out (-4). In connected (4) the same silence MUST move the client to connection timed out (-5).
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

A connection cookie packet MUST be processed in requesting cookie (1) and sending connection request (2) and MUST be ignored in every other state. A client MUST refuse a packet whose size is not the cookie packet size of [t8 Packet formats](#t8), and MUST ignore one whose echoed connection salt is not the attempt's salt. A client that accepts a cookie MUST store its last 24 bytes, replacing any cookie already stored, and MUST send nothing in response. In requesting cookie (1) the accepted cookie MUST move the client to sending connection request (2); in sending connection request (2) the state MUST NOT change.

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

A session packet opens under the session keys from sending connection response (3) on. Every opened session packet MUST refresh the last-received time and MUST have its ack block applied. A keep-alive received in sending connection response (3) MUST store the client index and the slot count it carries and MUST move the client to connected (4). A payload received in sending connection response (3) MUST be delivered to the application. A disconnect received in sending connection response (3) MUST release the connection state and MUST move the client to disconnected (0).

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
| requesting cookie (1) | timeout | no packet received for the token timeout | | cookie request timed out (-2) |
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
| sending connection request (2) | timeout | no packet received for the token timeout | | connection request timed out (-3) |
| sending connection request (2) | total deadline | token lifetime elapsed | | connect token expired (-7) |
| sending connection response (3) | update, send due | | send the stored response byte for byte | sending connection response (3) |
| sending connection response (3) | keep-alive | opens | store the client index and the slot count | connected (4) |
| sending connection response (3) | payload | opens | deliver the payload | sending connection response (3) |
| sending connection response (3) | denied | size correct, opens | release the connection state | connection denied (-1) |
| sending connection response (3) | denied | size wrong | refuse as WRONG_SIZE | sending connection response (3) |
| sending connection response (3) | denied | sequence zero or does not open | refuse; an unopened packet counts an integrity failure | sending connection response (3) |
| sending connection response (3) | disconnect | opens | release the connection state | disconnected (0) |
| sending connection response (3) | timeout | no packet received for the token timeout | | connection response timed out (-4) |
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
| [t11 Handshake: client](#t11) | `t11.client-transcript` |
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
