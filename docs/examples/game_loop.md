# Game loop

A 60 Hz fixed-step loop driving one server and N clients in one process, and
what it costs: client network calls per frame against a 100 microsecond
budget, the server's tick, and message latency in frames.

## What it shows

1. A fixed-step loop: accumulate `rocketnet_time()`, run one frame per 1/60 s
   of it, and `rocketnet_sleep` for the rest.
2. The frame order, on every client and on the server: (1) `*_update` takes in
   what arrived; (2) `*_receive_messages` drains every message batch until `NULL`; (3) simulate;
   (4) queue this frame's messages with guarded `*_start_message` (checking the non-`NULL` pointer)
   and `*_finish_message` inside that branch; (5)
   `*_send_packets` last, making this frame's queued work available to the
   transport.
3. Each client sends its position every frame (`ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED`,
   channel 0) and an event twice a second (`ROCKETNET_RELIABILITY_RELIABLE_ORDERED`, channel
   1); the server sends each client a state update every frame. Each message
   carries the frame it was queued in, and the receiver counts frames since.

Every run is in memory over the `loopback` transport: the in-memory transport
holds one server and up to `ROCKETNET_MAX_CLIENTS` (`1000`) clients. Clients join 50 a frame; timing starts once
all are connected.

## Run it

From the repository root, after the normal build:

```sh
./bin/example_game_loop                      # 32 clients, 5 seconds
./bin/example_game_loop --wrong-order        # send_packets before queuing
./bin/example_game_loop --clients 1000 --seconds 30
```

The 1000-client command holds all clients in one process over loopback and
can require substantial memory. It is a profiling example, not qualification
of 1000 clients under every workload; see [Testing](../TESTING.md) and
[Memory](../MEMORY.md). `--clients` accepts `1..ROCKETNET_MAX_CLIENTS` (1000
in the default build); values outside that range are refused. On Windows the binary is `bin\Release\example_game_loop.exe`. The default
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

The two `latency ... frames` lines give each direction's min, mean, and max.
Clients run their frames before the server. The causal minimum is therefore
0 frames client-to-server and 1 server-to-client; `--wrong-order` adds one to
each minimum. Both directions pass through background handoffs, which can add
more frames. Neither direction has a fixed delivery frame count.

The printed `extra frame(s)` is the rounded mean client-to-server latency. It
includes worker scheduling, so it does not isolate the cost of call order.
`PASS` means every client stayed connected, all reliable events arrived, and
no measured delivery preceded its causal minimum. Timing percentiles and
additional handoff latency are informational, not pass thresholds. The sample
numbers above illustrate the output format; measure on the intended host.

<a id="public-calls-it-uses"></a>

Source: [`examples/game_loop.c`](../../examples/game_loop.c). See the
[integration guide](../USAGE.md) for shared lifecycle, threading, and message
ownership rules.
