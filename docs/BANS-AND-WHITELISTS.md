# Bans and whitelists

## How one purchase can become two live sessions

Alice and Bob buy one copy of a game that allows one online player per
purchase. Imagine its backend checks ownership and bans whenever it issues
a token, and its game servers check tokens only when a player connects.

| What happens | What the game server sees |
| --- | --- |
| Alice gives Bob her token for server A. Bob joins. | A sees a valid token for Alice's account. Nobody else on A has that id, so Bob starts playing. |
| Alice gets a fresh token for A and tries to join Bob. | A finds Alice's `client_id` already connected. It silently ignores her request. Bob keeps playing. |
| While Bob plays, Alice gets a fresh token for server B and joins. | B also sees a valid token for Alice's account. Nobody else on B has that id, so Alice starts playing too. |
| An operator bans Alice's account. | The backend refuses her next token. A and B receive no update, so both friends keep playing. |

One purchase is now supporting two simultaneous players. Each successful
handshake used its own genuine token. A checked its own slots correctly,
but B has no record of the connection on A. The same failure applies if the
account supplies fresh tokens on demand to more friends across more servers.

## Why IP bans fail

RocketNet has no IP bans and will not add them.

On a mobile network that uses carrier-grade NAT (CGNAT), two strangers
playing on separate phones with separate subscriptions can reach the game
server from the same public IPv4 address at the same time. An IP ban for
one player's conduct locks both out. Satellite broadband can use CGNAT
too. Even households with separate satellite dishes can share a public
IPv4 address. Meanwhile, a banned player who switches from home Wi-Fi to mobile
data can return with a different address.
See [RFC 6269, section 13.1](https://www.rfc-editor.org/rfc/rfc6269.html#section-13.1).

Consider a game that checks an IP ban only when its backend issues a
connect token. Alice's home IP is banned. She requests a token through a
VPN, disconnects the VPN, and then plays from home. The backend saw the
VPN address. The unused token names the game server and carries Alice's
account id, so it can be presented for its first handshake from her home
address. A ban on Alice's account would apply on either network.

## How to enforce an account ban

RocketNet recommends banning the authenticated account by its user hash
and enforcing one seat through per-server whitelists. In the example
above, moving Alice's seat to B removes her id from A's whitelist. A
disconnects Bob when it receives that update. Banning the account removes
her id from both lists, so neither friend keeps a seat once the servers
receive the ban.

The backend verifies the player's sign-in ticket, derives a stable keyed
user hash from the provider name and user id, and puts that hash in the
signed token as the 64-bit `client_id`. It checks for a known ban before
issuing the token and assigns the id to one server's whitelist. The id
stays the same across network changes.

An already-issued token goes directly to the game server. The backend
gets no new match request when someone presents a copy. The server needs
the current whitelist to enforce a ban or a seat change in that case.

### Why your game implements the whitelist

Suppose Alice joins while her account is in good standing. An operator
then bans her in the backend. The backend can refuse her next token, but
her existing connection looks exactly the same to the game server as it
did before the ban. An unused, unexpired token issued before the ban can
also still pass the handshake. Its signature proves what the backend
authorized when it issued the token.

For bans that take effect during play, we recommend an authenticated path
from the backend to the game server. Without fresh information from the
backend, the server cannot tell that Alice's authorization has changed.
In the example, the server polls for its whitelist and disconnects Alice
as soon as it sees that her id is missing.

RocketNet ships no official backend. As the game developer, you decide who
may play, maintain the whitelist in your backend, and have your game server
fetch and enforce it.
The SDK exposes the connected player's id through `rocketnet_server_client_id`
and lets the server disconnect a player with `rocketnet_server_disconnect_client`.
The [example game server](../examples/backend_server.c) shows how to use
those calls with a whitelist fetched from either of the
[Go and C# example backends](../examples/backend/README.md).

### Running multiple backend instances

Each backend instance can act only on the bans and seat assignments it
has learned about. Suppose instance A records Alice's ban, but the game
server's next whitelist request goes to instance B. If B still lists Alice, the
server keeps her connected. If her next match request also reaches B,
it may issue her another token.

Use the same user hash key on every instance so the same account always
maps to the same `client_id`. Have every instance check bans, assign seats
and build whitelists from a consistent view of the same records. One way
to do this is to use a shared database with one seat per account and
have match and whitelist requests read current ban decisions. If you use
replicas or caches with delayed updates, that delay adds to the time it
takes a game server to enforce a ban.

Each example backend keeps its own bans, seats and server registrations
in memory. Running a second copy does not share those records.

## Run it

Follow steps 1 through 3 of [Production integration](PRODUCTION.md) to build
the examples, generate a signing keypair, and start one backend and one game
server in separate terminals. Wait for the server to report that it has
registered. The example server runs for up to 60 seconds, so have the next
two terminals ready.

From the repository root in a third terminal, sign in as Alice, ask for a
match, and keep the client connected after it receives the server's echo.

```sh
backend=http://127.0.0.1:8080
ticket="$(curl -fsS -X POST "$backend/mock/ticket/alice")"
token="$(curl -fsS --data-binary "$ticket" "$backend/match")"
./bin/example_backend_client --token "$token" --stay --seconds 60
```

After the client prints `client received:`, run this in a fourth terminal.
The ban returns HTTP 204. Watch the game server log the kick and the client
enter `SERVER_DISCONNECTED`.

```sh
backend=http://127.0.0.1:8080
curl -fsS -o /dev/null -w '%{http_code}\n' -X PUT "$backend/bans/mock/alice"
```

While the backend is still running, sign in as Alice again. Even a fresh
ticket gets HTTP 403 at match. Lifting the ban returns HTTP 204.

```sh
ticket="$(curl -fsS -X POST "$backend/mock/ticket/alice")"
curl -sS -o /dev/null -w '%{http_code}\n' --data-binary "$ticket" "$backend/match"
curl -fsS -o /dev/null -w '%{http_code}\n' -X DELETE "$backend/bans/mock/alice"
```

## Why it works

### The client cannot read or change its `client_id`

The player presents a sign-in ticket. The backend verifies it and gets two
values from the verifier, the provider name and that provider's user id.
The match request carries only the ticket. In the examples, the backend
hashes the UTF-8 bytes of `"<provider>:<user id>"` with BLAKE2b-256 keyed
by `ROCKETNET_BACKEND_USER_HASH_KEY`. The first eight digest bytes, read
little endian, become the `client_id`. Provider names exclude `:`, so
accounts such as `mock:alice` and `guest:alice` have distinct hash inputs.

Keep the hash key stable across backend instances and restarts. Changing it
changes the mapping used to look up seats and bans. A game that already has
a unique 64-bit account id can use that id directly.

The backend puts the id in the token's private block, seals the block to the
selected server's public key, and signs the data containing that block.
The client gets its session keys and can present the token, but it cannot
read or alter the id in it. After the handshake the server reads that id
with `rocketnet_server_client_id`. See the [token format](STANDARD.md).

<a id="one-seat-per-account"></a>

### What happens if players share a connect token?

In the [one-purchase example](#how-one-purchase-can-become-two-live-sessions),
Bob and Alice both present Alice's `client_id`. Which check can detect
the second session depends on where they connect.

- **On the same server:** Bob already occupies Alice's seat. The server
  reads the same `client_id` from Alice's fresh token, silently ignores
  the duplicate connection request, and leaves Bob connected. The transport
  also refuses reuse of a token from another address while it retains the
  original address binding. See the
  [handshake rules](../standard/TRANSPORT.md#t12) and
  [binding lifetime](../standard/TRANSPORT.md#t17).
- **On different servers:** Suppose Bob is playing on server A and Alice
  gets a new match on server B. The backend moves the account's seat to B.
  When A receives its updated whitelist, it disconnects Bob. The two
  sessions can overlap until A receives that update. This is why each
  server needs to hear about seat changes after the backend issues a token.

If the game bans Alice's account, it withdraws the seat altogether. The
server disconnects whoever is using it when it receives the updated list.
If Alice and Bob take turns, the one-seat rule alone cannot tell them
apart. Both present Alice's signed account id, and the game attributes play
and any moderation action to that account. Treat connect tokens as private
credentials.

## The example game server

The server registers its advertised UDP address and public key with the
backend. By default it registers as `example-<port>` at `127.0.0.1:8080`.
Use `--server-id <id>` and `--backend <ip:port>` to change those settings.

Every two seconds the main thread asks a worker to fetch the whitelist.
The worker reads the reply into a separate buffer. The main thread takes
the newest complete list during the game update, checks connected slots,
and calls `rocketnet_server_disconnect_client` for each id it must remove.
A frame can continue while the worker waits for the backend.
See the [server loop](../examples/backend_server.c) and
[whitelist worker](../examples/whitelist.h).

| What the server sees | What happens to a connected player |
| --- | --- |
| A valid list requested after the player connected | The player stays if their id is present. An empty list authorizes nobody. |
| Only a list requested before or at connection time | The player has a six-second grace period for a fresh fetch. This lets a newly matched player join before the server sees the updated list. |
| A fetch fails or the body is invalid | The server keeps the last good list. After the grace period it disconnects anyone absent from that list. If it has never received a list, it disconnects everyone past the grace period. |

A backend outage can delay a new ban for a player who is on the server's
cached list. The six seconds run from **connection time**. They limit how long a new connection can wait for its first check. Once the backend is reachable, its next successful
reply carries the current bans. A 404 for an unknown server id causes the
server to register again.

The example fetcher accepts a plain HTTP response with a declared body
length and room for up to 4096 ids. A game should use its authenticated
backend connection and size its whitelist for the number of seats it assigns.

## The example backends

The Go and C# services expose the same [HTTP routes and configuration](../examples/backend/README.md).

| Request | Result |
| --- | --- |
| `POST /mock/ticket/{userID}` | The local sign-in stand-in returns a ticket valid for 60 seconds for any user id of 1 to 128 UTF-8 bytes. |
| `POST /match` | The body is the ticket. An invalid or expired ticket gets 401. With a valid ticket, no registered servers gives 503. Otherwise a banned user gets 403, and a successful match returns a base64 connect token. |
| `PUT /bans/{provider}/{userID}` | Bans the account and returns 204. Subsequent whitelist replies omit the id. |
| `DELETE /bans/{provider}/{userID}` | Lifts the ban and returns 204. In this example the backend remembers the account's last seat. |
| `PUT /servers/{serverID}` | Registers or updates the server's addresses and public key. |
| `GET /servers/{serverID}/whitelist` | Returns the allowed ids as ascending unsigned 64-bit integers, each encoded little endian. The body has type `application/octet-stream` and a `Content-Length`. An empty list has zero bytes. |

The mock ticket encodes an eight-byte little-endian expiry time, a one-byte
user id length, the UTF-8 user id and a 32-byte keyed BLAKE2b-256 MAC over
those fields. The service sends it as standard base64. The mock provider
key is separate from the user hash key and the backend signing key.

For a real game, remove the mock ticket route. Replace the verifier supplied
by `verifyTicketWith` or `Users.VerifyWith` with one that validates the
platform's ticket and returns its provider and user id. See the
[Go implementation](../examples/backend/go/users.go) and
[C# implementation](../examples/backend/csharp/Backend/Users.cs). Authenticate the game server for registration and whitelist reads,
and restrict ban changes to authorized operators. Both examples keep bans,
seats and registrations in memory. Persist bans across restarts and
release seats when sessions end.

## Testing

The [Go](../examples/backend/go/users_test.go) and
[C#](../examples/backend/csharp/Tests/UsersTests.cs) unit suites use a fixed
clock and common expected ticket and hash values. They check ticket expiry,
the id in the token, refusal of a banned user and movement between whitelists.

The [end-to-end test](../tools/run_backend_examples.py) starts the backend,
server and clients on loopback. It checks a shared token, a second sign-in,
a ban during play, and a token issued before the ban. With the backend
offline it checks that a listed player stays and an unlisted player is
disconnected. Its bounds on loopback are 2.5 seconds for a kick with a
healthy backend and 6.5 seconds for a newly connected, unlisted player
during an outage. Run it using the [backend verification instructions](../examples/backend/README.md#unit-and-functional-verification).
