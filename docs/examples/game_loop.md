# Game loop

A 60 Hz fixed-step loop driving one server and N clients in one process, and
what it costs: client network calls per frame against a 100 microsecond
budget, the server's tick, and message latency in frames.

## What it shows

1. A fixed-step loop: accumulate `rocketnet_time()`, run one frame per 1/60 s
   of it, and `rocketnet_sleep` for the rest.
2. The frame order, on every client and on the server: (1) `*_update` takes in
   what arrived; (2) `*_receive` and `*_release` every message; (3) simulate;
   (4) queue this frame's messages with `*_start` and `*_finish`; (5)
   `*_send_packets` last, so this frame's messages leave this frame.
3. Each client sends its position every frame (`ROCKETNET_UNRELIABLE_SEQUENCED`,
   channel 0) and an event twice a second (`ROCKETNET_RELIABLE_ORDERED`, channel
   1); the server sends each client a snapshot every frame. Each message
   carries the frame it was queued in, and the receiver counts frames since.

Up to 63 clients run over the in-memory `loopback` transport. More run over UDP
on 127.0.0.1: the server on the platform's recommended transport (`auto`) and
each client on `portable`. Clients join 50 a frame; timing starts once all are
connected. The header at this base still names the message calls start,
finish, receive, release and send_packets; the lifecycle rename makes them
`*_start_message`, `*_finish_message`, `*_receive_message`,
`*_release_message` and `*_send_messages`.

## Run it

From the repository root, after the normal build:

```sh
./bin/example_game_loop                      # 32 clients, 5 seconds
./bin/example_game_loop --wrong-order        # send_packets before queuing
ulimit -n 4096; ./bin/example_game_loop --clients 1000 --seconds 30
```

The full-scale run holds 1000 clients in one process, and on Linux each
`portable` client holds two file descriptors: raise the limit first.
`--clients` outside 1..1000 prints a usage line and exits 1. On Windows the
binary is `bin\Release\example_game_loop.exe`. The default run prints `PASS`
last; a failure prints one `FAIL: ...` line and exits 1.

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
the run, because a shared machine's noise is not the library's cost. `server
tick` is the server's frame over all clients.

The two `latency ... frames` lines give each direction's min, mean and max.
Clients run their frames before the server's, so the server reads a message in
the frame it was sent (0) and a client reads its snapshot in the next frame
(1): that is the delay of running in one process, and `extra frame(s)` is what
the frame order adds on top. With `--wrong-order`, `send_packets` runs before
this frame's messages are queued, so they wait a frame: the lines read 1 and 2
and `latency: 1 extra frame(s) (wrong order)`. `PASS` means every client stayed
connected, every event arrived, and the extra latency is 0 in the correct order
and at least 1 in the wrong one.

## Public calls it uses

- `rocketnet_init`, `_term`, `_time`, `_sleep`, `_write_dev_connect_token` (development library only)
- `rocketnet_server_dev_config`, `_create`, `_port`, `_transport_name`,
  `_update`, `_client_connected`, `_receive`, `_release`, `_start`, `_finish`,
  `_send_packets`, `_destroy`
- `rocketnet_client_default_config`, `_create`, `_update`, `_state`,
  `_receive`, `_release`, `_start`, `_finish`, `_send_packets`, `_destroy`
