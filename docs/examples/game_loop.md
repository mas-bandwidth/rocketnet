# Game loop

A 60 Hz fixed-step loop driving one server and N clients in one process, and
what it costs: client network calls per frame against a 100 microsecond
budget, what the server's frame takes from its main thread, and message
latency in frames.

## What it shows

1. A fixed-step loop: accumulate `rocketnet_time()`, run one frame per 1/60 s
   of it, and `rocketnet_sleep` for the rest.
2. The client frame order: (1) `rocketnet_client_update` takes in what
   arrived; (2) `rocketnet_client_receive_messages` drains every message batch
   until `NULL`; (3) simulate; (4) queue this frame's messages with guarded
   `rocketnet_client_start_message` (checking the non-`NULL` pointer) and
   `rocketnet_client_finish_message` inside that branch; (5)
   `rocketnet_client_send_packets` last, making this frame's queued work
   available to the transport.
3. The server frame, the way a game server runs it, with the second half of
   the network update split off to run in parallel with the main thread:
   1. join: the main thread waits for last frame's split-off half, and owns
      the server again;
   2. a bit of networking on the main thread: `rocketnet_server_send_packets`
      puts last frame's state updates on the wire, `rocketnet_server_update`
      takes in what arrived, and `rocketnet_server_receive_messages` is
      drained into the game's state;
   3. the world update on the main thread, which writes this frame's state;
   4. the second half of the network update is split off onto another thread
      (`examples/thread.h`, the platform's own threads): it queues every
      client's state update with `rocketnet_server_start_message` and
      `rocketnet_server_finish_message`, while the main thread goes on with
      its frame. It is the server's only user until the join.

   The server keeps two frames of state and swaps them at the frame boundary,
   so the main thread never writes the frame the other thread is sending
   from. A game hands the split-off half to its job system on its game
   threads; the example uses one platform thread so it depends on nothing
   else. Each server is used from one thread at a time
   ([USAGE](../USAGE.md)): the join is what hands it back.

   The price is one frame of added send latency: a state update queued in
   frame F goes on the wire at the start of frame F + 1, 16.67 ms later on
   average at 60 Hz.
4. Each client sends its position every frame (`ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED`,
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
./bin/example_game_loop --wrong-order        # clients call send_packets before queuing
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
[end] client frame p50 8.7 us, p99 21.6 us, max 62.2 us (budget 100 us)
server main thread in rocketnet mean 356.4 us, max 491.4 us; split-off half mean 25.8 us, max 111.6 us
client frame budget held at p99 (informational, never a failure)
latency client->server frames: min 0 mean 0.73 max 1
latency server->client frames: min 2 mean 2.00 max 2
latency: 1 extra frame(s) (correct order)
PASS
```

A `[1s]` line prints every second and `[end]` covers the run. `client frame`
is one client's update, receive, queue and send calls in one frame: p50 is the
typical frame, p99 the slow one in a hundred. The budget verdict never fails
the run, because a shared machine's noise is not the library's cost.
`server main thread in rocketnet` is what the server's frame takes from the
main thread: the join, send, update and receive of steps 1 and 2.
`split-off half` is the other thread's queuing of every client's state
update, in parallel with the main thread.

The two `latency ... frames` lines give each direction's min, mean, and max.
Clients run their frames before the server. The causal minimum is therefore
0 frames client-to-server and 2 server-to-client: 1 because the clients run
first, and 1 for the split-off half, whose state updates go on the wire at the
start of the next server frame. `--wrong-order` adds one to the
client-to-server minimum. Both directions pass through background handoffs, which can add
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
