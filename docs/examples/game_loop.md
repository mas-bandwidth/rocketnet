# Game loop

A game's shape around RocketNet: a launcher that runs a server, a client or
both, and a 60 Hz frame loop on each side whose network step runs in parallel
with the next frame.

```
examples/game_loop/
    launcher.cpp   main: listen mode (server and clients in one process), server, client
    server.cpp     server_main: the server's frame loop and network step
    client.cpp     client_main: the client's frame loop and network step
    game.h         what they share: the message, the frame pacing, the wire check
```

## What it shows

1. The launcher's three modes. With no mode it is a listen server: the server
   and the clients run in one process, each on its own thread, over the
   in-memory `loopback` transport. `server` runs a dedicated server over UDP;
   `client` runs a client against it.
2. The frame loop, the same on both sides:

   ```cpp
   std::thread network( server_network_step, &s, &frames[current ^ 1], rocketnet_time() );
   server_simulate( &s, &frames[current], frame );
   network.join();
   current ^= 1;
   wait_for_frame_end( frame_end );
   ```

   The network step for what the last frame produced runs on its own thread
   while this frame simulates; the frame joins it, then swaps the two buffers.
   The network step queues and sends what the last frame wrote
   (`*_start_message`, `*_finish_message`, `*_send_packets`), then updates and
   takes every message (`*_update`, `*_receive_messages`) into the buffer the
   next frame reads. The simulation never calls RocketNet, and RocketNet is
   called from one thread at a time. The parallelism is the game's: RocketNet
   runs no thread of its own here.
3. Each client sends an input every frame (`ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED`,
   channel 0); the server sends each client its state every frame.

## Run it

From the repository root, after the normal build:

```sh
./bin/example_game_loop                          # listen mode: a server and 1 client, 5 seconds
./bin/example_game_loop --clients 64 --seconds 30
./bin/example_game_loop server                   # a dedicated server on 127.0.0.1:40000, over UDP
./bin/example_game_loop client                   # in the same directory, once the server is up
```

`--port <n>` or `ROCKETNET_EXAMPLE_PORT` moves the dedicated server and its
client off 40000. The server writes its public key to `game_loop.keys`, as it
would register it with a backend, and the client reads it to mint its
development token. On Windows the binary is `bin\Release\example_game_loop.exe`.
Each mode prints `PASS` last; a failure prints one `FAIL: ...` line and exits 1.

## What the output means

```
1 clients and the server on their own threads, transport loopback
server network step mean 21.4 us, max 135.6 us over 316 frames
client 0 network step mean 9.0 us, max 53.2 us over 316 frames
299 inputs and 297 state updates; 0 not taken by the receiver's next update
PASS
```

A network step is the RocketNet work of one frame on one side: queue, send,
update and receive. It is informational, never a failure, because a shared
machine's noise is not the library's cost.

The last line is the listen mode's check, and it is exact. After its
`send_packets` returns, each side records the last frame it sent; at the start
of its update, the other side reads that record. Every message sent before an
update began must be taken by that update, and the run fails on one that is
not. The loopback transport makes a send visible before `send_packets` returns
and the receiver's next update takes it (`tla/RNLoopbackWire.tla`), so no frame
is lost to the handoff between the two threads.
