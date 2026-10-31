# Getting started

The first goal is a received message. Then you will run two processes over UDP
and learn the contracts your game loop must keep. All commands run from the
repository root. The examples use development credentials on your own machine.

## 1. Build and link

Install the [platform prerequisites](BUILDING.md#prerequisites). On Linux
or macOS:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DROCKETNET_DEV=ON
cmake --build build --target example_hello example_client_server_server example_client_server_client --parallel 4
./bin/example_hello
```

On Windows, in PowerShell or cmd.exe with the MSVC toolchain installed:

```powershell
cmake -S . -B build -A x64 -DROCKETNET_DEV=ON
cmake --build build --config Release --target example_hello example_client_server_server example_client_server_client --parallel 4
.\bin\Release\example_hello.exe
```

The important output is:

```text
server received: hello from the client
PASS
```

`PASS` follows connection, delivery, a second client connection, and clean
shutdown. Other lines are library diagnostics. [Hello](examples/hello.md)
uses an in-memory loopback hub, so this first check needs no network, port,
or backend service. It still waits for asynchronous progress.

To link your own program, follow [Installing and embedding](BUILDING.md#installing-and-embedding-the-sdk).
Use the [complete C integration example](INTEGRATION-EXAMPLE.md) if you want
one self-contained program. Excerpts below explain particular steps; they
are not separate complete programs.

## 2. Start a dedicated server

In one terminal, run:

```sh
./bin/example_client_server_server
```

Keep it running and start the client in the next step promptly. The server
binds UDP port 40000 and writes `client_server.keys` in its working directory.
This file contains its public address and public key, not its secret key.
The client must run from the same directory. On Windows use
`.\bin\Release\example_client_server_server.exe`.

The [server source](../examples/client_server_server.c) initializes the
library, fills a default config, selects one client slot and the portable
transport, creates a stopped server, and calls `rocketnet_server_start`.
Create and start are separate operations; check each result. Only after start
succeeds should a backend advertise the bound addresses.

## 3. Connect a client with a connect token

In a second terminal, from the same directory:

```sh
./bin/example_client_server_client
```

On Windows use `.\bin\Release\example_client_server_client.exe`.
The client reads `client_server.keys`, creates a development token for that
server, and connects. It sends one reliable ordered message, receives the
server's echo, disconnects, and prints `PASS`. The server also prints `PASS`
after serving the client and cleaning up. Both exit nonzero on failure.

A connect token is an opaque credential. A production client gets it from
your backend; it never holds the backend signing secret. Read [Sessions](SESSION.md)
for the handshake and [Keys](KEYS.md) for ownership.

Check the return from `rocketnet_client_connect` independently of the create
status. An accepted connect starts an attempt; it does not mean the handshake
has completed. Continue updating until the client state is `CONNECTED` or a
negative terminal state. [Hello's source](../examples/hello.c) shows separate
create and connect error paths; [troubleshooting](ERRORS-AND-LOGGING.md) explains
the status, state, and disconnect-reason distinctions.

## 4. Run the frame loop

Use this order on both endpoints:

1. **Update** with finite, non-decreasing time from one monotonic clock.
2. **Receive** every batch until receive returns `NULL`. Process or copy each
   message before another receive or update invalidates its data pointer.
3. **Simulate** the game.
4. **Queue** outgoing messages: reserve, check the pointer, write, then finish
   or abort. Keep only one reservation open per endpoint.
5. **Send packets** last, making this frame's queued work available for sending.

This order avoids an unnecessary frame of delay. It does not guarantee delivery
in the same update: background I/O and the network progress asynchronously.
Do not test delivery with a single immediate send/receive pair or a fixed sleep.
Observe the expected state or message within a bounded deadline.

Here is the receive portion of the hello example. The `batch` variable is
declared earlier in its frame loop; the returned data is used before the next call.

From [examples/hello.c](../examples/hello.c), lines 202 to 206:

```c
            while ( ( batch = rocketnet_server_receive_messages( server ) ) != NULL )
                for ( int i = 0; i < batch->num_messages; i++, received++ )
                {
                    printf( "server received: %.*s\n", batch->bytes[i], (const char *) batch->data[i] );
                }
```

The sender waits for `CONNECTED`, then checks available room and the actual
reservation. This fragment is inside that connected-state check:

From [examples/hello.c](../examples/hello.c), lines 213 to 224:

```c

                // reliable: defer to a later frame when there is no room; the frame bound above diagnoses a stuck defer

                if ( rocketnet_client_can_send_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, (int) strlen( message ) ) )
                {
                    uint8_t * p = rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, (int) strlen( message ) );
                    if ( p )
                    {
                        memcpy( p, message, strlen( message ) );
                        sent += rocketnet_client_finish_message( client, (int) strlen( message ) ) != 0;
                    }
                }
