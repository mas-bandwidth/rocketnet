# Stats

How a game's debug overlay reads connection quality: a server and a client in
one process over the in-memory `loopback` transport. The client connects,
exchanges a little traffic, then prints the client's and the server's state and
counters through the public query calls and enums.

## What it shows

1. Initialize the library and create a server on the `loopback` transport.
2. Mint a development connect token and connect a client.
3. Run frames until the client reports `ROCKETNET_CLIENT_STATE_CONNECTED`.
4. Exchange traffic for about one and a half seconds: the client sends a
   reliable-ordered message each frame and the server echoes one back, so both
   directions have traffic to measure.
5. Print the client's state name, RTT, jitter, packet loss, bandwidth sent and
   received, and a few counters through `rocketnet_client_stat` and
   `rocketnet_client_counter`.
6. Print the server's per-client view of the same connection through
   `rocketnet_server_client_stat` and `rocketnet_server_client_counter`.
7. Disconnect, then print the disconnect reason on both ends through
   `rocketnet_client_disconnect_reason` and
   `rocketnet_server_disconnect_reason`.

Each frame runs the calls in the order a game does: update, receive and
release, simulate, queue, then send last. Sending last is what makes this
frame's messages leave this frame instead of waiting a frame.

## Run it

From the repository root, after the normal build:

```sh
./bin/example_stats
```

On Windows the binary is `bin\Release\example_stats.exe`. The program finishes
well inside ten seconds and prints `PASS` as its last line; any failure prints
one `FAIL: ...` line and exits 1.

## What the output means

The output prints the client's state and connection quality, then the server's
per-client view, then the disconnect reasons, then `PASS`. The check passes
when the numbers are present and sane: RTT, jitter, loss and bandwidth are
never negative, and bytes sent are above zero after the traffic exchange. `PASS`
means the client connected, traffic flowed both ways, and the disconnect was
seen on both ends. The informational log lines before the output come from the
library and are not part of the check.

## Public calls it uses

- `rocketnet_init`, `rocketnet_term`
- `rocketnet_server_dev_config`, `rocketnet_server_create`,
  `rocketnet_server_update`, `rocketnet_server_port`,
  `rocketnet_server_max_clients`, `rocketnet_server_client_connected`,
  `rocketnet_server_start`, `rocketnet_server_finish`,
  `rocketnet_server_receive`, `rocketnet_server_release`,
  `rocketnet_server_send_packets`, `rocketnet_server_disconnect_reason`,
  `rocketnet_server_client_stat`, `rocketnet_server_client_counter`,
  `rocketnet_server_destroy`
- `rocketnet_client_default_config`, `rocketnet_client_create`,
  `rocketnet_client_state`, `rocketnet_client_state_name`,
  `rocketnet_client_index`, `rocketnet_client_update`,
  `rocketnet_client_start`, `rocketnet_client_finish`,
  `rocketnet_client_receive`, `rocketnet_client_release`,
  `rocketnet_client_send_packets`, `rocketnet_client_disconnect`,
  `rocketnet_client_disconnect_reason`, `rocketnet_client_stat`,
  `rocketnet_client_counter`, `rocketnet_client_destroy`
- `rocketnet_write_dev_connect_token` (development library only)
- `rocketnet_time`, `rocketnet_sleep`
