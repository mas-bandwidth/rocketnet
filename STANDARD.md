# rocketnet 1.0 (draft)

**rocketnet** is a simple protocol for creating secure client/server connections over UDP.

This document describes the standard for this protocol so people can create their own implementations.

It is deliberately equivalent in shape to the netcode 1.02 standard, with two differences that run through everything:

1. All cryptography is [libhydrogen](https://github.com/jedisct1/libhydrogen), not libsodium. Hydrogen builds in kernel, which lets conforming servers move validation into XDP programs.

2. There is no private key shared between the backend and the dedicated servers. The backend signs connect tokens with a private key that never leaves the backend, and seals per-server secrets to each server's own public key. A compromised server cannot mint tokens, and leaks only its own sessions.

## Architecture

There are three main components in the rocketnet architecture:

1. The backend
2. Dedicated servers
3. Clients

The backend is whatever service authenticates players and decides who plays where — a web server, a matchmaker, a game's own service. Clients are endpoints running the rocketnet protocol that want to connect to dedicated server instances. Dedicated servers are instances of the server-side portion of the game running in data centers, in the cloud, or on community-hosted machines.

The sequence of operations for a client connect are:

1. A client authenticates with the backend
2. The authenticated client requests to play via the backend
3. The backend generates a _connect token_ and returns it to that client over a secure channel (for example HTTPS)
4. The client uses the connect token to establish a connection with a dedicated server over UDP
5. The dedicated server runs logic to ensure that only clients with a valid connect token can connect to it
6. Once a connection is established, the client and server exchange sealed (encrypted and authenticated) UDP packets

## Keys

Every party holds only keys whose exposure it can afford:

* The **backend** holds a long-term signing keypair (`hydro_sign`). The private half never leaves the backend. Its public half is distributed to every server as configuration.

* Each **server** holds its own key exchange keypair (`hydro_kx`). The public half is registered with the backend when the server is provisioned; how is outside the scope of this standard. The private half never leaves the server, and its exposure compromises only sessions sealed to that one server.

* Each **client** holds only the per-session keys issued to it inside a connect token.

There is no key in the system that is both shared and secret. This is the load-bearing difference from netcode 1.02, whose fleet-wide shared private key made connect tokens unsafe on hardware the backend operator does not control.

## General Conventions

**rocketnet** is a binary protocol.

All data is written in little-endian byte order unless otherwise specified.

All hydrogen operations use an 8-byte context. The contexts used by this standard are:

    "rkt-sign"      connect token signature (hydro_sign)
    "rkt-tokn"      private connect token sealing (hydro_secretbox, under kx-derived key)
    "rkt-chal"      challenge token sealing (hydro_secretbox)
    "rkt-pack"      packet sealing (hydro_secretbox)

Where this document says a buffer is _sealed_, it means `hydro_secretbox_encrypt` with the stated key, context, and message id. A sealed buffer is `hydro_secretbox_HEADERBYTES` (36 bytes) longer than its plaintext, and fails to open if any byte of it, its message id, or its context is wrong. Sealing provides both encryption and authentication; there is no separate MAC field anywhere in this protocol.

## The Prefilter Header

Every rocketnet UDP packet begins with the same 18-byte prefilter header:

    [packet type] (uint8)
    [pittle] (2 bytes)
    [chonkle] (15 bytes)

The prefilter header is not cryptography. It exists so that servers — in particular kernel XDP programs in front of servers — can reject garbage and off-path noise at line rate before any cryptographic work happens. It is generated and checked as follows.

_pittle_ is a 2-byte value computed from a 16-bit sum over the packet's source address bytes, destination address bytes, and payload length:

    sum = 0
    <for each byte b of from address (4 bytes IPv4 + 2 bytes port)> sum += b
    <for each byte b of to address (4 bytes IPv4 + 2 bytes port)> sum += b
    sum += ( payload_bytes >> 8 )
    sum += ( payload_bytes & 0xFF )
    pittle[0] = 1 | ( ( sum & 0xFF ) ^ ( sum >> 8 ) ^ 193 )
    pittle[1] = 1 | ( ( 255 - pittle[0] ) ^ 113 )

_chonkle_ is 15 bytes derived from a 64-bit FNV-1a hash over an 8-byte magic value, the from address, the to address, and the payload length, with each output byte constrained to a fixed range so that a filter can validate structure byte by byte. The derivation of the 15 bytes from the hash is normative and matches the reference implementation. In this version of the protocol the magic value is all zeros; a future version assigns per-session magic so the prefilter itself becomes keyed.

A receiver (or a kernel program in front of it) drops any packet that is smaller than 18 bytes, larger than 1400 bytes, or whose pittle and chonkle bytes fail their structural checks. These checks prove a packet has rocketnet's shape; the cryptography below proves everything else.

## Connect Token

A _connect token_ ensures that only authenticated clients can connect to dedicated servers. It is the netcode connect token design, rebuilt on public keys: the backend that mints a token says "this is a valid client, coming from your matchmaker", and a server can verify that statement without holding any secret it shares with the backend.

A connect token has a public portion, one or more sealed per-server portions, and a signature over the whole.

### Private connect token data

Prior to sealing, the private connect token data has the following binary format:

    [client id] (uint64)                // globally unique identifier for an authenticated client
    [timeout seconds] (uint32)          // timeout in seconds. negative values disable timeout (dev only)
    [client to server key] (32 bytes)
    [server to client key] (32 bytes)
    [user data] (256 bytes)             // user defined data specific to this protocol id
    <zero pad to 512 bytes>

This data is written to a fixed size buffer of 512 bytes. Unused bytes are zero padded.

### Sealing to a server

The private connect token data is sealed separately to each candidate server, using that server's registered kx public key:

1. The backend performs `hydro_kx_n_1` against the server's kx public key, producing a kx packet (`hydro_kx_N_PACKET1BYTES` = 48 bytes) and a session keypair. No packet is exchanged with the server; this is a purely local computation.

2. The backend seals the 512-byte private connect token data with `hydro_secretbox_encrypt`, using the kx session `tx` key, context `"rkt-tokn"`, and message id equal to the connect token expire timestamp.

The result for each candidate server is a _sealed private block_:

    [kx packet] (48 bytes)
    [sealed private connect token data] (548 bytes)   // 512 + hydro_secretbox_HEADERBYTES

The server recovers the plaintext by performing `hydro_kx_n_2` with its own kx keypair on the kx packet, then opening the sealed data with the resulting `rx` key, the same context, and the expire timestamp as message id. Using the expire timestamp as the message id binds the sealed data to the expiry stated in the public portion: altering the public expiry makes the seal fail to open.

### Connect token format

    [version info] (14 bytes)           // "ROCKETNET 1.0" ASCII with null terminator.
    [protocol id] (uint64)              // 64 bit value unique to this particular game/application
    [create timestamp] (uint64)         // 64 bit unix timestamp when this connect token was created
    [expire timestamp] (uint64)         // 64 bit unix timestamp when this connect token expires
    [client id] (uint64)
    [timeout seconds] (uint32)
    [client to server key] (32 bytes)   // the same keys sealed in the private blocks, readable by the client
    [server to client key] (32 bytes)
    [num server addresses] (uint32)     // in [1,4]
    <for each server address>
    {
        [address type] (uint8)          // value of 1 = IPv4 address, 2 = IPv6 address.
        <if IPV4 address>
        {
            [a] (uint8) [b] (uint8) [c] (uint8) [d] (uint8) [port] (uint16)
        }
        <else IPv6 address>
        {
            [a..h] (8 x uint16) [port] (uint16)
        }
        [sealed private block] (596 bytes)   // kx packet + sealed private connect token data, for this server
    }
    <zero pad to 2048 - 64 bytes>
    [signature] (64 bytes)              // hydro_sign over all preceding bytes, context "rkt-sign"

The whole token is a fixed 2048 bytes. The signature is created with the backend's sign private key over every byte that precedes it, including the zero padding, and is verified by servers and clients with the backend's sign public key.

The token carries the session keys twice on purpose: once in the clear for the client, which is handed the token over a secure channel and needs the keys to speak; and once sealed per server, so the keys reach each server confidentially through an untrusted carrier — the client itself.

Up to 4 candidate server addresses may be listed, each with its own sealed private block, and the client tries them in order exactly as in netcode. Four is deliberate where netcode allowed 32: each candidate costs a sealed block, and a backend that assigns servers at matchmaking time rarely needs more than a primary and a few fallbacks.

## Challenge Token

Challenge tokens stop clients with spoofed IP packet source addresses from connecting to servers. They are a purely server-local construction: the key involved is random, generated at server start, and never leaves the server.

Prior to sealing, challenge tokens have the following structure:

    [client id] (uint64)
    [user data] (256 bytes)
    <zero pad to 300 bytes>

The challenge token data is sealed with `hydro_secretbox_encrypt` using the server's random challenge key, context `"rkt-chal"`, and a message id equal to the _challenge token sequence number_, which starts at zero and increases with each challenge token generated:

    [sealed challenge token data] (336 bytes)   // 300 + hydro_secretbox_HEADERBYTES

## Packets

**rocketnet** has the following packets:

* _connection request packet_ (0)
* _connection denied packet_ (1)
* _connection challenge packet_ (2)
* _connection response packet_ (3)
* _connection keep alive packet_ (4)
* _connection payload packet_ (5)
* _connection disconnect packet_ (6)

Every packet begins with the 18-byte prefilter header. The prefilter header's packet type byte carries the packet type in its low 4 bits and the number of sequence bytes in its high 4 bits, exactly as netcode's prefix byte does. For the _connection request packet_ the type byte is exactly zero.

### Connection request packet

The _connection request packet_ (0) is not sealed and has the following format after the prefilter header:

    [version info] (14 bytes)           // "ROCKETNET 1.0" ASCII with null terminator.
    [protocol id] (8 bytes)
    [connect token expire timestamp] (8 bytes)
    [client id] (uint64)
    [signature] (64 bytes)              // the connect token signature, copied from the token
    [sealed private block] (596 bytes)  // the block for THIS server address, copied from the token
    <zero pad to 1024 bytes>

The request is a fixed 1024 bytes (plus the 18-byte prefilter header). The client sends only the sealed block addressed to the server it is currently trying, not the whole token.

Note that unlike netcode, the request does not carry enough data for the server to re-verify the backend signature — the signature in the request is used as a stable identifier of the token for the purposes of the used-token history, and authenticity is established by the sealed private block itself: only the real backend could have sealed data to this server's public key that opens correctly. A conforming server MAY additionally require and verify full-token signatures by deployment configuration; the reference implementation does not, keeping request processing to one kx computation and one secretbox open.

### Sealed packets

All other packet types are sealed. Prior to sealing, packet types >= 1 have the following format after the prefilter header:

    [sequence number] (variable length 1-8 bytes)
    [inner type byte] (uint8)           // must equal the packet type in the prefilter header
    [per-packet type data] (variable length according to packet type)

The sequence number encoding is identical to netcode 1.02: the high 4 bits of the type byte give the byte count in [1,8], and the sequence bytes are written low byte first.

The _inner type byte_ is the replacement for netcode's associated-data construction. Hydrogen's secretbox has no associated data parameter, so the packet type is protected by sealing a copy of it inside the packet: after opening, a receiver drops any packet whose inner type byte does not match the outer one. An attacker who alters the outer type changes which checks run, but cannot make the inner byte agree.

The per-packet type data:

_connection denied packet_: no data.

_connection challenge packet_:

    [challenge token sequence] (uint64)
    [sealed challenge token data] (336 bytes)

_connection response packet_:

    [challenge token sequence] (uint64)
    [sealed challenge token data] (336 bytes)

_connection keep-alive packet_:

    [client index] (uint32)
    [max clients] (uint32)

_connection payload packet_:

    [payload data] (1 to 1300 bytes)

_connection disconnect packet_: no data.

The inner type byte and per-packet data are sealed together with `hydro_secretbox_encrypt`, context `"rkt-pack"`, and message id equal to the packet sequence number. Packets from client to server are sealed with the client to server key; packets from server to client are sealed with the server to client key.

Post sealing, packet types >= 1 have the following format:

    [prefilter header] (18 bytes)       // type byte: (num_sequence_bytes << 4) | packet_type
    [sequence number] (variable length 1-8 bytes)
    [sealed data] (36 + 1 + per-packet type data bytes)

## Reading Sealed Packets

The following steps are taken when reading a sealed packet, in this exact order:

* If the packet is smaller than 18 + 1 + 1 + 36 bytes, it is too small to possibly be valid, ignore the packet.

* If the prefilter checks on pittle and chonkle fail, ignore the packet.

* If the low 4 bits of the type byte are greater than or equal to 7, the packet type is invalid, ignore the packet.

* The server ignores packets with type _connection challenge packet_.

* The client ignores packets with type _connection request packet_ and _connection response packet_.

* If the high 4 bits of the type byte (sequence bytes) are outside the range [1,8], ignore the packet.

* If the packet type fails the replay protection already-received test, ignore the packet.

* If the sealed data fails to open with the expected key, context `"rkt-pack"`, and the sequence number as message id, ignore the packet.

* If the inner type byte does not match the packet type in the prefilter header, ignore the packet.

* Advance the most recent replay protection sequence number.

* If the opened data size does not match the expected size for the packet type, ignore the packet:

    * 1 byte for _connection denied packet_
    * 1 + 8 + 336 bytes for _connection challenge packet_
    * 1 + 8 + 336 bytes for _connection response packet_
    * 1 + 8 bytes for _connection keep-alive packet_
    * 1 + [1,1300] bytes for _connection payload packet_
    * 1 byte for _connection disconnect packet_

* If all the above checks pass, the packet is processed.

## Replay Protection

Replay protection is identical to netcode 1.02 in mechanism and intent:

* Sealed packets are sent with 64-bit sequence numbers that start at zero and increase with each packet sent.

* The sequence number is included in the packet header and read prior to opening.

* The sequence number is the secretbox message id, so any modification to it makes the seal fail to open.

The algorithm is unchanged: discard anything older than the most recent sequence number minus the replay buffer size; within the window, accept a sequence number only once; on successful open, mark the entry received and advance the most recent sequence number. Replay protection applies to _connection keep alive_, _connection payload_, and _connection disconnect_ packets on both client and server. A replay buffer of 256 entries per client is a reasonable floor; implementations serving high packet rates should size the buffer to at least one second of their maximum per-client receive rate.

## Message Id Reuse and Server Restarts

Sequence numbers are used directly as secretbox message ids, so the nonce-reuse discipline of netcode 1.02 carries over whole:

* Per-client packet sequence numbers start at zero for each client slot.

* The server's _global sequence number_, used for packets sent before a client is assigned to a slot (_connection challenge packet_ and _connection denied packet_), is set to 2^63 when the server starts, keeping global and per-client message id spaces disjoint under the same key within a run.

The guarantee is scoped to a single run of the server. The server to client key is carried inside the connect token, so a client that presents the same unexpired token to a restarted server would repeat message ids under that key. The window is bounded by token expiry, and an implementation MAY close it entirely with either mitigation from netcode 1.02, both of which apply here unchanged: reject connect tokens whose expire timestamp predates server start plus the deployment's maximum token lifetime, or persist the used-token history. A server that implements neither remains conformant.

## Client State Machine

The client states, transitions, timeouts, retry behaviour, iteration across candidate server addresses, and error states are identical to netcode 1.02:

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

Before its first connection request, the client verifies the connect token: the signature must verify against the backend's sign public key (which the client holds as configuration), the number of server addresses must be in [1,4], every address type must be 1 or 2, and the create timestamp must not be more recent than the expire timestamp; otherwise the client transitions to _invalid connect token_.

All other client behaviour — send rates, storing the challenge token and echoing it back, transition to _connected_ on the first keep-alive, keep-alive generation, timeout handling, redundant disconnect packets — follows the netcode 1.02 standard, reading "sealed" for "encrypted".

## Server-Side Connection Process

The server manages a set of n client slots exactly as in netcode 1.02. Implementations are expected to support high slot counts; the reference implementation targets 1000.

### Processing Connection Requests

The server follows netcode's strict rules: clients must have a valid connect token; respond only when absolutely necessary; ignore malformed requests as early and as cheaply as possible; and every response packet must be smaller than the request that provoked it, so there is nothing to amplify.

The server takes the following steps, in this exact order, when processing a _connection request packet_:

* If the packet, excluding the prefilter header, is not the expected size of 1024 bytes, ignore the packet.

* If the version info in the packet doesn't match "ROCKETNET 1.0" (14 bytes, with null terminator), ignore the packet.

* If the protocol id in the packet doesn't match the expected protocol id of the dedicated server, ignore the packet.

* If the connect token expire timestamp is <= the current timestamp, ignore the packet.

* OPTIONAL. If the server implements the restart mitigation, and the expire timestamp is earlier than server start time plus maximum token lifetime, ignore the packet.

* Perform `hydro_kx_n_2` on the kx packet in the sealed private block with the server's own kx keypair. If it fails, ignore the packet.

* Open the sealed private connect token data with the derived rx key, context `"rkt-tokn"`, and the expire timestamp as message id. If it fails to open, ignore the packet. A successful open proves the block was sealed by a party holding nothing more than this server's public key — combined with the session keys inside being ones the backend minted, this is the token's authenticity check at the server.

* If the opened private connect token data fails to read for any reason, ignore the packet.

* If the client id in the opened data does not match the client id in the request, ignore the packet.

* If a client from the packet IP source address and port is already connected, ignore the packet.

* If a client with this client id is already connected, ignore the packet.

* If the token signature in the request already appears in the history of used connect tokens with a different source IP address and port, ignore the packet. Otherwise, record the signature with this source address and port in the history.

* If no client slots are available, respond with a _connection denied packet_.

* Add an encryption mapping for the packet source IP address and port: packets from it open with the client to server key; packets to it are sealed with the server to client key. The mapping expires after _timeout_ seconds without traffic, or if the connection is not established within _timeout_ seconds. If the mapping cannot be added, ignore the packet.

* Respond with a _connection challenge packet_ and increment the challenge sequence number.

### Processing Connection Response Packets

Identical to netcode 1.02, reading "sealed" for "encrypted": open the challenge token with the server's challenge key and the challenge sequence as message id; ignore on failure; ignore if the source address or client id is already connected; respond _connection denied_ if full; otherwise assign a slot, copy the user data across, mark the client unconfirmed, and respond with a _connection keep-alive packet_.

### Connected Clients

Client slots, the client index, the confirmed flag, keep-alive prefixing until confirmation, payload delivery in both directions, redundant disconnect packets, and slot reset on timeout or disconnect are all identical to netcode 1.02.

## Conformance

An implementation conforms to this standard if it produces and accepts the exact binary formats above, performs the processing steps in the exact orders given, and holds the key inventory discipline of the _Keys_ section. The prefilter header must be present and correctly formed on every packet an implementation sends; whether a receiver checks it in userspace, in a kernel program, or not at all is a quality-of-implementation choice, since the sealed layer beneath it carries the security guarantees.

Higher layers — delivery disciplines, flows, fragmentation — ride inside _connection payload packets_ and are specified separately. This document is the security and connection standard only, as netcode 1.02 was.
