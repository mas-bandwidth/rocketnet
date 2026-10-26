# Stats

How a game's debug overlay reads connection quality: a server and a client in
one process over the in-memory `loopback` transport. The client connects,
exchanges a little traffic, then prints the client's and the server's state and
counters. Each stats call fills a caller-owned struct. Client quality stats are
computed synchronously; the server per-client call copies its latest published
ACK-derived quality values without waiting for their background calculation.

## What it shows

1. Initialize the library and create a server on the `loopback` transport,
   with `ROCKETNET_DEV_BACKEND_PUBLIC_KEY` copied into the config. Create
   generates the server's ephemeral sealing keypair.
2. After start, read the server public key and bound address, create an opaque
   base64 development token with `rocketnet_create_dev_connect_token`, and
   connect a client with that string.
3. Run frames until the client reports `ROCKETNET_CLIENT_STATE_CONNECTED`.
4. Exchange traffic for about one and a half seconds: the client sends a
   reliable-ordered message each frame and the server echoes one back, so both
   directions have traffic to measure.
5. Print the client's state name, then RTT, jitter, packet loss, bandwidth sent
   and received, and a few counters (`api_misuses` among them) from one
   `struct rocketnet_client_stats_t` filled by `rocketnet_client_stats`.
6. Print the server's per-client view of the same connection from one
   `struct rocketnet_server_client_stats_t` filled by
   `rocketnet_server_client_stats`, then the server-wide totals from one
   `struct rocketnet_server_stats_t` filled by `rocketnet_server_stats`.
7. Disconnect, then print how the connection ended on both ends: the client's
   state through `rocketnet_client_state` (a negative state names how the
   server or the protocol ended a connection; after the client's own
   disconnect it is `disconnected`) and the server's reason for the slot
   through `rocketnet_server_disconnect_reason`.

Each frame runs the calls in the order a game does: update, drain the one
receive queue, simulate, queue, then send last. Sending last is what makes this
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
per-client view and totals, then the client's state and the server's reason
after the disconnect, then `PASS`. The check passes
when the numbers are present and sane: RTT, jitter, loss and bandwidth are
never negative, and bytes sent are above zero after the traffic exchange. `PASS`
means the client connected, traffic flowed both ways, and the disconnect was
seen on both ends. The informational log lines before the output come from the
library and are not part of the check.

Both endpoints use the same ACK-based connection-quality definitions. Client
loss estimates its outgoing packets to the server; a server slot estimates
its outgoing packets to that client. The values need not match on asymmetric
paths. This is an unacknowledged-packet estimate, not a message failure rate:
delayed or lost returning ACKs can affect it, and later ACKs can reduce it.

Loss excludes the newest half of the sequence-history window, including during
startup, and ignores sequence positions never recorded as sent. With the default
4096-position history, no sends are eligible until the highest sent sequence
exceeds 2048. Zero therefore also means that there are no eligible samples.
Jitter is the mean excess of retained positive first-ACK round-trip samples over
their minimum, in milliseconds. It includes the remote endpoint's ACK scheduling and caller
update cadence. Zero-time samples are excluded from both the minimum and mean.
These histories do not expire with elapsed time; reconnect resets them. Client
stats recompute on read. Server-slot stats are always current: each slot keeps
running aggregates that every send, receive and ack updates in O(1) (the rtt
minimum in O(log n)), equal at every read to a full recompute over the retained
history, so the caller's server-slot stats call copies values that already
include the latest ack, and an idle slot costs nothing. Server update does no
statistics pass and runs no statistics thread. A reset clears the slot's values
with the rest of its connection state (sdk/rocketnet_ack.cpp,
tla/RNServerStats.tla).

The final API changes the client's previous receive-direction, one-second loss
estimate and smoothed arrival-gap jitter to these shared definitions. Existing
dashboard directions and alert thresholds must be reviewed; historical values
are not directly comparable. Field names, units, struct layouts and the wire
format are unchanged.

## Public calls it uses

- `rocketnet_init`, `rocketnet_term`
- `rocketnet_server_default_config`, `rocketnet_server_create`,
  `rocketnet_server_start`, `rocketnet_server_destroy`,
  `rocketnet_server_update`, `rocketnet_server_send_packets`,
  `rocketnet_server_ipv4_address`,
  `rocketnet_server_public_key`, `rocketnet_server_client_connected`,
  `rocketnet_server_disconnect_reason`, `rocketnet_server_can_send_message`, `rocketnet_server_start_message`,
  `rocketnet_server_finish_message`, `rocketnet_server_receive_messages`,
  `rocketnet_server_stats`, `rocketnet_server_client_stats`
- `rocketnet_client_default_config`, `rocketnet_client_create`,
  `rocketnet_client_connect`, `rocketnet_client_destroy`,
  `rocketnet_client_update`, `rocketnet_client_send_packets`,
  `rocketnet_client_disconnect`, `rocketnet_client_state`,
  `rocketnet_client_state_name`, `rocketnet_client_index`,
  `rocketnet_client_can_send_message`, `rocketnet_client_start_message`, `rocketnet_client_finish_message`,
  `rocketnet_client_receive_messages`, `rocketnet_client_stats`
- `rocketnet_create_dev_connect_token` and
  `ROCKETNET_DEV_BACKEND_PUBLIC_KEY` (development library only)
- `rocketnet_status_name`, `rocketnet_time`, `rocketnet_sleep`,
  `rocketnet_disconnect_reason_name`