```

If preflight returns false, this example defers until a later frame. Your game
can defer reliable work or coalesce replaceable state. A true preflight does
not replace checking the returned pointer. Never call finish after a refused
start. See [Sending](USAGE.md#sending-messages) and
[Backpressure](USAGE.md#asking-whether-there-is-room).

## 5. Choose reliability modes and channels

Use unreliable sequenced delivery for replaceable state, such as a position
update, and reliable ordered delivery for events that must arrive in order
while the connection remains healthy. Ordering and sequencing are per channel;
choose separate channels for independent streams. Reliability cannot promise
delivery after a connection ends.

The [reliability example](examples/reliability.md) demonstrates all eight modes.
The [message contract](CHANNELS.md#reliability) explains fragmentation, ordering,
and receipts. RocketNet moves bytes; your application defines their format.

## 6. Shut down

Request `rocketnet_client_disconnect` or `rocketnet_server_stop`, then keep
polling `rocketnet_client_disconnected` or `rocketnet_server_stopped` from
your event loop. Continue updates when using a step-driven development client.
After completion, destroy the endpoint. Destroy the shared loopback hub after
all its endpoints, and call `rocketnet_term` after all objects are gone.

Keep each object and its allocator/callback contexts alive until destruction
completes. A client state of `DISCONNECTED` alone does not establish worker
completion. If a bounded wait expires, report that failure and retain the live
storage; a timeout is not permission to free memory still owned by I/O.
See [Connection lifecycle](USAGE.md#connection-lifecycle).

## 7. Threads

Make public calls on one endpoint from one thread at a time. Your application
owns any handoff between threads. Allocator and logging callbacks can run on
network threads and must satisfy the [threading contract](USAGE.md#threads).

<a id="step-1-build-the-production-library-and-backend-examples-rocketnet_dev0"></a>

<a id="step-2-provision-the-backend-signing-keypair-and-keep-key-boundaries"></a>

<a id="step-3-start-and-register-a-live-production-server"></a>

<a id="step-4-authenticate-a-player-mint-a-token-and-connect-the-c-client"></a>

<a id="step-5-handle-token-and-handshake-refusals-safely"></a>

## 8. Move from development tokens to a production backend (`ROCKETNET_DEV=0`)

Follow [Production integration](PRODUCTION.md) for the production build,
backend keys, authenticated server registration, token issuance, and refusal
handling. The example backend service needs application authentication before
deployment. Read [qualification](TESTING.md) before choosing a deployment size.

## If the examples do not finish

| Symptom | Check |
| --- | --- |
| Binary missing | Build the named target; Windows and other multi-config generators add `Release` to the output path. |
| Server cannot bind | Stop a previous instance or run the server with `--port 41234`; the client reads the new port from the key file. |
| Client cannot read keys or handshake times out | Run it from the server's directory while that server is alive. Recreate the key file by starting a fresh server. |
| Configuration unexpectedly changes or refuses | Inspect inherited `ROCKETNET_*` variables; documented environment values override config fields. See [Environment](ENVIRONMENT.md). |
| A call fails | Keep the status from that call and inspect its log. Do not report the earlier create status for a failed connect. See [Troubleshooting](ERRORS-AND-LOGGING.md). |

## Where to go next

- [Integration guide](USAGE.md): frame contracts, backpressure, receipts, lifecycle, configuration, and stats.
- [Game loop example](examples/game_loop.md): a fixed-step simulation and the cost of the wrong frame order.
- [Fixed memory example](examples/fixed_memory.md): size a server and a client up front and hold each to one region of that size.
- [Transports](TRANSPORTS.md): platform selection and deployment requirements.
- [Documentation index](README.md): all reading paths, references, and engineering records.
