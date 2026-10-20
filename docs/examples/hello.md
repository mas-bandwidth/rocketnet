# Hello

The least RocketNet program that works: a server and a client in one process
over the in-memory `loopback` transport, which needs no socket and no network.

## What it shows

1. Initialize the library.
2. Create a server with the published development keys, an ephemeral port,
   and the `loopback` transport pinned in its config.
3. Mint a development connect token for that server address.
4. Create a client pinned to `loopback`, then update both ends until the
   client reports `ROCKETNET_CLIENT_STATE_CONNECTED`.
5. Send one `ROCKETNET_RELIABILITY_RELIABLE_ORDERED` message from the client to the
   server with `rocketnet_client_start_message`, a `memcpy` into the returned buffer,
   and `rocketnet_client_finish_message`.
6. Receive and print it on the server, release the view, disconnect the
   client, and destroy both ends.
7. Connect a second client with a second development token, then disconnect
   it, showing that a server keeps serving after one client leaves.

Each frame runs the calls in the order a game does: update, receive and
release, simulate, queue, then send last. Sending last is what makes this
frame's messages leave this frame instead of waiting a frame.

The lifecycle card that lands after this one renames start, finish, receive,
release and send_packets to `*_start_message`, `*_finish_message`,
`*_receive_message`, `*_release_message` and `*_send_packets`. The header at
this base still declares the old names, so the example uses them and says so
in the source comment.

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
- `rocketnet_server_dev_config`, `rocketnet_server_create`,
  `rocketnet_server_update`, `rocketnet_server_port`,
  `rocketnet_server_max_clients`, `rocketnet_server_client_connected`,
  `rocketnet_server_receive_message`, `rocketnet_server_release_message`,
  `rocketnet_server_send_packets`, `rocketnet_server_destroy`
- `rocketnet_client_default_config`, `rocketnet_client_create`,
  `rocketnet_client_state`, `rocketnet_client_update`,
  `rocketnet_client_start_message`, `rocketnet_client_finish_message`,
  `rocketnet_client_receive_message`, `rocketnet_client_release_message`,
  `rocketnet_client_send_packets`, `rocketnet_client_disconnect`,
  `rocketnet_client_destroy`
- `rocketnet_write_dev_connect_token` (development library only)
- `rocketnet_time`, `rocketnet_sleep`
