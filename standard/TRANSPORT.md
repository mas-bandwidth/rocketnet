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
does not subtract peer processing or acknowledgement aggregation delay. The reported RTT MUST be the minimum strictly positive retained first-ack
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
| [t15 Acknowledgement](#t15) | `t15.fresh` |
| [t15 Acknowledgement](#t15) | `t15.fully-received` |
| [t15 Acknowledgement](#t15) | `t15.invalid-first-too-long` |
| [t15 Acknowledgement](#t15) | `t15.invalid-fresh-first` |
| [t15 Acknowledgement](#t15) | `t15.invalid-fresh-range` |
| [t15 Acknowledgement](#t15) | `t15.invalid-geometry` |
| [t15 Acknowledgement](#t15) | `t15.invalid-short` |
| [t15 Acknowledgement](#t15) | `t15.invalid-walk` |
| [t15 Acknowledgement](#t15) | `t15.invalid-zero-first` |
| [t15 Acknowledgement](#t15) | `t15.invalid-zero-gap` |
| [t15 Acknowledgement](#t15) | `t15.invalid-zero-length` |
| [t15 Acknowledgement](#t15) | `t15.one-loss` |
| [t15 Acknowledgement](#t15) | `t15.sequence-wrap` |
| [t15 Acknowledgement](#t15) | `t15.truncated-oldest` |
| [t15 Acknowledgement](#t15) | `t15.two-losses` |
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
