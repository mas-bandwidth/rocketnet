# Bans and whitelists

How to keep banned players out of a game that uses RocketNet: ban the user,
never the IP address. The example backends in
[examples/backend/](../examples/backend/README.md) and the example game
server [examples/backend_server.c](../examples/backend_server.c) do it end to
end, and this page explains the pattern so a title can adapt it.

RocketNet itself needs only one thing for this: a unique 64-bit `client_id`
per user in every connect token. Everything else on this page is the title's
backend and game server, shown in the examples.

## The pattern in one list

1. **An auth provider says who the player is.** The client signs in with the
   title's auth provider and gets a ticket. The backend checks the ticket in
   one place and gets the provider's name and the provider's user id.
2. **The backend derives the `client_id`.** It is a keyed hash of
   `"<provider>:<user id>"`. The client never sends or chooses it.
3. **The backend checks bans before it mints.** A banned user gets no connect
   token.
4. **The backend keeps one seat per user.** When it matches a user to a
   server, that user's `client_id` goes on that server's whitelist and off
   every other server's.
5. **The game server enforces the whitelist.** Every few seconds it fetches
   its whitelist, and it disconnects any connected client whose `client_id`
   is not on it.
6. **The game server fails closed.** A failed fetch keeps the last good
   list. A client on no list is disconnected, list or no list.

## Why it works

### The client cannot read or change its `client_id`

The backend writes `client_id` into the connect token's 384-byte private
block and seals that block with `crypto_box_seal` to the chosen server's
X25519 public key ([STANDARD.md](STANDARD.md), and the writers in
[go/main.go](../examples/backend/go/main.go) and
[csharp/Backend/ConnectToken.cs](../examples/backend/csharp/Backend/ConnectToken.cs)).
Only that server can open it. The backend's Ed25519 signature covers the
576-byte signed connect data, and the sealed block is inside it, so any
change to the block fails the server's signature check. The client's own
section of the token holds only its session keys.

So the `client_id` a server reads with `rocketnet_server_client_id` is the
one the backend chose, for the user the backend authenticated.

### A user hash, not a raw platform id

The examples compute `client_id` as the first 8 bytes, read little endian,
of BLAKE2b-256 keyed with a backend secret, over `"<provider>:<user id>"`
(for example `"mock:alice"`):

- the same user on the same provider always gets the same `client_id`, so a
  ban sticks to the user however often they sign in;
- the same user id on two providers gets two different `client_id`s, so ids
  from different providers never collide by accident;
- the key is the backend's secret (`ROCKETNET_BACKEND_USER_HASH_KEY`), so
  nobody outside the backend can compute a user's `client_id` from their
  user id. Keep the key fixed for the life of the title: a new key gives
  every user a new `client_id`, and every ban keyed by the old one stops
  matching.

A provider name may not contain `:`, so `"<provider>:<user id>"` reads only
one way.

RocketNet does not require this scheme. It requires only that each user has
one unique 64-bit `client_id`. A title may instead hash its own
cross-platform account name, or use a 64-bit account id it already has. The
choice is the title's.

### Sharing a token gains nothing

A connect token is a bearer credential: whoever holds it can try to use it,
and a player can give theirs to someone else. That cannot be prevented, and
the pattern makes it worthless:

