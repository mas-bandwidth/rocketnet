# Game loop

A 60 Hz fixed-step loop driving one server and N clients in one process, and
what it costs: client network calls per frame against a 100 microsecond
budget, the server's tick, and message latency in frames.

## What it shows

1. A fixed-step loop: accumulate `rocketnet_time()`, run one frame per 1/60 s
   of it, and `rocketnet_sleep` for the rest.
2. The frame order, on every client and on the server: (1) `*_update` takes in
   what arrived; (2) `*_receive_messages` takes every message in batches; (3) simulate;
   (4) queue this frame's messages with `*_start_message` and `*_finish_message`; (5)
   `*_send_packets` last, so this frame's messages leave this frame.
3. Each client sends its position every frame (`ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED`,
   channel 0) and an event twice a second (`ROCKETNET_RELIABILITY_RELIABLE_ORDERED`, channel
   1); the server sends each client a state update every frame. Each message
   carries the frame it was queued in, and the receiver counts frames since.

Every run is in memory over the `loopback` transport: the in-memory transport
holds one server and up to `ROCKETNET_MAX_CLIENTS` clients. Clients join 50 a frame; timing starts once
all are connected.

## Run it

From the repository root, after the normal build:

```sh
./bin/example_game_loop                      # 32 clients, 5 seconds
./bin/example_game_loop --wrong-order        # send_packets before queuing
./bin/example_game_loop --clients 1000 --seconds 30
```

The full-scale run holds 1000 clients in one process, all in memory over the
loopback transport. `--clients` outside 1..1000 prints a usage line and exits
1. On Windows the binary is `bin\Release\example_game_loop.exe`. The default
run prints `PASS` last; a failure prints one `FAIL: ...` line and exits 1.

## What the output means

```
32 clients, server transport loopback
[end] client frame p50 7.1 us, p99 24.3 us, max 313.6 us (budget 100 us); server tick mean 183.1 us, max 412.4 us
client frame budget held at p99 (informational, never a failure)
latency client->server frames: min 0 mean 0.00 max 0
latency server->client frames: min 1 mean 1.00 max 1
latency: 0 extra frame(s) (correct order)
PASS
```

A `[1s]` line prints every second and `[end]` covers the run. `client frame`
is one client's update, receive, queue and send calls in one frame: p50 is the
typical frame, p99 the slow one in a hundred. The budget verdict never fails
the run, because a shared machine's noise is not the library's cost. `server tick` is the server's frame over all clients.

The two `latency ... frames` lines give each direction's min, mean and max.
Clients run their frames before the server's, so the server reads a message in
the frame it was sent (0) and a client reads its state update no earlier than
the next frame (1): that is the delay of running in one process, and
`extra frame(s)` is what the frame order adds on top, read from client->server.
That direction has no receive handoff, so every message must read 0 in the correct order and 1 with
`--wrong-order`. Server->client passes through a background receive handoff;
moving that receive work off the frame thread lets data processing scale, with
extra frames under load as the chosen cost. Its min, mean and max expose that
cost, but worker scheduling has no fixed frame-count maximum. The causal
minimum is still fixed by the loop order: at least 1 frame in the correct order
and 2 in the wrong order. `PASS` means every client stayed connected, every
reliable event arrived, the client->server order had no unnecessary extra
frame, and no state update arrived before its causal minimum.

## Public calls it uses

- `rocketnet_init`, `rocketnet_term`
- `rocketnet_server_default_config`, `rocketnet_server_create`,
  `rocketnet_server_start`, `rocketnet_server_destroy`,
  `rocketnet_server_update`, `rocketnet_server_send_packets`,
  `rocketnet_server_ipv4_address`,
  `rocketnet_server_public_key`, `rocketnet_server_transport_name`,
  `rocketnet_server_start_message`, `rocketnet_server_finish_message`,
  `rocketnet_server_receive_messages`
- `rocketnet_client_default_config`, `rocketnet_client_create`,
  `rocketnet_client_connect`, `rocketnet_client_destroy`,
  `rocketnet_client_update`, `rocketnet_client_send_packets`,
  `rocketnet_client_state`, `rocketnet_client_start_message`,
  `rocketnet_client_finish_message`, `rocketnet_client_receive_messages`
- `rocketnet_create_dev_connect_token` and
  `ROCKETNET_DEV_BACKEND_PUBLIC_KEY` (development library only)
- `rocketnet_status_name`, `rocketnet_time`, `rocketnet_sleep`
