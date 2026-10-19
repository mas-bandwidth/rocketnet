# Game loop

A 60 Hz server and N clients in one process over the in-memory `loopback`
transport. It shows the frame loop of a real game: update, receive and
release, simulate, queue, then send last so this frame's messages leave this
frame.

## What it shows

1. Create a server and N clients, each with its own development connect token.
2. Every frame each client sends an unreliable-sequenced state packet and a
   reliable-ordered event now and then; the server echoes each client a
   snapshot.
3. It measures each client's per-frame network cost (the time around that
   client's update, send and receive calls) and reports p50, p99 and max in
   microseconds against the 100 µs budget, plus the server's per-tick time.
4. It checks the frame order by measuring end-to-end message latency in
   frames: a message queued in frame N leaves in frame N and is received the
   first frame the other side's update sees it. The correct order shows 0
   extra frames; `--wrong-order` sends before queueing so every message waits
   one frame and shows 1.
5. It passes when every client stayed connected and every event arrived. The
   budget is reported but does not fail the run (CI runners are noisy).

## Run it

From the repository root, after the normal build:

```sh
./bin/example_game_loop
./bin/example_game_loop --clients 1000 --seconds 30
./bin/example_game_loop --wrong-order
```

The full-scale command runs 1000 clients for 30 seconds. `--clients` must be
1..1000 (default 32); outside that is refused with a usage line and a nonzero
exit. `--seconds` lengthens the run (default 5).

## What the output means

```
per-client per-frame p50=3us p99=32us max=1087us (budget 100us held)
server_tick=38us
latency: 0 extra frame(s) (correct order)
PASS
```

`p50`/`p99`/`max` are the per-client per-frame network cost in microseconds;
`server_tick` is the server's mean per-tick time. The `latency` line is the
frame-order check: `--wrong-order` reports 1 here. The example fails only if
the correct order shows an extra frame or `--wrong-order` shows none.

## Public calls it uses

- `rocketnet_init`, `rocketnet_term`
- `rocketnet_server_dev_config`, `rocketnet_server_create`,
  `rocketnet_server_update`, `rocketnet_server_port`,
  `rocketnet_server_client_connected`, `rocketnet_server_receive`,
  `rocketnet_server_release`, `rocketnet_server_start`,
  `rocketnet_server_finish`, `rocketnet_server_send_packets`,
  `rocketnet_server_destroy`
- `rocketnet_client_default_config`, `rocketnet_client_create`,
  `rocketnet_client_state`, `rocketnet_client_update`,
  `rocketnet_client_receive`, `rocketnet_client_release`,
  `rocketnet_client_start`, `rocketnet_client_finish`,
  `rocketnet_client_send_packets`, `rocketnet_client_disconnect`,
  `rocketnet_client_destroy`
- `rocketnet_write_dev_connect_token` (development library only)
- `rocketnet_time`, `rocketnet_sleep`
