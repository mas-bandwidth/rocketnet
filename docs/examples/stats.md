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
   32-byte reliable-ordered message each frame and the server echoes each one
   back, so both directions have traffic to measure.
5. Print the client's state name, RTT, jitter, packet loss, bandwidth sent and
   received, and a few counters through `rocketnet_client_stat` and
   `rocketnet_client_counter`.
6. Print the server's per-client view of the same connection through
   `rocketnet_server_client_stat` and `rocketnet_server_client_counter`.
7. Disconnect, run frames until the client reports disconnected (up to two
   seconds), then print the numeric disconnect reason on both ends through
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

```
client state connected
client rtt 2.09 ms, jitter 0.09 ms, loss 0.00%, bandwidth 737.0 kbps sent 737.0 kbps received
client bytes sent 139132 received 137595
server client 0 rtt 2.09 ms, jitter 0.28 ms, loss 0.08%, bandwidth 539.2 kbps sent 533.8 kbps received
server client 0 bytes sent 137701 received 139026
client after disconnect state disconnected reason 0
server client 0 disconnect reason 0
PASS
```

The output prints the client's state and connection quality, then the server's
per-client view, then the disconnect reasons (0 is a clean disconnect), then
`PASS`. The numbers vary from run to run. The check passes when the numbers
are sane: the client's RTT, jitter, loss and bandwidth are never negative, the
server's RTT is never negative, and bytes sent are above zero on both ends
after the traffic exchange. `PASS` means the client connected and traffic
flowed both ways; the disconnect lines are printed, not checked. The informational log lines before the output come from the
library and are not part of the check.

## Public calls it uses

- `rocketnet_init`, `rocketnet_term`
- `rocketnet_server_dev_config`, `rocketnet_server_create`, `rocketnet_server_start`,
  `rocketnet_server_update`, `rocketnet_server_port`,
  `rocketnet_server_max_clients`, `rocketnet_server_client_connected`,
  `rocketnet_server_start_message`, `rocketnet_server_finish_message`,
  `rocketnet_server_receive_message`, `rocketnet_server_release_message`,
  `rocketnet_server_send_packets`, `rocketnet_server_disconnect_reason`,
  `rocketnet_server_client_stat`, `rocketnet_server_client_counter`,
  `rocketnet_server_destroy`
- `rocketnet_client_default_config`, `rocketnet_client_create`, `rocketnet_client_connect`,
  `rocketnet_client_state`, `rocketnet_client_state_name`,
  `rocketnet_client_index`, `rocketnet_client_update`,
  `rocketnet_client_start_message`, `rocketnet_client_finish_message`,
  `rocketnet_client_receive_message`, `rocketnet_client_release_message`,
  `rocketnet_client_send_packets`, `rocketnet_client_disconnect`,
  `rocketnet_client_disconnect_reason`, `rocketnet_client_stat`,
  `rocketnet_client_counter`, `rocketnet_client_destroy`
- `rocketnet_write_dev_connect_token` (development library only)
- `rocketnet_time`, `rocketnet_sleep`
