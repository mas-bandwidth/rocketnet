# Hello

The least RocketNet program that works: a server and a client in one process
over the in-memory `loopback` transport, which needs no socket and no network.

## What it shows

1. Initialize the library.
2. Fill a server config with `rocketnet_server_default_config`, copy
   `ROCKETNET_DEV_BACKEND_PUBLIC_KEY` into `backend_public_key`, select an
   ephemeral port, and pin the `loopback` transport. Server create generates
   the server's ephemeral sealing keypair.
3. After start, read the bound address with `rocketnet_server_ipv4_address`
   and the generated public key with `rocketnet_server_public_key`. Pass both
   to `rocketnet_create_dev_connect_token`, which writes an opaque,
   NUL-terminated base64 token string.
4. Create a client pinned to `loopback`, then update both ends until the
   client reports `ROCKETNET_CLIENT_STATE_CONNECTED`.
5. Send one `ROCKETNET_RELIABILITY_RELIABLE_ORDERED` message from the client to the
   server with `rocketnet_client_start_message`, checking that the returned pointer is
   non-`NULL` (`if ( out )`) before writing the text straight into the returned buffer
   and calling `rocketnet_client_finish_message` with the bytes written inside that
   branch (calling `finish_message` without a successful outstanding `start_message`
   is an error and returns `0`).
6. Receive and print it on the server, disconnect the
   client, poll `rocketnet_client_disconnected` (stepping `rocketnet_client_update`)
   before destroying it, and stop/destroy the server once `rocketnet_server_stopped`
   returns `true`.
7. Connect a second client with a second development token, then disconnect
   it, showing that a server keeps serving after one client leaves.

Each frame runs the calls in the order a game does: update, drain the one
receive queue until `NULL`, simulate, queue with guarded `start_message` /
`finish_message`, then send last. Sending last is what makes this
frame's messages leave this frame instead of waiting a frame. See
[docs/GETTING-STARTED.md](../GETTING-STARTED.md) and [USAGE.md](../../USAGE.md)
for the full integration guide.

## Run it

From the repository root, after the normal build:

```sh
./bin/example_hello
```

On Windows the binary is `bin\Release\example_hello.exe`. The program finishes
well inside five seconds and prints `PASS` as its last line; any failure
prints one `FAIL: ...` line and exits 1.

## What the output means

```
server received: hello from the client
PASS
```

The first line is the message the server received. `PASS` means the client
connected, the message arrived, and the client reconnected with a second
token and disconnected cleanly. The informational log lines before it come
from the library and are not part of the check.

## Public calls it uses

- `rocketnet_init`, `rocketnet_term`
- `rocketnet_server_default_config`, `rocketnet_server_create`,
  `rocketnet_server_start`, `rocketnet_server_stop`, `rocketnet_server_stopped`,
  `rocketnet_server_destroy`,
  `rocketnet_server_update`, `rocketnet_server_send_packets`,
  `rocketnet_server_ipv4_address`,
  `rocketnet_server_public_key`, `rocketnet_server_receive_messages`
- `rocketnet_client_default_config`, `rocketnet_client_create`,
  `rocketnet_client_connect`, `rocketnet_client_disconnected`,
  `rocketnet_client_destroy`,
  `rocketnet_client_update`, `rocketnet_client_send_packets`,
  `rocketnet_client_disconnect`, `rocketnet_client_state`,
  `rocketnet_client_start_message`, `rocketnet_client_finish_message`,
  `rocketnet_client_receive_messages`
- `rocketnet_create_dev_connect_token` and
  `ROCKETNET_DEV_BACKEND_PUBLIC_KEY` (development library only)
- `rocketnet_status_name`, `rocketnet_time`, `rocketnet_sleep`