- RocketNet binds a token to the address that first completes the handshake
  with it, and refuses the token from any other address while that binding
  lives ([standard/TRANSPORT.md](../standard/TRANSPORT.md#t12), t12 rules 9
  and 13). A binding lasts until the token expires. The binding table holds
  a fixed number of bindings per client slot; when it is full of live
  bindings the oldest is replaced, and that token is then no longer refused
  from another address ([t17](../standard/TRANSPORT.md#t17)).
- A server refuses a second connection whose `client_id` already holds a
  slot (t12 rules 10 and 13), so two people sharing one account cannot both
  be on one server.
- The backend seats a user on one server at a time, so a second session on
  another server pushes the first off the first server's whitelist, and that
  server disconnects it.
- Whoever uses the token plays as the user who got it: their `client_id`,
  their one seat, their bans.

The end-to-end test shows each of these (see [Testing](#testing)).

## Why not ban IP addresses

IP banning does not work, and it does harm:

- **Players change IP addresses in seconds.** Reconnecting a phone, restarting
  a home router or turning on a VPN gives a new address.
- **Many players share one address.** Carrier-grade NAT puts a whole mobile
  network's subscribers behind a few addresses; VPN exits, campuses, offices,
  cafes and homes share one too. Banning that address bans every innocent
  player behind it.
- **Blocklists of VPN and proxy addresses are an arms race** that the banned
  player wins, and they catch honest players who use a VPN for privacy.
- **IP addresses are personal data under the GDPR**, so a list of them is a
  store of personal data with all the duties that brings.

RocketNet has no IP bans and no IP ban list, and will not add them. Ban by
user, as above.

## The example backends

Both backends (Go and C#) implement the same routes, wire forms and hashes.
Their unit tests check the same known values, so a ticket or user hash from
one is the same as from the other given the same keys.

| Route | What it does |
| --- | --- |
| `POST /mock/ticket/{userID}` | The mock auth provider: signs a ticket for any user id (1 to 128 bytes of UTF-8). Stands in for a platform's sign-in. |
| `POST /match` | Body: the ticket. Checks it, hashes the user, refuses a banned user, seats them on a server and returns a connect token (base64 text) whose `client_id` is the user hash. |
| `PUT /bans/{provider}/{userID}` | Bans the user. Their hash comes off every whitelist at once and they get no new token. `204`. |
| `DELETE /bans/{provider}/{userID}` | Lifts the ban. `204`. |
| `PUT /servers/{serverID}` | The game server registers its address and public key (unchanged). |
| `GET /servers/{serverID}/whitelist` | The `client_id`s allowed on the server: matched there and not banned. |

`POST /match` answers `401` for a bad or expired ticket, `403` for a banned
user and `503` when no server is registered. A body three or more bytes
longer than the longest valid ticket is `400`. A ban with an empty provider name, a provider
name holding `:`, or a bad user id is `400`. The whitelist of a server that
is not registered is `404`, and the game server then registers again.

**The whitelist wire form.** The body of `GET /servers/{serverID}/whitelist`
is each `client_id` as 8 bytes little endian, in ascending order, and
nothing else: `Content-Type: application/octet-stream`, with a
`Content-Length`. An empty list is an empty body. 1000 players are 8000
bytes.

**The mock ticket.** `expire timestamp (8 bytes, little endian) | user id
length (1) | user id | MAC (32)`, as standard base64. The MAC is BLAKE2b-256
keyed with the mock provider's own secret
(`ROCKETNET_BACKEND_MOCK_PROVIDER_KEY`) over everything before it. Without
that secret nobody can make a ticket or change one, and a ticket is refused
from its expire timestamp on (60 seconds after it is issued).

**The seam.** The ticket check is one function: `verifyTicketWith` returns a
`verifyTicketFunction` in [go/users.go](../examples/backend/go/users.go), and
`Users.VerifyWith` returns a `VerifyTicket` delegate in
[csharp/Backend/Users.cs](../examples/backend/csharp/Backend/Users.cs). Each
takes the ticket and the time, and returns the provider name and user id or
an error. A title replaces it with its platform's ticket check, or with one
check per platform, each returning its own provider name. The mock route
`POST /mock/ticket/{userID}` goes away: a platform's sign-in runs on the
client and the platform's own servers.

**State.** Bans, seats and the server registry are in memory, so they are
lost when the backend stops. A real backend keeps them in its database. The
examples never drop a seat except when the user is matched elsewhere; a real
backend also drops a user from a whitelist when their session ends, so lists
do not grow without bound.

## The example game server

[examples/backend_server.c](../examples/backend_server.c) with
[examples/whitelist.h](../examples/whitelist.h):

- At start it registers itself with the backend (`PUT /servers/{serverID}`)
  with its address and public key. The backend is at `127.0.0.1:8080`
  unless `--backend <ip:port>` says otherwise, and the server's id is
  `example-<port>` unless `--server-id <id>` says otherwise.
- Every 2 seconds (`WHITELIST_SECONDS`) the main thread asks for a fetch of
  `GET /servers/{serverID}/whitelist`. A separate thread does the request,
  parses the list into one of two preallocated buffers, and publishes it.
  The main thread takes the newest list at the start of its world update.
  The lock between them is held only to swap which buffer is which, never
  across a request, so the frame never waits on the network. The thread
  never writes the buffer the main thread is reading.
- In its world update the main thread checks each connected slot's
  `rocketnet_server_client_id` against the list and calls
  `rocketnet_server_disconnect_client` for one that is not on it. The client
  sees `ROCKETNET_CLIENT_STATE_SERVER_DISCONNECTED`.
- A new connection is judged by the first list asked for after it connected,
  so a player who was just matched is not kicked by a list fetched before
  their match. Once a client has been connected for `WHITELIST_GRACE` (6
  seconds) it is judged by the newest list there is, fetched since or not.
- **It fails closed.** A failed fetch (no connection, a timeout, a bad
  status, a malformed body) keeps the last good list and is printed. A
  client not on the list the server holds is not allowed; with no list at
  all, no client stays past the grace.
- The HTTP client is HTTP/1.1 over plain TCP sockets (POSIX or Winsock),
  one request per connection, literal IP addresses only, `Content-Length`
  bodies only, with a 2-second bound on connect, each send and each
  receive. It is the smallest client that does the job, for the example. A
  title uses its own HTTP client, with TLS and its backend's authentication.

RocketNet is not changed by any of this: the server uses only
`rocketnet_server_client_connected`, `rocketnet_server_client_connection_id`,
`rocketnet_server_client_id` and `rocketnet_server_disconnect_client`, all
on the main thread while it owns the server.

**How fast a ban takes effect.** After a ban, the backend's next whitelist
answer leaves the user out. The server asks every 2 seconds, so the kick
comes within one interval plus the fetch, a frame and the disconnect packet.
The end-to-end test requires at most 2.5 seconds; measured on a Linux bench
over loopback, kicks came 0.87 to 0.97 seconds after the ban or connect,
and the fail-closed kick of a client on no list came at the 6-second grace.

## Run it

From the repository root, after building `example_backend_server` and
`example_backend_client` (see [Production integration](PRODUCTION.md)):

```sh
# Terminal 1: a backend
(cd examples/backend/go && go run .)
# Or: (cd examples/backend/csharp && dotnet run --project Backend)
```

```sh
# Terminal 2: the game server; it registers itself and fetches its whitelist
./bin/example_backend_server --sessions 10
```

```sh
# Terminal 3: sign in, match, connect and stay
ticket="$(curl -sS -X POST http://127.0.0.1:8080/mock/ticket/alice)"
token="$(curl -sS --data "$ticket" http://127.0.0.1:8080/match)"
./bin/example_backend_client --token "$token" --stay --seconds 60
```

```sh
# Terminal 4: ban alice; the client in terminal 3 is disconnected within seconds
curl -sS -X PUT http://127.0.0.1:8080/bans/mock/alice
curl -sS -X POST --data "$(curl -sS -X POST http://127.0.0.1:8080/mock/ticket/alice)" http://127.0.0.1:8080/match
# this user is banned
curl -sS -X DELETE http://127.0.0.1:8080/bans/mock/alice
```

The server prints each connect, kick and disconnect with the `client_id` in
hex, and each whitelist fetch that fails.

## Testing

Unit tests, with no sockets and no real time (the clock is an argument):
`go test ./...` in `examples/backend/go` (`users_test.go`) and
`dotnet run --project Tests` in `examples/backend/csharp`
(`Tests/UsersTests.cs`). They check that a forged, edited or expired ticket
gets no token; the same user gets the same hash and another provider another
hash; a banned user gets no token; a whitelist lists only users matched to
that server and not banned; a ban takes a user off the list; a user is on
one server's list at a time; the token's `client_id` is the user hash; and
that both backends produce the same ticket and hash for the same keys.

The end-to-end test, `tools/run_backend_examples.py`, runs against both
backends with the real server and client over loopback:

1. with no server registered, a match is `503`;
2. the server registers itself and fetches its whitelist;
3. client A signs in as `alice`, is matched, connects, and stays connected
   across whitelist fetches;
4. client B with A's connect token never connects;
5. B signed in as `alice` is matched to the same one whitelist entry but
   cannot connect while A holds the seat;
6. `alice` is banned: off the whitelist at once, and A is kicked within the
   bound above;
7. A's token still never connects from another address;
8. B's token, minted before the ban, connects once the seat is free, and B,
   playing as `alice`, is kicked within the same bound;
9. `alice`'s next match is `403`;
10. with the backend stopped, `bob`, who is on the last good list, stays
    connected, while `carol`, with a token minted offline and so on no list,
    is kicked within the grace.
