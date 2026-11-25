# Bans and whitelists

Ban a player by their account. A player can change networks, and several
players can share one public IP address. RocketNet has no IP bans and will
not add them.

The backend decides who may play. It puts the authenticated player's 64-bit
`client_id` in a connect token and assigns that id to one game server. Each
server fetches its whitelist from the backend and disconnects players whose
ids are missing. A ban therefore affects both the player's next match and
the session they already have open.

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

### One seat per account

A connect token is a bearer credential. Someone who receives a copy can
present it as the original account. Sharing gives them neither a second
seat nor a different identity. The server refuses a second simultaneous
connection with the same `client_id`. If the backend matches the account
to a different server, only that server's next whitelist contains the id.
The previous server disconnects the player when it receives the change.
The transport also tracks token reuse across addresses in a bounded table.
See the [handshake rules](../standard/TRANSPORT.md#t12) and
[binding lifetime](../standard/TRANSPORT.md#t17).

A ban makes the backend refuse new matches and omit the user from every
whitelist it returns. A previously issued token can still complete a
handshake. The game server enforces the ban when it checks the whitelist.

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
