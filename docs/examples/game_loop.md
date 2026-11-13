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
3. The server frame. By default (`--game-threads 1`) every server call is
   made on the main thread, one thread at a time, as the
   [integration guide](../USAGE.md) describes: `rocketnet_server_receive_messages`
   reads what the last update left, the world update runs, then the main
   thread queues every client's state update with
   `rocketnet_server_start_message` and `rocketnet_server_finish_message`,
   calls `rocketnet_server_send_packets`, and calls `rocketnet_server_update`.
   Nothing else is needed, and this is the form to start with.

   With `--game-threads N` the same frame runs the way a game server runs
   it, with the second half of the network update split off to run in
   parallel with the main thread:
   1. join: the main thread waits for last frame's split-off half, and owns
      the server again;
   2. a bit of networking on the main thread: `rocketnet_server_receive_messages`
      reads the messages the last update left into the game's state. This and
      the join are all rocketnet takes from the main thread;
   3. the world update on the main thread, which writes this frame's state;
   4. the second half of the network update is split off onto another thread
      and the main thread goes on with its frame. That thread runs, in
      order: every client's state update queued across the N game threads,
      each taking a range of clients and calling
      `rocketnet_server_start_message` then
      `rocketnet_server_finish_client_message` for its own clients (an
      unreliable message that fits a packet leaves from that call, on that
      thread); `rocketnet_server_send_packets`; then the update in three calls,
      `rocketnet_server_update_receive`, `rocketnet_server_update_client` for
      each client across the game threads, and
      `rocketnet_server_update_finish`.

   The server keeps two frames of state and swaps them at the frame boundary,
   so the main thread never writes the frame the other thread is sending
   from. The game threads here are a stand-in job system in
   [`examples/thread.h`](../../examples/thread.h): it runs a function over
   ranges of the clients on N platform threads and joins them. A game hands
   this work to its own job system on its own game threads instead; rocketnet
   owns no threads or job system. The server's threading rules are the
   rules in `sdk/rocketnet.h` for `rocketnet_server_t`: the main thread and
   the game threads never call at once, one thread sends to a client between
   two main thread calls, and the join is the barrier in both directions.

   The price of the split is up to one frame of send latency: 16.67 ms on
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
./bin/example_game_loop                           # 32 clients, 5 seconds, one thread
./bin/example_game_loop --game-threads 4          # the second half split off, 4 game threads
./bin/example_game_loop --wrong-order             # clients call send_packets before queuing
./bin/example_game_loop --clients 1000 --seconds 30 --game-threads 8
```

The 1000-client command holds all clients in one process over loopback and
can require substantial memory. It is a profiling example, not qualification
of 1000 clients under every workload; see [Testing](../TESTING.md) and
[Memory](../MEMORY.md). `--clients` accepts `1..ROCKETNET_MAX_CLIENTS` (1000
in the default build) and `--game-threads` accepts `1..64`; values outside
those ranges are refused. On Windows the binary is `bin\Release\example_game_loop.exe`. The default
run prints `PASS` last; a failure prints one `FAIL: ...` line and exits 1.

## What the output means

```
32 clients, 1 game thread(s), server transport loopback
[end] client frame p50 2.4 us, p99 13.6 us, max 74.6 us (budget 100 us)
server main thread in rocketnet mean 83.7 us, max 222.5 us; network half (on the main thread) mean 81.6 us, max 220.1 us
client frame budget held at p99 (informational, never a failure)
latency client->server frames: min 1 mean 1.99 max 2
latency server->client frames: min 1 mean 1.00 max 1
latency: 2 extra frame(s) (correct order)
PASS
```

and with `--game-threads 4`:

```
32 clients, 4 game thread(s), server transport loopback
[end] client frame p50 2.5 us, p99 13.5 us, max 88.2 us (budget 100 us)
server main thread in rocketnet mean 4.8 us, max 43.7 us; network half (on the game threads) mean 476.8 us, max 1139.5 us
client frame budget held at p99 (informational, never a failure)
latency client->server frames: min 1 mean 1.59 max 2
latency server->client frames: min 1 mean 1.00 max 1
latency: 2 extra frame(s) (correct order)
PASS
```

(Samples from a Linux bench.)

A `[1s]` line prints every second and `[end]` covers the run. `client frame`
is one client's update, receive, queue and send calls in one frame: p50 is the
typical frame, p99 the slow one in a hundred. The budget verdict never fails
the run, because a shared machine's noise is not the library's cost.
`server main thread in rocketnet` is what the server's frame takes from the
main thread: the join and the receive of steps 1 and 2, and, with one game
thread, the whole network half too. `network half` is the queuing of every
client's state update, the send and the update, timed where it runs: on the
main thread by default, on the game threads in parallel with the main thread
with `--game-threads N`. The second sample above shows the point of the
split: the main thread's share falls to the join and the receive.

The two `latency ... frames` lines give each direction's min, mean, and max.
Clients run their frames before the server, and the server's update runs in
the second half of its frame. The causal minimum is therefore 1 frame
client-to-server (the update takes a message in during the frame it was sent
in, and the main thread reads it at the start of the next) and 1 frame
server-to-client (the clients run first, so they read a state update the next
frame). `--wrong-order` adds one to the client-to-server minimum. Both
directions pass through background handoffs, which can add more frames, and
with game threads the second half can finish after the next frame's clients
have run. Neither direction has a fixed delivery frame count.

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
