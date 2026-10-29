# Client and server

Use this flow to connect a development client and server in separate
processes over UDP. Exchange the server's public information, with the
published development backend key standing in for the game's backend.

## Connection flow

The server process:

1. Calls `rocketnet_server_default_config`, sets its nonzero `protocol_id`, and
   copies `ROCKETNET_DEV_BACKEND_PUBLIC_KEY` into
   `rocketnet_server_config_t.backend_public_key`.
2. Calls `rocketnet_server_create`. This example uses the default generated
   X25519 sealing keypair. Other integrations can supply `server_secret_key`
   at create when they need a stable key; see [Keys](../../KEYS.md).
3. Calls `rocketnet_server_start`, then reads the generated public key with
   `rocketnet_server_public_key` and the bound address with
   `rocketnet_server_ipv4_address` or `rocketnet_server_ipv6_address`.
4. Makes that public key and address available to the development client by an
   application-defined local channel. The server secret key remains inside the
   server object and is wiped at destroy.

The client-side development token helper:

1. Passes the server's public key and served address, token lifetime, timeout,
   client id, protocol id, current Unix timestamp, and optional user data to
   `rocketnet_create_dev_connect_token`.
2. Receives an opaque NUL-terminated base64 string in a
   `ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES` buffer.
3. Passes that string directly to `rocketnet_client_connect`; it does not parse
   or modify the token.

`ROCKETNET_DEV_BACKEND_PUBLIC_KEY` and
`rocketnet_create_dev_connect_token` exist only in the development library. A
release server (`ROCKETNET_DEV=0`) refuses the development key, development
tokens, and the default test `packet_filter`. In production, the game's backend
owns the signing secret key, mints tokens for the title's `protocol_id` and
server public key, and returns the same kind of opaque token string
([docs/SESSION.md](../SESSION.md), [KEYS.md](../../KEYS.md)); RocketNet does
not expose production key-generation or token-minting calls.

## Running the two programs

Use sample sources and a library built against the same public header.

From the repository root, after the normal build, start the server and then the
client in separate terminals:

```sh
./bin/example_client_server_server
./bin/example_client_server_client
```

The server binds port `40000` by default, or `--port <n>` when supplied (the
`ROCKETNET_EXAMPLE_PORT` environment variable sets it too). It writes its
address and public key to the key file `client_server.keys`, which the client
reads, so the client takes no port argument. Run both from the same working
directory:

```sh
./bin/example_client_server_server --port 41234
./bin/example_client_server_client
```

The message exchange is: the client connects,
sends one reliable-ordered message (preflight checks room with
`rocketnet_client_can_send_message`, then checks the non-`NULL` pointer from
`rocketnet_client_start_message` before calling
`rocketnet_client_finish_message` inside that branch), drains
`rocketnet_client_receive_messages` until `NULL` to receive the server's echo,
disconnects, polls `rocketnet_client_disconnected` before destroy,
and reports failures with a nonzero exit status. Use `--wrong-protocol` to create the development token with a different protocol id and check
that the connection is refused. See [docs/GETTING-STARTED.md](../GETTING-STARTED.md)
for the step-by-step walkthrough of these two files.

## Public calls in the final flow

- `rocketnet_init`, `rocketnet_term`
- `rocketnet_server_default_config`, `rocketnet_server_create`,
  `rocketnet_server_start`, `rocketnet_server_stop`, `rocketnet_server_stopped`,
  `rocketnet_server_destroy`,
  `rocketnet_server_public_key`, `rocketnet_server_ipv4_address`,
  `rocketnet_server_ipv6_address`, `rocketnet_server_update`,
  `rocketnet_server_send_packets`, `rocketnet_server_can_send_message`, `rocketnet_server_start_message`,
  `rocketnet_server_finish_message`, `rocketnet_server_receive_messages`
- `rocketnet_client_default_config`, `rocketnet_client_create`,
  `rocketnet_client_connect`, `rocketnet_client_disconnected`,
  `rocketnet_client_destroy`,
  `rocketnet_client_update`, `rocketnet_client_send_packets`,
  `rocketnet_client_disconnect`, `rocketnet_client_state`,
  `rocketnet_client_state_name`, `rocketnet_client_start_message`,
  `rocketnet_client_finish_message`, `rocketnet_client_receive_messages`
- `rocketnet_create_dev_connect_token` and
  `ROCKETNET_DEV_BACKEND_PUBLIC_KEY` (development library only)
- `rocketnet_status_name`, `rocketnet_time`, `rocketnet_sleep`
