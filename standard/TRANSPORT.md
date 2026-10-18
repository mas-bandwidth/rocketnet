# RocketNet Transport Standard (RN-1)

This document is a normative standard. The key words "MUST", "MUST NOT",
"REQUIRED", "SHALL", "SHALL NOT", "SHOULD", "SHOULD NOT", "RECOMMENDED",
"NOT RECOMMENDED", "MAY", and "OPTIONAL" in this document are to be
interpreted as described in RFC 2119 and RFC 8174.

## t11. Handshake: client

### t11.1 Rule

A client implements the connection handshake as a state machine whose states
are the numbers of Table t11-1. The client MUST begin disconnected. It SHALL
create no transport in a connected state; create leaves the client
disconnected (state 0). The client SHALL accept a connect token only while
disconnected. A negative state is how the last attempt or connection ended
and holds until disconnect, which returns the client to disconnected after
the per-connection reset. The client SHALL connect again with a new token
only from disconnected.

The client MUST process a received handshake packet in this order: the
prefilter header, then the type's fixed wire size, then the state gate, then
the packet's own checks. A packet below its type's floor or of the wrong size
MUST be refused as `WRONG_SIZE` before any later refusal, exactly as the
challenge and denied processors do: a wrong-size challenge or denied MUST NOT
be opened, MUST count a `DID_NOT_OPEN`, and MUST count an integrity failure.

The client MUST accept a cookie only in requesting-cookie or
sending-connection-request. It MUST store the carried cookie and, when in
requesting-cookie, MUST transition to sending-connection-request.

The client MUST accept a challenge only in sending-connection-request. It
MUST refuse a challenge whose size is not the challenge wire size as
`WRONG_SIZE` (see t11.1), MUST refuse a zero sequence, MUST open the sealed
packet under the handshake keys derived from the carried nonce, and on a
successful open MUST store the nonce, hold the session keys, erase the
ephemeral secret, and transition to sending-connection-response.

The client MUST accept a denied only in sending-connection-request or
sending-connection-response. It MUST refuse a denied whose size is not the
denied wire size as `WRONG_SIZE`, MUST refuse a zero sequence, MUST open the
sealed packet, and on a successful open MUST transition to connection-denied.

### t11.2 State numbers

| State | Number |
| --- | --- |
| connect token expired | -7 |
| invalid connect token | -6 |
| connection timed out | -5 |
| connection response timed out | -4 |
| connection request timed out | -3 |
| cookie request timed out | -2 |
| connection denied | -1 |
| disconnected | 0 |
| requesting cookie | 1 |
| sending connection request | 2 |
| sending connection response | 3 |
| connected | 4 |

### t11.3 State table

| State | Event | Guard | Action | Next |
| --- | --- | --- | --- | --- |
| disconnected (0) | connect(token) | token valid, not expired | derive keys, salt, ephemeral; sequences at 1 | requesting cookie (1) |
| disconnected (0) | connect(token) | token invalid | set invalid-connect-token | invalid connect token (-6) |
| disconnected (0) | connect(token) | token expired | set connect-token-expired | connect token expired (-7) |
| requesting cookie (1) | receive cookie | state in {1,2}; size = cookie wire size; salt matches | store cookie; rtt sample | sending connection request (2) |
| requesting cookie (1) | receive cookie | wrong size | refuse WRONG_SIZE | requesting cookie (1) |
| sending connection request (2) | receive challenge | state = 2; size = challenge wire size; sequence != 0; opens | store nonce; derive session keys; erase ephemeral; build response; advance sequence; rtt sample | sending connection response (3) |
| sending connection request (2) | receive challenge | wrong size | refuse WRONG_SIZE; integrity failure | sending connection request (2) |
| sending connection request (2) | receive denied | state in {2,3}; size = denied wire size; sequence != 0; opens | — | connection denied (-1) |
| sending connection response (3) | receive denied | state in {2,3}; size = denied wire size; sequence != 0; opens | — | connection denied (-1) |

### t11.4 Wire sizes

A handshake packet's wire size is fixed and is checked before any state gate
or open. The sizes are in Table t11-4.

| Packet | Number | Wire size |
| --- | --- | --- |
| connection request | 0 | 710 |
| connection denied | 1 | 34 |
| connection challenge | 2 | 66 |
| connection response | 3 | 50 |
| connection cookie | 7 | 38 |

### t11.5 Vectors

- `vector t11-handshake-client-lost-challenge`: the client half of a byte
  transcript of a full handshake including one lost challenge, with the
  client state after each packet. Owed until rocketnet-client-wrong-size-not-
  integrity lands and the code at BASE produces it.
- `vector t11-handshake-client-cookie-to-request`: the client's transmitted
  bytes from requesting-cookie to sending-connection-request.
- `vector t11-handshake-client-challenge-response`: the client's stored
  response bytes on accepting a challenge.
