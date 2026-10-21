# Client and server

Use this flow to connect a development client and server in separate
processes over UDP. Exchange the server's public information, with the
published development backend key standing in for the game's backend.

## Connection flow

The server process:

1. Calls `rocketnet_server_default_config`, sets its nonzero `protocol_id`, and
   copies `ROCKETNET_DEV_BACKEND_PUBLIC_KEY` into
   `rocketnet_server_config_t.backend_public_key`.
2. Calls `rocketnet_server_create`. Create generates a fresh X25519 sealing
   keypair inside the server object; the application does not configure a
   server public or secret key.
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
release server refuses the development key. In production, the game's backend
owns token minting and returns the same kind of opaque token string; RocketNet
does not expose production key-generation or token-minting calls.

## Running the two programs

Use sample sources and a library built against the same public header.

From the repository root, after the normal build, start the server and then the
client in separate terminals:

```sh
./bin/example_client_server_server
./bin/example_client_server_client
```

Both programs use `--port <n>` when supplied and otherwise use port `40000`.
Give both processes the same port:

```sh
./bin/example_client_server_server --port 41234
./bin/example_client_server_client --port 41234
```

The message exchange is: the client connects,
sends one reliable-ordered message, receives the server's echo, disconnects,
and reports failures with a nonzero exit status. Use `--wrong-protocol` to create the development token with a different protocol id and check
that the connection is refused.

## Public calls in the final flow

- `rocketnet_init`, `rocketnet_term`
- `rocketnet_server_default_config`, `rocketnet_server_create`,
  `rocketnet_server_start`, `rocketnet_server_destroy`,
  `rocketnet_server_public_key`, `rocketnet_server_ipv4_address`,
  `rocketnet_server_ipv6_address`, `rocketnet_server_update`,
  `rocketnet_server_send_packets`, `rocketnet_server_start_message`,
  `rocketnet_server_finish_message`, `rocketnet_server_receive_messages`
- `rocketnet_client_default_config`, `rocketnet_client_create`,
  `rocketnet_client_connect`, `rocketnet_client_destroy`,
  `rocketnet_client_update`, `rocketnet_client_send_packets`,
  `rocketnet_client_disconnect`, `rocketnet_client_state`,
  `rocketnet_client_state_name`, `rocketnet_client_start_message`,
  `rocketnet_client_finish_message`, `rocketnet_client_receive_messages`
- `rocketnet_create_dev_connect_token` and
  `ROCKETNET_DEV_BACKEND_PUBLIC_KEY` (development library only)
- `rocketnet_status_name`, `rocketnet_time`, `rocketnet_sleep`
