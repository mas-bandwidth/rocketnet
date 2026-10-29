# AGENTS.md: Guide for AI Coding Agents Integrating RocketNet

This guide is the technical reference for an AI coding agent integrating RocketNet into a C or C++ game client and dedicated server. Every function, struct, macro, enum, and configuration variable named here matches [sdk/rocketnet.h](sdk/rocketnet.h).

For the complete documentation path across this repository:

- [README.md](README.md) — entry point, minimal lifecycle skeleton, and documentation map
- [docs/GETTING-STARTED.md](docs/GETTING-STARTED.md) — step-by-step walkthrough backed by runnable examples ([hello](docs/examples/hello.md), [client_server](docs/examples/client_server.md), [game_loop](docs/examples/game_loop.md), [transports](docs/examples/transports.md))
- [USAGE.md](USAGE.md) — configuration, memory sizing, receive buffer sizing, the five-step frame loop, sending/receiving, backpressure, receipts, stats, and connection lifecycle
- [TRANSPORTS.md](TRANSPORTS.md) — transport selection (`auto`, pin, candidate list), platform availability, and privilege requirements
- [docs/SESSION.md](docs/SESSION.md) and [KEYS.md](KEYS.md) — connect tokens, keys, session isolation, and reconnect rules
- [sdk/rocketnet.h](sdk/rocketnet.h) — authoritative C header for every identifier and contract

- [Before production](#before-production)
- [What RocketNet is](#what-rocketnet-is)
- [Building and linking on Linux, Windows and macOS](#building-and-linking-on-linux-windows-and-macos)
- [The integration loop](#the-integration-loop)
- [Choosing a transport](#choosing-a-transport)
- [The eight channel types](#the-eight-channel-types)
- [Connect tokens and security](#connect-tokens-and-security)
- [Errors and refusals](#errors-and-refusals)
- [Threading rules](#threading-rules)
- [Testing an integration](#testing-an-integration)
- [Common mistakes](#common-mistakes)
- [Zero-copy messages](#zero-copy-messages)

---

## Before production

A default build is a development build (`ROCKETNET_DEV` is 1). Before you deploy:

1. **Build with `ROCKETNET_DEV=0`.** CMake: `-DROCKETNET_DEV=OFF` (or `make ROCKETNET_DEV=OFF`); compiling the sources yourself: `#define ROCKETNET_DEV 0` (`-DROCKETNET_DEV=0`). Link `RocketNet::rocketnet` (or the `rocketnet` target) from that build, never the development library target. See [BUILDING.md, "Production builds"](BUILDING.md#production-builds).
2. **Run your own backend that mints connect tokens.** Write it in Go, C# or any language, following `examples/backend/go` and `examples/backend/csharp`: it holds the backend secret key, signs each token, and hands the client the token string. Any implementation works if it writes the token format in [standard/TRANSPORT.md, t6 Connect token](standard/TRANSPORT.md#t6-connect-token). Production clients never mint their own tokens.
3. **Never ship the development values.** Development connect tokens, the published development keys and the published test packet filter are for development only, and a `ROCKETNET_DEV=0` build refuses them: the test packet filter at client and server create (`ROCKETNET_STATUS_TEST_PACKET_FILTER_IN_PRODUCTION`), the development keys at server start (`ROCKETNET_STATUS_DEV_KEYS_IN_PRODUCTION`), and a token sealed to the development server key at client connect (`ROCKETNET_STATUS_DEV_CONNECT_TOKEN_IN_PRODUCTION`). Set your title's own 4-byte `packet_filter` (`ROCKETNET_PACKET_FILTER_BYTES`), nonzero `protocol_id`, and 32-byte backend public key (`ROCKETNET_BACKEND_PUBLIC_KEY_BYTES`).
4. **Set keys and security settings from environment variables at deploy time, never in code.** Every config field has its variable (`ROCKETNET_SERVER_BACKEND_PUBLIC_KEY`, `ROCKETNET_SERVER_PACKET_FILTER`, `ROCKETNET_CLIENT_PACKET_FILTER` and the rest), and the environment overrides the config struct.
5. **Choose a supported production transport configuration and handle production status codes.** Leave `transport` at `"auto"` (`NULL`) or supply an explicit candidate list (`"xdp,gso,io_uring,portable"` on Linux, `"msgx,portable"` on macOS, `"iocp,rio,portable"` on Windows); `"portable"` remains supported on both client and server in production builds. Check and handle every negative `enum rocketnet_status_t` from `rocketnet_init`, `rocketnet_client_create`, `rocketnet_server_create`, `rocketnet_client_connect`, and `rocketnet_server_start`.
6. **Qualify server scale within the demonstrated 1000-player bound.** RocketNet's demonstrated server configuration covers up to `max_clients = 1000` (with the existing 4096-byte XDP frame/ring configuration) under its stated workload, topology, and deployment conditions. Above 1000 clients, applications must research, stabilize, and qualify their own configuration.

---

## What RocketNet is

RocketNet is a low-level, connection-oriented client/server UDP transport and message library written in C++ with a pure C public interface:

- **Frame-time target:** Main frame calls aim for less than 100 microseconds
  per call in Release builds. This target is not yet established under a
  1000-client load. Create may take longer. Client disconnect and server stop
  request shutdown without waiting for workers. Poll client disconnected or
  server stopped before synchronous destroy joins and frees resources. An early
  destroy logs and counts the misuse, then completes the shutdown and join
  itself with bounded updates and no sleep; a worker that has not exited is
  logged and its object kept, not freed. An early destroy on a threaded transport normally keeps the object, and a second destroy after completion frees it. Long soak, load and profiling runs remain part of hardening.

- **One public header:** [sdk/rocketnet.h](sdk/rocketnet.h). Include it directly from C or C++ (`#include "rocketnet.h"`); the header provides its own `extern "C"` guards when compiled as C++.
- **One link library:** `RocketNet::rocketnet` (or the `rocketnet` CMake target when embedded via `add_subdirectory`).
- **Version macros:** `ROCKETNET_VERSION` is one comparable integer, `ROCKETNET_VERSION_MAJOR * 10000 + ROCKETNET_VERSION_MINOR * 100 + ROCKETNET_VERSION_PATCH` (`10000` for 1.0.0, and for v1.0.0-preview-3, since the prerelease is not in the integer; a development build uses `255` for each part). The text form is `ROCKETNET_VERSION_FULL` (`"v1.0.0-preview-3"` for this release, or `"dev"` in a development build). `rocketnet_version()` returns the integer the library was built with; compare it with `ROCKETNET_VERSION` to catch a header and a shared library from different releases.
- **Demonstrated 1000-client server configuration (`ROCKETNET_MAX_CLIENTS`):** The default and demonstrated server configuration supports up to `1000` clients (`0 .. max_clients - 1`) under its stated workload, topology, and deployment conditions (preserving the existing 4096-byte XDP frame/ring configuration). Above 1000 clients, users must research, stabilize, and qualify their own configuration. Clients are addressed by an `int` slot index `0 .. max_clients - 1`; `rocketnet_client_index` returns the client's own index while connected and `-1` otherwise, and only `rocketnet_server_start_message` takes a `uint64_t` destination id (a client index today, a flow id with `ROCKETNET_FLOW_ID_BIT` set later). Create allocates each client and server as one block through the allocator (`rocketnet_client_required_memory` and `rocketnet_server_required_memory` give the lifecycle peak, aligned to `ROCKETNET_ALLOCATION_ALIGNMENT`). Connect and start then allocate the transport through the same allocator, before any traffic: sockets, worker threads, I/O rings and buffers, and on the server its per-client protocol tables. All allocation happens up front at create, connect and start; update, receive, start/finish/abort message and send packets never allocate.
- **Up to 32 channels (`ROCKETNET_MAX_CHANNELS`) and 8 delivery modes:** Every message is sent on a channel `0 .. num_channels - 1` with one of eight reliability modes.

---

## Building and linking on Linux, Windows and macOS

Full build instructions live in [BUILDING.md](BUILDING.md).

### Prerequisites

- **Linux (Ubuntu 24.04):**
  ```sh
  sudo apt update
  sudo apt install -y cmake build-essential clang libbpf-dev libxdp-dev liburing-dev xxd
  ```
- **Linux (Ubuntu 22.04, stock libraries, XDP enabled):**
  ```sh
  sudo apt update
  sudo apt install -y cmake build-essential clang libbpf-dev liburing-dev xxd
  ```
  AF_XDP is supplied by libbpf 0.5 on this distribution; there is no
  `libxdp-dev` package. Build/link compatibility does not establish runtime
  qualification on the original 5.15 kernel or a particular NIC.
- **macOS:**
  ```sh
  brew install cmake
  ```
- **Windows:** MSVC (`cl.exe`, v18 / 2026 toolchain) and CMake 4.2 or later.

RocketNet vendors a pinned copy of libsodium under `sodium/` and compiles it directly into the library target, so no external libsodium package is required on any platform.

### Building and installing

On Linux and macOS:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
cmake --install build --prefix /path/to/prefix
```

On Windows (x64):

```powershell
cmake -B build -A x64
cmake --build build --config Release --parallel 4
cmake --install build --config Release --prefix C:/path/to/prefix
```

### Linking from CMake

When consuming an installed prefix:

```cmake
find_package(RocketNet 1.0 CONFIG REQUIRED)
target_link_libraries(game PRIVATE RocketNet::rocketnet)
```

Pass `-DCMAKE_PREFIX_PATH=/path/to/prefix` when configuring the consuming project. The `rocketnet` target is always static; `BUILD_SHARED_LIBS` does not change it. See [BUILDING.md](BUILDING.md) for the shared-library target and linking instructions.

When embedding the source tree with `add_subdirectory`:

```cmake
add_subdirectory(external/rocketnet)
target_link_libraries(game PRIVATE rocketnet)
```

---

## The integration loop

A RocketNet integration follows a strict lifecycle and per-frame call order:

1. **Initialize the library once per process:** Call `rocketnet_init(NULL, NULL, NULL)`; it returns `ROCKETNET_STATUS_OK` on success or a negative `rocketnet_status_t` refusal. The second and third arguments are the allocator's malloc and free functions; NULL for both is the system allocator, and passing only one is refused with `ROCKETNET_STATUS_INVALID_ARGUMENT`.
2. **Create the server and client:**
   - Fill `struct rocketnet_server_config_t` with `rocketnet_server_default_config(&server_config)`, set `protocol_id` and `backend_public_key` into it (a development build can use `ROCKETNET_DEV_BACKEND_PUBLIC_KEY`), call `rocketnet_server_create` with the address and a status out-parameter (it writes the refusal's code on NULL; `rocketnet_status_name` names it), then call `rocketnet_server_start(server)`. Create generates a fresh server keypair and starts stopped; start binds the transport and accepts clients. Read `rocketnet_server_public_key` and the served IPv4/IPv6 addresses, then register them with the backend over an authenticated channel before it issues tokens.
   - Fill `struct rocketnet_client_config_t` with `rocketnet_client_default_config(&client_config)`, call `rocketnet_client_create` with a status out-parameter (it writes the refusal's code on NULL), then call `rocketnet_client_connect(client, connect_token)` with an opaque NUL-terminated base64 connect-token string. The client connects using the token alone; the handshake begins on the first `rocketnet_client_update`.
3. **Run the five-step frame loop in exact order every tick** (using the caller's monotonic `double time` in seconds):
   - **Step 1 — Update:** Call `rocketnet_client_update(client, time)` and `rocketnet_server_update(server, time)` first to pump the transport, process incoming packets and ACKs, and advance connection state.
   - **Step 2 — Receive:** Drain the one receive queue with `rocketnet_client_receive_messages` / `rocketnet_server_receive_messages`. Each call returns one `rocketnet_message_batch_t`; keep calling until it returns `NULL`. Process each entry's client index, channel, reliability, byte count and data pointer before the next receive or update on that endpoint. Draining is required: a message still in the queue at the next update is API misuse, which asserts in a debug build, counts `MESSAGE_NOT_RECEIVED`, and ends that connection (`ROCKETNET_DISCONNECT_REASON_MESSAGE_NOT_RECEIVED` for the server's slot, `ROCKETNET_CLIENT_STATE_MESSAGE_NOT_RECEIVED` for the client). Calling receive once per frame is not enough: one update can deliver more than one batch. Read ACK/loss receipts with `rocketnet_client_receipts` / `rocketnet_server_receipts`.
   - **Step 3 — Simulate:** Run the game simulation for the frame.
   - **Step 4 — Queue outgoing messages:** Reserve space in place with `rocketnet_client_start_message` / `rocketnet_server_start_message`, check that the returned pointer is non-`NULL`, write the message bytes directly into the returned pointer inside that branch, and commit with `rocketnet_client_finish_message` / `rocketnet_server_finish_message` (or cancel with `rocketnet_client_abort_message` / `rocketnet_server_abort_message`) inside the same non-`NULL` branch:
     ```c if ( rocketnet_client_can_send_message( client, channel, reliability, max_bytes ) ) { uint8_t * out = rocketnet_client_start_message( client, channel, reliability, max_bytes ); if ( out ) { /* write 1 .. max_bytes into out */ uint32_t serial = rocketnet_client_finish_message( client, bytes_written ); (void) serial; } } /* else skip or defer by application policy */ ```
     Calling `finish_message` without a successful outstanding `start_message` is an error and returns `0`. Optional preflight with `rocketnet_client_can_send_message` / `rocketnet_server_can_send_message` answers whether `start_message` would be granted room now without ending the connection on `MESSAGE_MEMORY_EXHAUSTED`, while `start_message` remains the authoritative reservation result (always keep the `if ( out )` check). What the application does when `can_send_message` returns `false`—skipping, deferring, dropping, or coalescing a send—is application policy, not library behavior. To send one message to every connected client, loop over the slots with `rocketnet_server_client_connected` and run a guarded `start_message` / `finish_message` for each.
   - **Step 5 — Send packets last:** Call `rocketnet_client_send_packets(client)` and `rocketnet_server_send_packets(server)` **last** so every message queued during the frame goes out on the wire in the same frame instead of waiting until the next tick.
4. **Request shutdown, check completion, then destroy:** Call `rocketnet_client_disconnect` and `rocketnet_server_stop`. Keep your event loop running and poll `rocketnet_client_disconnected` (stepping `rocketnet_client_update` on step-driven clients) and `rocketnet_server_stopped`. After the corresponding query returns true, `rocketnet_client_destroy` or `rocketnet_server_destroy` synchronously joins and frees the object (and before reconnecting a client, wait until `rocketnet_client_disconnected` is true so the new connection starts with fresh session keys, replay window, sequence numbers, and message queues). Destroy asserts this precondition; called early, it logs one error naming the skipped completion function, records the misuse, then requests shutdown, updates a bounded number of times without sleeping, and performs the join and free itself once complete; if the worker has not exited it logs a second error and keeps the object rather than free memory the worker owns. Keep allocator and callback contexts alive through destroy, then call `rocketnet_term()` after every object has been freed. Stop/disconnect and their queries do not wait for workers; a stuck worker leaves completion false. A newly created object that has never run is already complete.

### Compilable loopback integration example

Link this example with the development library, which supplies
`ROCKETNET_DEV=1`. The development key and token helper are available
only in that build. Production clients receive token strings from a backend.

<!-- check_agents_md:example:begin -->
```c
#include "rocketnet.h"

#include <stdint.h>
#include <string.h>
#include <time.h>

int main( void )
{
    if ( rocketnet_init( NULL, NULL, NULL ) != ROCKETNET_STATUS_OK )
        return 1;

    int result = 1;
    struct rocketnet_server_t * server = NULL;
    struct rocketnet_client_t * client = NULL;
    static const uint8_t dev_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES] = ROCKETNET_DEV_BACKEND_PUBLIC_KEY;
    const uint64_t protocol_id = 0x1122334455667788ULL;
    struct rocketnet_server_config_t server_config;
    struct rocketnet_client_config_t client_config;
    if ( rocketnet_server_default_config( &server_config ) != ROCKETNET_STATUS_OK ||
         rocketnet_client_default_config( &client_config ) != ROCKETNET_STATUS_OK )
        goto cleanup;
    server_config.protocol_id = protocol_id;
    server_config.max_clients = 8;
    server_config.transport = "loopback";
    memcpy( server_config.backend_public_key, dev_key, sizeof( dev_key ) );
    client_config.transport = "loopback";

    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
    server = rocketnet_server_create( NULL, "127.0.0.1:40000", NULL, &server_config, &status );
    if ( !server || status != ROCKETNET_STATUS_OK ||
         rocketnet_server_start( server ) != ROCKETNET_STATUS_OK )
        goto cleanup;

    uint8_t public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    char address[ROCKETNET_MAX_ADDRESS_STRING_BYTES];
    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    rocketnet_server_public_key( server, public_key );
    if ( !rocketnet_server_ipv4_address( server, address, sizeof( address ) ) )
        goto cleanup;
    const time_t now = time( NULL );
    if ( now == (time_t) -1 || rocketnet_create_dev_connect_token(
             address, NULL, public_key, 60, 15, 42, protocol_id,
             (uint64_t) now, NULL, token ) != ROCKETNET_STATUS_OK )
        goto cleanup;

    client = rocketnet_client_create( NULL, &client_config, &status );
    if ( !client || status != ROCKETNET_STATUS_OK ||
         rocketnet_client_connect( client, token ) != ROCKETNET_STATUS_OK )
        goto cleanup;

    double time = 1000.0;
    const double dt = 1.0 / 60.0;
    int client_sent = 0;
    int client_received_reply = 0;
    for ( int tick = 0; tick < 300 && !client_received_reply; tick++, time += dt )
    {
        rocketnet_client_update( client, time );
        rocketnet_server_update( server, time );

        const struct rocketnet_message_batch_t * batch = NULL;
        while ( ( batch = rocketnet_client_receive_messages( client ) ) != NULL )
            for ( int i = 0; i < batch->num_messages; i++ )
                if ( batch->bytes[i] == 4 && memcmp( batch->data[i], "pong", 4 ) == 0 )
                    client_received_reply = 1;

        int reply_index = -1;
        while ( ( batch = rocketnet_server_receive_messages( server ) ) != NULL )
            for ( int i = 0; i < batch->num_messages; i++ )
                if ( batch->bytes[i] == 4 && memcmp( batch->data[i], "ping", 4 ) == 0 )
                    reply_index = batch->client_index[i];

        /* Simulate the game here, then queue outgoing messages. */
        if ( reply_index >= 0 )
        {
            if ( !rocketnet_server_can_send_message( server, reply_index, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 4 ) )
                goto cleanup;
            uint8_t * out = rocketnet_server_start_message( server, (uint64_t) reply_index, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 4 );
            if ( out )
            {
                uint8_t * p = out;
                *p++ = 'p'; *p++ = 'o'; *p++ = 'n'; *p++ = 'g';
                if ( !rocketnet_server_finish_message( server, (int) ( p - out ) ) )
                    goto cleanup;
            }
            else
            {
                goto cleanup;
            }
        }
        if ( !client_sent && rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED )
        {
            if ( !rocketnet_client_can_send_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 4 ) )
                goto cleanup;
            uint8_t * out = rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 4 );
            if ( out )
            {
                uint8_t * p = out;
                *p++ = 'p'; *p++ = 'i'; *p++ = 'n'; *p++ = 'g';
                if ( !rocketnet_client_finish_message( client, (int) ( p - out ) ) )
                    goto cleanup;
                client_sent = 1;
            }
            else
            {
                goto cleanup;
            }
        }
        rocketnet_client_send_packets( client );
        rocketnet_server_send_packets( server );

        /* Wait for the next frame. The I/O threads run in real time. */
        rocketnet_sleep( dt );
    }
    result = client_received_reply ? 0 : 1;

cleanup:
    /* Command-line exit cleanup; a game would poll once per frame instead. */
    if ( client ) rocketnet_client_disconnect( client );
    if ( server ) rocketnet_server_stop( server );
    {
        const double deadline = rocketnet_time() + 10.0;
        while ( ( client && !rocketnet_client_disconnected( client ) ) ||
                ( server && !rocketnet_server_stopped( server ) ) )
        {
            if ( rocketnet_time() >= deadline )
                return 1; /* Fail without freeing storage still owned by I/O. */
            if ( client && !rocketnet_client_disconnected( client ) )
                rocketnet_client_update( client, 1000.0 );
            rocketnet_sleep( 0.001 );
        }
    }
    if ( client ) rocketnet_client_destroy( client );
    if ( server ) rocketnet_server_destroy( server );
    if ( rocketnet_term() != ROCKETNET_STATUS_OK )
        return 1;
    return result;
}
```
<!-- check_agents_md:example:end -->

---

## Choosing a transport

Both `rocketnet_client_config_t` and `rocketnet_server_config_t` expose a `transport` field (overridable in the environment via `ROCKETNET_CLIENT_TRANSPORT` and `ROCKETNET_SERVER_TRANSPORT`).

- **Ordered candidate list ([TRANSPORTS.md](TRANSPORTS.md)):** `transport` accepts a comma-separated list of transport names tried left-to-right at client connect and server start (for example, `"xdp,gso,io_uring,portable"`). The first candidate that initializes on the host wins. A known name that this build does not offer for the endpoint on this platform (`xdp` or `io_uring` on a client, or `gso` on macOS, for example) is skipped with a log line. A list with at least one name left after skipping is a strict pin: only those transports are tried, and connect or start fails loudly if every one refuses. A list whose every name is skipped falls back to the endpoint's `"auto"` order with a warning, so one fleet-wide list deploys unchanged to mixed machines. An unknown name refuses the create with `ROCKETNET_STATUS_INVALID_TRANSPORT_LIST`. The portable server is available in both builds; production ranks it after native server transports.
- **`"auto"` (the default when `transport` is `NULL` or `""`):** Selects the platform's recommended preference order ([TRANSPORTS.md](TRANSPORTS.md)):
  - **Linux server:** both builds try `"xdp"` (rank 10), `"gso"` (rank 20), `"io_uring"` (rank 25, when built with `liburing`), then `"portable"` (rank 30 in `ROCKETNET_DEV=1`, rank 50 in `ROCKETNET_DEV=0`).
  - **Linux client:** tries `"gso"` (rank 10), then `"portable"` (rank 30).
  - **macOS server and client:** tries `"msgx"` (rank 10), then `"portable"` (rank 30 on client; rank 30 in `ROCKETNET_DEV=1` / rank 50 in `ROCKETNET_DEV=0` on server).
  - **Windows server:** production (`ROCKETNET_DEV=0`) tries `"iocp"` (rank 40), `"rio"` (rank 45), then `"portable"` (rank 50); development (`ROCKETNET_DEV=1`) tries `"portable"` (rank 30), `"iocp"` (rank 40), then `"rio"` (rank 45).
  - **Windows client:** tries `"iocp"` (rank 10), then `"portable"` (rank 30).
- **No-root high-performance Linux server path:** When the process does not run with effective UID 0, `"xdp"` is skipped and `"gso"` (UDP `UDP_SEGMENT` / `UDP_GRO` coalescing with `sendmmsg` / `recvmmsg` worker threads) or `"io_uring"` (asynchronous batched ring I/O) provides high-throughput multi-queue UDP without kernel privileges.
- **AF_XDP (`"xdp"`):** Kernel-bypass eBPF/AF_XDP server dataplane on Linux (preserving the existing 4096-byte UMEM frame and ring configuration) requires effective UID 0, permission to raise `RLIMIT_MEMLOCK`, a concrete interface IP, a non-zero port, and NIC support.
- **In-process `"loopback"`:** Carries packets in memory with no sockets and is pin-only (`"auto"` never selects `"loopback"`).
- **Inspecting the active transport:** Call `rocketnet_client_transport_name(client)` and `rocketnet_server_transport_name(server)` after connect or start to read the winning transport name.

### Public configuration names and defaults

Each config struct's first field, `config_bytes`, is the size of the struct
the caller compiled against; `rocketnet_client_default_config` and
`rocketnet_server_default_config` set it, so always fill a config with them
first (a zeroed config is refused with `ROCKETNET_STATUS_CONFIG_NOT_INITIALIZED`;
docs/ABI-EVOLUTION.md). The header documents every range and environment name. The
frame and retransmission controls use these names:

| Field | Client environment | Server environment | Default |
| --- | --- | --- | --- |
| `packets_per_update` | `ROCKETNET_CLIENT_PACKETS_PER_UPDATE` | `ROCKETNET_SERVER_PACKETS_PER_UPDATE` | `256` packets per connection per update |
| `message_resend_delay_initial_ms` | `ROCKETNET_CLIENT_MESSAGE_RESEND_DELAY_INITIAL_MS` | `ROCKETNET_SERVER_MESSAGE_RESEND_DELAY_INITIAL_MS` | `2000` ms |
| `message_resend_delay_min_ms` | `ROCKETNET_CLIENT_MESSAGE_RESEND_DELAY_MIN_MS` | `ROCKETNET_SERVER_MESSAGE_RESEND_DELAY_MIN_MS` | `0` (no sampled floor) |
| `message_resend_delay_max_ms` | `ROCKETNET_CLIENT_MESSAGE_RESEND_DELAY_MAX_MS` | `ROCKETNET_SERVER_MESSAGE_RESEND_DELAY_MAX_MS` | `2000` ms |
| `transport` | `ROCKETNET_CLIENT_TRANSPORT` | `ROCKETNET_SERVER_TRANSPORT` | `NULL` (`"auto"`) |

`rocketnet_server_create` takes the addresses the server serves: one IPv4
address, one IPv6 address, or one of each on the same port (either may be
NULL, not both). `ROCKETNET_SERVER_IPV4_ADDRESS` and
`ROCKETNET_SERVER_IPV6_ADDRESS`, when set, override them for every server
created. A transport that cannot serve both
configured families refuses start with
`ROCKETNET_STATUS_TRANSPORT_FAMILIES_UNSUPPORTED`.

Other key defaults are `num_channels = 32`, `reliable_messages_in_flight = 1024`,
`receive_message_buffer_bytes = 1048576`, `max_message_bytes = 1048576`,
`send_message_buffer_bytes = 4194304`, `send_queue_messages = 1024`,
`packets_in_flight = 1024`, `unreliable_messages_per_update = 1024`,
`sequenced_messages_held = 16`, `max_message_receipts = 256`,
`reliable_message_timeout_ms = 10000`, client `send_kbps = 1000`, server
`per_client_send_kbps = 10000`, and server `max_clients = 1000`. A send rate of
`0` means the default rate, a negative rate is refused with
`ROCKETNET_STATUS_INVALID_ARGUMENT`, and `ROCKETNET_SEND_KBPS_UNLIMITED` means no
send budget on that connection. The network
simulator is development only. The backend public key and protocol ID default to zero and must be supplied
through the config or environment. The server generates its own keypair at create.

---

## The eight channel types

RocketNet implements eight reliability modes across up to `ROCKETNET_MAX_CHANNELS` (`32`) independent channels (`0 .. num_channels - 1`). Details of the wire codes and channel state machines live in [docs/RELIABILITY.md](docs/RELIABILITY.md) and [docs/CHANNELS.md](docs/CHANNELS.md).

| Mode | RocketNet macro | What the receiver sees | Receipt delivered via `rocketnet_client_receipts` / `rocketnet_server_receipts` |
| --- | --- | --- | --- |
| `UNRELIABLE` | `ROCKETNET_RELIABILITY_UNRELIABLE` | Arrives at most once, in arrival order, or is dropped; an oversize message above one packet is promoted to `ROCKETNET_RELIABILITY_RELIABLE` | None |
| `UNRELIABLE_SEQUENCED` | `ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED` | Latest-only on the channel: any message older than the newest delivered sequencing index is dropped; oversize above one packet is promoted to `ROCKETNET_RELIABILITY_RELIABLE_SEQUENCED` | None |
| `RELIABLE` | `ROCKETNET_RELIABILITY_RELIABLE` | Every message arrives exactly once; may arrive out of order relative to other messages | None |
| `RELIABLE_ORDERED` | `ROCKETNET_RELIABILITY_RELIABLE_ORDERED` | Every message on the channel arrives exactly once and in exact send order | None |
| `RELIABLE_SEQUENCED` | `ROCKETNET_RELIABILITY_RELIABLE_SEQUENCED` | The newest message on the channel is guaranteed to arrive; older out-of-order messages overtaken before delivery are skipped | None |
| `UNRELIABLE_WITH_ACK_RECEIPT` | `ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT` | Same delivery as `ROCKETNET_RELIABILITY_UNRELIABLE` | `ROCKETNET_RECEIPT_STATUS_ACKED` or `ROCKETNET_RECEIPT_STATUS_LOSS` (if promoted for size, `ROCKETNET_RECEIPT_STATUS_ACKED` only) |
| `RELIABLE_WITH_ACK_RECEIPT` | `ROCKETNET_RELIABILITY_RELIABLE_WITH_ACK_RECEIPT` | Same delivery as `ROCKETNET_RELIABILITY_RELIABLE` | `ROCKETNET_RECEIPT_STATUS_ACKED` only (when all fragments are acknowledged) |
| `RELIABLE_ORDERED_WITH_ACK_RECEIPT` | `ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT` | Same delivery as `ROCKETNET_RELIABILITY_RELIABLE_ORDERED` | `ROCKETNET_RECEIPT_STATUS_ACKED` only (when all fragments are acknowledged) |

---

## Connect tokens and security

RocketNet's security model is documented in [SECURITY.md](SECURITY.md):

The backend issues opaque base64 token strings and sends them to clients over
an authenticated channel. A client passes the string directly to
`rocketnet_client_connect`; it does not decode or construct it. Allow
`ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES`, including the NUL terminator,
when storing a token. The C API exposes no production token minting or
key-generation functions.

A real server starts as follows:

1. Set `ROCKETNET_SERVER_BACKEND_PUBLIC_KEY` to the backend's Ed25519 public
   key, base64-encoded, and `ROCKETNET_SERVER_PROTOCOL_ID` to the nonzero
   protocol ID in decimal. Config fields can also supply them; environment
   values take precedence. Keep the signing secret on the backend.
2. Initialize the config, create the server and check the pointer and status.
   Create generates a fresh X25519 keypair for this server object. Start the
   server and check that status before serving clients.
3. Read `rocketnet_server_public_key` into a buffer of
   `ROCKETNET_SERVER_PUBLIC_KEY_BYTES`. Read the served addresses with
   `rocketnet_server_ipv4_address` and `rocketnet_server_ipv6_address`, using
   `ROCKETNET_MAX_ADDRESS_STRING_BYTES` for each buffer. A false getter result
   means that family is unavailable; do not register that empty string.
4. Register the public key and served addresses with the backend over an
   authenticated channel. It signs tokens for this protocol and seals them to
   the server's key. Register again when creating a new server object.

In development, link the development library and initialize the server's backend key
from `ROCKETNET_DEV_BACKEND_PUBLIC_KEY`. Call
`rocketnet_create_dev_connect_token` with the served IPv4 and IPv6 addresses
(`NULL` for an unavailable family, but not both), generated server public key,
positive expiry duration and idle timeout in seconds, client ID, protocol ID,
current Unix timestamp, optional `ROCKETNET_CONNECT_TOKEN_USER_DATA_BYTES`
user data (`NULL` means zeros), and output string. Check its returned status.
The declaration and development key exist only with `ROCKETNET_DEV`.
A production server refuses that published key.

Tokens bind the client ID, protocol ID, server addresses, lifetime and signed
user data. The server verifies the signature, opens the token with its own
key and enforces expiry and replay checks. On the server, read a connected
slot's client ID with `rocketnet_server_client_id` and its user data with
`rocketnet_server_client_user_data( server, client_index, user_data )`, which
copies `ROCKETNET_CONNECT_TOKEN_USER_DATA_BYTES` and returns true from connect
until the slot is released; release wipes the server's copy, and a free slot
writes zeros and returns false. A dual-family client selects a
family it can open; this is not a guarantee of fallback after a reachability
failure. See [standard/TRANSPORT.md](standard/TRANSPORT.md) for the wire format.

---

## Errors and refusals

RocketNet refuses invalid configuration rather than clamping it. Check returned status codes, pointers and serials. Only the misuse cases listed in `sdk/rocketnet.h` increment `api_misuses`; a refusal is not automatically misuse.

- **Status codes (`enum rocketnet_status_t` and `rocketnet_status_name`):**
  - `ROCKETNET_STATUS_OK` (`0`): operation succeeded. Every failure is negative, from `-1` to `-31`, so `status < 0` means the call failed.
  - `ROCKETNET_STATUS_ALREADY_INITIALIZED`, `ROCKETNET_STATUS_NOT_INITIALIZED`, `ROCKETNET_STATUS_LIVE_ALLOCATIONS`, `ROCKETNET_STATUS_PLATFORM_INIT_FAILED`, `ROCKETNET_STATUS_CRYPTO_INIT_FAILED`: lifecycle and allocator state codes for `rocketnet_init` and `rocketnet_term`.
  - `ROCKETNET_STATUS_INVALID_ARGUMENT`, `ROCKETNET_STATUS_CONFIG_ENV_INVALID`, `ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE`, `ROCKETNET_STATUS_MEMORY_BUDGET_EXCEEDED`, `ROCKETNET_STATUS_MISSING_KEYS`, `ROCKETNET_STATUS_INVALID_PROTOCOL_ID`, `ROCKETNET_STATUS_DEV_KEYS_IN_PRODUCTION`, `ROCKETNET_STATUS_INVALID_ADDRESS`, `ROCKETNET_STATUS_INVALID_TRANSPORT_LIST`: argument and configuration refusals, written to the status out-parameter of `rocketnet_client_create` and `rocketnet_server_create` (create is the one check of a config) or returned by the call that refuses.
  - `ROCKETNET_STATUS_CONFIG_NOT_INITIALIZED` (`-27`): a config whose `config_bytes` is `0` or too small, one that skipped `rocketnet_client_default_config` or `rocketnet_server_default_config`.
  - `ROCKETNET_STATUS_INVALID_CONNECT_TOKEN`, `ROCKETNET_STATUS_CONNECT_TOKEN_EXPIRED`: client connect-token pre-send checks.
  - `ROCKETNET_STATUS_CONNECT_TOKEN_PORT_ZERO` (`-31`): client connect refuses a token that names a server address with port 0, on every platform; mint tokens with the server's bound port (`rocketnet_server_port`).
  - `ROCKETNET_STATUS_TRANSPORT_PORT_ZERO_UNSUPPORTED`, `ROCKETNET_STATUS_TRANSPORT_LADDER_EXHAUSTED`, `ROCKETNET_STATUS_OUT_OF_MEMORY`, `ROCKETNET_STATUS_MUTEX_CREATE_FAILED`, `ROCKETNET_STATUS_THREAD_CREATE_FAILED`, `ROCKETNET_STATUS_TIMER_CREATE_FAILED`: transport candidate ladder and OS resource failures.
  - `ROCKETNET_STATUS_CLIENT_NOT_DISCONNECTED`, `ROCKETNET_STATUS_SERVER_NOT_STOPPED`, `ROCKETNET_STATUS_FLOW_ID_RESERVED`, `ROCKETNET_STATUS_TRANSPORT_FAMILIES_UNSUPPORTED`: lifecycle and destination refusals from `rocketnet_client_connect`, `rocketnet_server_start`, and server sends.
  - `ROCKETNET_STATUS_TRANSPORT_NOT_BUILT` (`-28`): `rocketnet_server_start` with a transport list whose every opened candidate is compiled out of this build, the `xdp` transport in a Linux build configured with `ROCKETNET_XDP=OFF` or without a BPF-capable clang, libbpf or an AF_XDP provider.
  - `ROCKETNET_STATUS_TEST_PACKET_FILTER_IN_PRODUCTION` (`-29`), `ROCKETNET_STATUS_DEV_CONNECT_TOKEN_IN_PRODUCTION` (`-30`): a production build (`ROCKETNET_DEV=0`) refuses the published test packet filter at client and server create, and a connect token sealed to the published development server key at client connect.
- **`NULL` and `0` returns as refused calls:** Functions returning pointers (`rocketnet_client_create`, `rocketnet_server_create`, `rocketnet_client_start_message`, `rocketnet_server_start_message`) return `NULL` when refused. Functions returning counts, booleans, or receipt serials (`rocketnet_client_finish_message`, `rocketnet_server_finish_message`, `rocketnet_client_receipts`, `rocketnet_server_receipts`) return `0` when refused. `rocketnet_client_receive_messages` and `rocketnet_server_receive_messages` return `NULL` when no batch is left. `rocketnet_client_set_send_kbps` and `rocketnet_server_set_send_kbps` return `enum rocketnet_status_t` like the lifecycle calls: `0` is `ROCKETNET_STATUS_OK` (the new rate took), and a refusal is a negative status (`ROCKETNET_STATUS_INVALID_ARGUMENT` for a negative rate, a NULL handle or a server index outside `0 .. max_clients - 1`); a rate of `0` sets the default rate. On a connection that is not CONNECTED the call returns `ROCKETNET_STATUS_OK` and changes nothing. API misuse that the library records increments a process-wide count, read from the `api_misuses` field of `struct rocketnet_client_stats_t` or `struct rocketnet_server_stats_t`; the API misuse comment under Lifecycle and logging in sdk/rocketnet.h is the full list. It includes a `NULL` handle passed to a call, a server start message to an index at or past `max_clients`, a destination id with `ROCKETNET_FLOW_ID_BIT` set, an in-place start left open across `update` or `send_packets`, and lifecycle or allocator misuse (a create before `rocketnet_init`, a repeated server start, an init with only one allocator function, an unusable update time). A send on a connection that is not CONNECTED (a client in any state but `ROCKETNET_CLIENT_STATE_CONNECTED`, a server index below `max_clients` with no CONNECTED client, a stopped server) is a normal drop, not misuse: it is counted in the `messages_dropped_not_connected` field of the client or server stats. A second `start`, an out-of-range channel or an oversized message is counted in the library's internal send-refusal counters instead, not in `api_misuses`; a bad `bytes_written` is refused with `0` and aborts the pending start, so a later start is never blocked.
- **Update returns nothing:** `rocketnet_client_update` and `rocketnet_server_update` return `void`; what an update did is read from `rocketnet_client_state`, the server's per-client calls and the stats calls.
- **Client states (`enum rocketnet_client_state_t`, `rocketnet_client_state`, `rocketnet_client_state_name`):**
  - Progress states: `ROCKETNET_CLIENT_STATE_DISCONNECTED` (`0`), `ROCKETNET_CLIENT_STATE_REQUESTING_COOKIE` (`1`), `ROCKETNET_CLIENT_STATE_SENDING_CONNECTION_REQUEST` (`2`), `ROCKETNET_CLIENT_STATE_SENDING_CONNECTION_RESPONSE` (`3`), `ROCKETNET_CLIENT_STATE_CONNECTED` (`4`).
  - Negative states, one for each way an attempt or a connection ends, kept until `rocketnet_client_disconnect`: `ROCKETNET_CLIENT_STATE_MESSAGE_NOT_RECEIVED` (`-13`): an update found a delivered message the application had not taken; loop `rocketnet_client_receive_messages` until `NULL` before every update; `ROCKETNET_CLIENT_STATE_MESSAGE_MEMORY_EXHAUSTED` (`-12`): the connection's send descriptors (`send_queue_messages`) or send buffer (`send_message_buffer_bytes`) ran out of room for a send that could not be dropped; `ROCKETNET_CLIENT_STATE_CONFIG_MISMATCH` (`-11`): wire-checked channel configuration fields (`num_channels`, `reliable_messages_in_flight`, `receive_message_buffer_bytes`, `max_message_bytes`) differed between client and server; `ROCKETNET_CLIENT_STATE_PROTOCOL_VIOLATION` (`-10`): the server sent a malformed message record or violated window or ring bounds; `ROCKETNET_CLIENT_STATE_RELIABLE_TIMED_OUT` (`-9`): a reliable message went unacknowledged past `reliable_message_timeout_ms`; `ROCKETNET_CLIENT_STATE_SERVER_DISCONNECTED` (`-8`): the client accepted the server's disconnect packet (the server called `rocketnet_server_disconnect_client`, `rocketnet_server_stop` or `rocketnet_server_destroy`); `ROCKETNET_CLIENT_STATE_CONNECT_TOKEN_EXPIRED` (`-7`), `ROCKETNET_CLIENT_STATE_INVALID_CONNECT_TOKEN` (`-6`), `ROCKETNET_CLIENT_STATE_CONNECTION_TIMED_OUT` (`-5`), `ROCKETNET_CLIENT_STATE_CONNECTION_RESPONSE_TIMED_OUT` (`-4`), `ROCKETNET_CLIENT_STATE_CONNECTION_REQUEST_TIMED_OUT` (`-3`), `ROCKETNET_CLIENT_STATE_COOKIE_REQUEST_TIMED_OUT` (`-2`), `ROCKETNET_CLIENT_STATE_CONNECTION_DENIED` (`-1`). `rocketnet_client_connect` accepts `ROCKETNET_CLIENT_STATE_DISCONNECTED` and every negative terminal state. A terminal attempt whose old worker has not stopped yet returns `ROCKETNET_STATUS_CLIENT_NOT_DISCONNECTED` while retaining that terminal state; retry after `rocketnet_client_disconnected` is true. Every positive handshake state and `ROCKETNET_CLIENT_STATE_CONNECTED` are refused with the same status.
  - A NULL client reads as `ROCKETNET_CLIENT_STATE_DISCONNECTED`, and the call counts as misuse.
- **Server disconnect reasons (`rocketnet_server_disconnect_reason`, `rocketnet_disconnect_reason_name`):** why a server's client slot's connection ended, kept until the slot's next connection.
  - `ROCKETNET_DISCONNECT_REASON_NONE` (`0`): no connection has ended, or the connection is up.
  - `ROCKETNET_DISCONNECT_REASON_CONFIG_MISMATCH` (`-1`): wire-checked channel configuration fields differed between client and server.
  - `ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION` (`-2`): the client sent a malformed message record or violated window/ring bounds.
  - `ROCKETNET_DISCONNECT_REASON_RELIABLE_TIMED_OUT` (`-3`): a reliable message went unacknowledged past `reliable_message_timeout_ms`.
  - `ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED` (`-4`): the slot's send descriptors or send buffer ran out of room for a send that could not be dropped.
  - `ROCKETNET_DISCONNECT_REASON_SERVER_DISCONNECTED` (`-5`): the server disconnected the client, stopped, or was destroyed.
  - `ROCKETNET_DISCONNECT_REASON_MESSAGE_NOT_RECEIVED` (`-6`): an update found a message from this client that the application had not taken; loop `rocketnet_server_receive_messages` until `NULL` before every update.
- **Counters and stats to watch:**
  - Read a client's counts and measurements with `rocketnet_client_stats( client, &stats )`, which fills a `struct rocketnet_client_stats_t` with the current stats: `packets_sent`, `packets_received`, `bytes_sent`, `bytes_received`, `payloads_received`, `messages_resent`, `messages_duplicated`, `receipts_acked`, `receipts_lost`, `messages_dropped_over_budget`, `messages_dropped_not_connected`, and the measured `rtt_ms`, `jitter_ms`, `packet_loss_percent`, `packets_sent_per_second`, `packets_received_per_second`, `bandwidth_sent_kbps`, `bandwidth_received_kbps`.
  - Read the server's totals with `rocketnet_server_stats( server, &stats )` (`struct rocketnet_server_stats_t`: `packets_sent`, `packets_received`, `bytes_sent`, `bytes_received`, `connections_accepted`, `connections_denied_full`, `handshakes_dropped_over_budget`, `payloads_received`, `messages_resent`, `messages_duplicated`, `receipts_acked`, `receipts_lost`, `messages_dropped_over_budget`, `messages_dropped_not_connected`, and the rates `packets_sent_per_second`, `packets_received_per_second`, `bandwidth_sent_kbps`, `bandwidth_received_kbps`) and one slot's with `rocketnet_server_client_stats( server, client_index, &stats )` (`struct rocketnet_server_client_stats_t`: `packets_sent`, `packets_received`, `bytes_sent`, `bytes_received`, `rtt_ms`, `jitter_ms`, `packet_loss_percent`, `bandwidth_sent_kbps`, `bandwidth_received_kbps`, `bandwidth_acked_kbps`).
  - The client and server stats structs also carry two process-wide counts: `api_misuses` and `thread_priority_refusals` (priority, affinity and timer period calls the operating system refused; `rocketnet_thread_priority` turns those calls off).

---

## Threading rules

- **Main-thread ownership:** A `rocketnet_client_t` or `rocketnet_server_t` instance is owned by one caller thread for its frame loop. Run `rocketnet_client_update` / `rocketnet_server_update`, `rocketnet_client_receive_messages` / `rocketnet_server_receive_messages`, `rocketnet_client_start_message` / `rocketnet_server_start_message`, `rocketnet_client_finish_message` / `rocketnet_server_finish_message`, `rocketnet_client_abort_message` / `rocketnet_server_abort_message`, `rocketnet_client_send_packets` / `rocketnet_server_send_packets`, `rocketnet_client_receipts` / `rocketnet_server_receipts`, and `rocketnet_client_disconnect` / `rocketnet_server_disconnect_client` sequentially on that owning thread—never concurrently on the same object.
- **Background I/O and shared protocol work:** Client I/O workers, where supported, receive UDP datagrams and may pre-open and authenticate eligible encrypted records; `rocketnet_client_update` consumes those records and advances client protocol state. Server I/O varies by transport. The portable and msgx server backends seal packets on the caller, copy borrowed send frames into one of three owned TX banks outside the lock, and try-publish bank indices to a transmit worker. A separate receive worker reads socket datagrams into three owned RX banks; the caller takes a published bank, copies its packets into the transport buffer and performs shared protocol processing, including decrypt/authentication. The two workers own socket I/O; packet sealing and protocol state remain on the caller. For the required cross-thread bulk handoff design, see [the buffer handoff decision](api/BUFFER-HANDOFF-DECISION-20261021.md). Keep buffer ownership explicit; copying, scanning, resetting, codec work, and socket I/O stay outside handoff locks.

---

## Testing an integration

- **Use the `"loopback"` transport:** Set `server_config.transport = "loopback"` and `client_config.transport = "loopback"`. The `"loopback"` transport routes datagrams in memory across up to 1001 endpoints per process (one server and `ROCKETNET_MAX_CLIENTS` clients) without opening any OS socket or touching the network stack. The loopback client's I/O still runs on a real thread: the built-in loopback uses the same worker I/O core as the other built-in client transports, with bounded worker-side idle polling. Only in a development build (`ROCKETNET_DEV=1`) does `ROCKETNET_CLIENT_STEP_DRIVEN=1` instead run the client's transport receive and send work inside `rocketnet_client_update` and `rocketnet_client_send_packets` on the caller's thread, with no client I/O thread; a production build does not recognize that variable. While the development link simulator is running, its frame work and backend sends are the simulator path and remain caller-owned; that exception is development only.
- **Pass synthetic monotonic `time`:** Both `rocketnet_client_update(client, time)` and `rocketnet_server_update(server, time)` take `double time` in seconds as an explicit parameter. Start test clocks at a fixed value such as `1000.0` and advance by `1.0 / 60.0` per tick—never sleep or read wall-clock time in unit tests.
- **Simulate packet loss, latency, jitter, and duplicates:** In builds linked against the development library, call `rocketnet_client_network_simulator` and `rocketnet_server_network_simulator`, or set the environment variables `ROCKETNET_DEV_CLIENT_PACKET_LOSS_PERCENT` / `ROCKETNET_DEV_SERVER_PACKET_LOSS_PERCENT` (`0 .. 100`), `ROCKETNET_DEV_CLIENT_LATENCY_MS` / `ROCKETNET_DEV_SERVER_LATENCY_MS`, `ROCKETNET_DEV_CLIENT_JITTER_MS` / `ROCKETNET_DEV_SERVER_JITTER_MS` and `ROCKETNET_DEV_CLIENT_DUPLICATE_PERCENT` / `ROCKETNET_DEV_SERVER_DUPLICATE_PERCENT` (`0 .. 100`), to exercise retransmission, ordering, and `ROCKETNET_RECEIPT_STATUS_LOSS` handling deterministically over `"loopback"`.

### Reviewing implementation changes

- Treat [sdk/rocketnet.h](sdk/rocketnet.h), [standard/TRANSPORT.md](standard/TRANSPORT.md), [docs/SESSION.md](docs/SESSION.md), [SECURITY.md](SECURITY.md), and [docs/ABI-EVOLUTION.md](docs/ABI-EVOLUTION.md) as the authorities for their respective contracts; link to them instead of copying API or wire details. Inspect the real field widths and container ownership. Bounded registries and queues must not own linked or index-linked lists ([docs/SEQUENCE-BUFFERS.md](docs/SEQUENCE-BUFFERS.md)).
- For sequence, window, ring, or allocator changes ([docs/SEQUENCE-BUFFERS.md](docs/SEQUENCE-BUFFERS.md)), check widths at each boundary: internal sequence-buffer identities and validation tags are `uint64_t`; packet sequences are 32-bit on the wire and reconstructed to 64-bit identities; message numbers are 24-bit on the wire and reconstructed to 64-bit identities; channel sequencing and ordering state uses `uint32_t` fields masked to the 24-bit wire range; and fragment `payload_id` values are 32-bit in the wire header and reassembly state. Exercise wraparound, sparse occupancy, late and duplicate arrivals, and live-slot aliasing. Keep sequence identity separate from physical storage identity; metadata rings do not by themselves preserve arbitrary payload capacity, stable pointers, or payload lifetime, and a fixed metadata ring does not justify lost capacity or a copy fallback.
- For lifecycle or slot reuse ([docs/CONNECTION-RESET.md](docs/CONNECTION-RESET.md)), prove that prior packets, messages, callbacks, statistics, and secrets cannot affect a reconnect or reused server slot. Observe asynchronous shutdown before reuse or destroy; distinguish metadata reset (without clearing whole payload capacities) from required secret erasure (`sodium_memzero`).
- Bound caller-thread work for mass disconnects and other per-update cleanup; cover dropped frames, bursts, and send/receive overlap without imposing real-time arrival assumptions in functional correctness. Measure caller time separately from worker time, verify intended overlap of expensive worker work with the application frame, and do not infer a latency guarantee from functional tests.
- Keep host-shim and loopback unit evidence distinct from real Linux XDP transport evidence ([docs/LIVE-XDP.md](docs/LIVE-XDP.md)). The approved review policy exempts the portable server from the 1000-client performance qualification; this is a policy boundary, not a measured performance result or a portable capacity claim. Preserve the existing 4096-byte XDP frame/ring configuration and the demonstrated 1000-client server limit under its stated workload and topology, and keep the separate 1000-client and 10-Gbit/s real-server qualification gates distinct ([docs/LOAD.md](docs/LOAD.md), [docs/LOAD_SPLIT.md](docs/LOAD_SPLIT.md), [docs/LAN-PROOF.md](docs/LAN-PROOF.md), [docs/XDP-PACKET-BLASTER.md](docs/XDP-PACKET-BLASTER.md)); no above-1000-player performance promise follows.
- Keep internal test controls behind literal `#if ROCKETNET_DEV` guards and out of `sdk/rocketnet.h`, and exclude only dependent cases in `ROCKETNET_DEV=0` while retaining genuine production-linked test coverage. Review crypto nonce uniqueness, key separation by role, fresh key/token handling, random-source failure paths, and secret erasure against the security and session contracts.
- Keep public headers self-contained with minimal includes. Preserve exported C ABI and layout checks; where wire portability applies, verify byte order with independent vectors or implementations.

---

## Common mistakes

1. **Holding a received message pointer too long:** Each `rocketnet_message_batch_t::data` entry is a zero-copy view valid until the next receive call or the next update on that endpoint, whichever comes first. Process or copy every entry before either call.
2. **Calling receive once per frame:** The application must take every message before the next update, so loop `rocketnet_client_receive_messages` / `rocketnet_server_receive_messages` until it returns `NULL`. One update can deliver more than one batch. A message left in the queue at update ends that connection with `MESSAGE_NOT_RECEIVED`, because its sender was already told it arrived.
3. **Calling a second `start` before `finish` or `abort`:** Only one `rocketnet_client_start_message` (per client) or `rocketnet_server_start_message` (per server) may be pending at a time. Calling `start` again while one is open returns `NULL` and leaves the first reservation open; the refusal is counted inside the library (as a refused send), not in the stats' `api_misuses`.
4. **Keeping the `start` pointer after `finish` or `abort`:** Once `rocketnet_client_finish_message` / `rocketnet_server_finish_message` or `rocketnet_client_abort_message` / `rocketnet_server_abort_message` is called, the buffer belongs to the transport queue or free pool. Never read or write through the pointer after committing or aborting.
5. **Passing a dishonest maximum to `start` or writing past it:** `start` reserves room for `bytes` bytes (`1 .. max_message_bytes`). Writing past `bytes` corrupts adjacent region memory; passing `bytes_written < 1` or `bytes_written > bytes` to `finish` is refused (`0` returned, and the pending `start` is aborted).
6. **Calling `send_packets` before queuing messages:** Calling `rocketnet_client_send_packets` or `rocketnet_server_send_packets` at the top of the frame instead of the end leaves every message queued during the frame sitting in the region until the next frame, adding a full tick of latency.
7. **Passing wall-clock time in deterministic tests:** Mixing wall-clock time with simulated ticks causes non-deterministic timeouts (`ROCKETNET_CLIENT_STATE_RELIABLE_TIMED_OUT` on the client, `ROCKETNET_DISCONNECT_REASON_RELIABLE_TIMED_OUT` for the server's slot). Pass a controlled monotonic `time` variable instead.
8. **Forgetting the backend key or protocol ID:** The server default config leaves both zero. Supply them before create, through the config or environment; start refuses missing keys. A development server can use `ROCKETNET_DEV_BACKEND_PUBLIC_KEY`. The server's own keypair is generated at create and is not a config field.
9. **Leaving a `start` open across `update` or `send_packets`:** An in-place start (a fitting unreliable message, written into the packet buffer) still pending when `rocketnet_client_update` / `rocketnet_server_update` or `rocketnet_client_send_packets` / `rocketnet_server_send_packets` runs is aborted there and counted as API misuse (the stats' `api_misuses`); nothing is sent, and its later `finish` returns `0`. Which path `start` took is not visible to the caller, so finish or abort every start before the next `update` or `send_packets`.
10. **Sending before the connection is up:** a send before the connection is `ROCKETNET_CLIENT_STATE_CONNECTED` is dropped: start returns NULL, finish returns 0, nothing is queued, on the client and the server alike. It is a normal drop, never API misuse: `api_misuses` is unchanged, and each dropped start adds 1 to the `messages_dropped_not_connected` stat on the client or the server. Gate the first send on `rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED`, or on `rocketnet_server_client_connected` for the slot, as the example above does.

---

## Zero-copy messages

RocketNet has no copying send call: the caller never hands it a finished buffer to copy. Its only per-connection send path is the start/finish pair:

- `rocketnet_client_start_message(client, channel, reliability, bytes)` and `rocketnet_server_start_message(server, destination_id, channel, reliability, bytes)` reserve room for up to `bytes` bytes and return a `uint8_t *` the caller writes the message at, once, in place.
- The caller checks `if ( out )` first, serializes or writes the message into that pointer in place inside the non-`NULL` branch, and calls `finish` (or `abort`) only inside that branch.
- `rocketnet_client_finish_message(client, bytes_written)` and `rocketnet_server_finish_message( server, bytes_written )` commit `bytes_written` bytes (`1 <= bytes_written <= bytes`) and return the message's nonzero `uint32_t` receipt serial (`0` if refused). Calling `finish` without a successful outstanding `start` is an error and returns `0`.
- `rocketnet_client_abort_message(client)` and `rocketnet_server_abort_message( server )` cancel an open reservation without sending and give its room back.
- `rocketnet_client_can_send_message( client, channel, reliability, max_bytes )` and `rocketnet_server_can_send_message( server, client_index, channel, reliability, max_bytes )` provide an optional preflight check answering whether that start would be given room now (`start` remains the authoritative reservation result and must still be checked for non-`NULL`). They change nothing and run in constant time. A start after a `false` still ends the connection with `MESSAGE_MEMORY_EXHAUSTED`; what the application chooses to do on `false` (skip, defer, drop, or coalesce) is application policy rather than library behavior.
- There is no public broadcast call: to send one message to every connected client, loop over the slots with `rocketnet_server_client_connected` and start and finish a message for each.

Where the returned pointer lies depends on the message:

- **A fitting unreliable message is written straight into the transport's packet buffer.** Only a `ROCKETNET_RELIABILITY_UNRELIABLE`, `ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED` or `ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT` message whose declared `bytes` fits one packet with every header (up to `1411` bytes for `ROCKETNET_RELIABILITY_UNRELIABLE` and `ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT`; up to `1405` bytes for `ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED`, `1404` on a channel of `256` or more) takes this path. `start` returns a pointer inside the packet the transport will seal and send; there is no copy between the message layer and the transport.
- **Every other message takes one copy into its packet.** A reliable message (and an unreliable one that is promoted to its reliable twin, or that falls back to the queue, below) is written into the connection's preallocated region, and the packet writer copies it from there into a packet at `rocketnet_client_send_packets` / `rocketnet_server_send_packets`. A reliable record has to outlive its packet for resends, so it stays in the region until it is acknowledged.

### Why RocketNet works this way

This API is an intentional design choice of RocketNet's author:

1. **A message that fits in one packet goes out with no copy:** a fitting unreliable message is written once, by the caller, into the packet itself. A message on the region path is still written once by the caller and copied once into its packet, never through an intermediate buffer.
2. **Room for the headers is reserved in front of it:** for an in-place message, `start` asks the transport for a packet with the message header's room in front of the caller's bytes and one byte after them for the form byte; the transport's own headers are already in front of that. `finish` writes the header and form byte around the bytes the caller wrote, so nothing is moved.
3. **Start's declared maximum size lets the library choose the right buffer:** for an unreliable message, `bytes` sets the packet size requested from the transport (`header + bytes + 1`), so a message declared to fit one packet gets a real packet buffer. For a message on the region path, `bytes` picks the region storage: a fixed send-store chunk (`128`, `512` or `1152` bytes) up to `1152` bytes, or a contiguous span in the connection's send spans above that (up to `max_message_bytes`), which is fragmented into packets without the caller ever building the pieces.

In practice: no per-message copy or allocation for unreliable traffic that takes the in-place path, no per-message heap allocation for any traffic (all room comes from memory sized at create), lower CPU cost per client within the demonstrated configurations of up to `1000` connected clients, and predictable per-connection memory (`rocketnet_client_required_memory` and `rocketnet_server_required_memory`).

### Using `start`, `finish`, and `abort` correctly

- **Declare an honest maximum `bytes`:** pass the upper bound (`1 .. max_message_bytes`) your serializer may write, and write only within `out[0 .. bytes - 1]`. Over-declaring an unreliable message can push it off the in-place path (it no longer fits one packet) and asks the send budget for more than it needs.
- **What `bytes_written` may be:** pass the actual serialized length (`1 <= bytes_written <= bytes`) to `rocketnet_client_finish_message` or `rocketnet_server_finish_message`. You may reserve `256` bytes and finish `72`. A `bytes_written` below `1` or above `bytes` returns `0` and aborts the pending start, so the message is not sent and the next `start` is never blocked.
- **One `start` pending per object at a time:** finish or abort the current reservation before calling `start` again on the same client or server, and never keep the `uint8_t *` after `finish` or `abort`.
- **Finish before `update` and `send_packets`:** an in-place start still pending when `rocketnet_client_update` / `rocketnet_server_update` or `rocketnet_client_send_packets` / `rocketnet_server_send_packets` runs is aborted there and counted as API misuse (the stats' `api_misuses`); its later `finish` returns `0`. A start on the region path is not aborted there, but the caller cannot tell the paths apart, so finish or abort every start before either call.
- **The in-place rules for a fitting unreliable message:**
  - Small fitting messages share a packet. `start` reserves their final payload location after previously committed records; `finish` commits the actual length without copying. A full packet is sealed before opening the next, and `send_packets` seals and flushes the remaining records. Acks ride in each sealed packet. `abort` releases only the pending reservation; earlier records remain intact.
  - Those packets count against `packets_per_update` (one allowance per update, shared with the packets `send_packets` writes). While reliable work is due, an in-place message never takes the last slot of the allowance: one is kept for the reliable work.
  - The message falls back to the queue (written to the region and sent by the next `send_packets`, one copy) when the transport has no packet buffer to give, or one smaller than the request, when unreliable messages are already queued on that connection (so it cannot overtake them; for `ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED` also when its channel's queue or a reliable block is still waiting), when the update's packet allowance is spent, or when the send budget cannot pay the declared maximum now (or a reliable message from the last pass is still waiting for budget). Each fallback is counted inside the library (the message layer's `MESSAGE_UNRELIABLE_IN_PLACE_FALLBACK` counter, which is not one of the public stats fields). `start` then returns a pointer into the region, or `NULL` if the region has no room (below).
  - If the send budget can no longer pay when the packet is sealed, its messages are dropped there, counted in the `messages_dropped_over_budget` stat, and each successful `finish` has returned its serial; a `ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT` message reports `ROCKETNET_RECEIPT_STATUS_LOSS` for it.
- **A reliable message:** `ROCKETNET_RELIABILITY_RELIABLE`, `ROCKETNET_RELIABILITY_RELIABLE_ORDERED`, `ROCKETNET_RELIABILITY_RELIABLE_SEQUENCED`, `ROCKETNET_RELIABILITY_RELIABLE_WITH_ACK_RECEIPT` and `ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT` are always written into the region (a store chunk up to `1152` bytes, a contiguous span above that) and copied into their packet, or into `1152`-byte fragments, at `send_packets`. A reliable message waits for send budget and is never dropped for it.
- **An oversize unreliable message:** when the declared `bytes` exceeds one packet (up to `max_message_bytes`), `start` accepts it and the message is promoted on the wire to its reliable twin (`ROCKETNET_RELIABILITY_UNRELIABLE` to `ROCKETNET_RELIABILITY_RELIABLE`, `ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED` to `ROCKETNET_RELIABILITY_RELIABLE_SEQUENCED`, `ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT` to reliable-with-receipt delivery, keeping its ACKED-only receipt). If `finish` then commits a `bytes_written` that fits one packet, the message is not promoted: the same region record moves to the queued unreliable route without a copy and goes out with the next `send_packets`. Above `max_message_bytes`, `start` refuses in every mode.
- **What a `NULL` return from `start` means:**
  - **Caller misuse:** another `start` is already pending, `channel` is outside `0 .. num_channels - 1`, `reliability` is not one of the eight channel types, `bytes` is outside `1 .. max_message_bytes`, or the server's `destination_id` is outside `0 .. max_clients - 1` or has `ROCKETNET_FLOW_ID_BIT` set. `start` returns `NULL` and the connection stays up.
  - **No connection yet, or no longer:** a send before the connection is `ROCKETNET_CLIENT_STATE_CONNECTED` is dropped: start returns NULL, finish returns 0, nothing is queued. This holds on the client and the server alike, handshake states included; nothing is held for the end of the handshake, and a stopped server drops the same way for an index below `max_clients`. It is a normal drop, never API misuse: each dropped start adds 1 to the client's or the server's `messages_dropped_not_connected` stat. Send only once `rocketnet_client_state` returns `ROCKETNET_CLIENT_STATE_CONNECTED` on the client, or `rocketnet_server_client_connected` returns true for the slot on the server.
  - **Connection send memory exhausted:** a connection's region has no room for the reservation. `start` returns `NULL` and ends that connection: the server disconnects that client slot with `ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED`, and a client ends its own connection in `ROCKETNET_CLIENT_STATE_MESSAGE_MEMORY_EXHAUSTED`.

### Writing a message in place

Serialize straight into the pointer `start` returns, then finish with the bytes written:

```cpp
// RocketNet: reserves the outgoing buffer first and serializes in place
static int write_u32(uint8_t * p, uint32_t value)
{
    p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8); p[2] = (uint8_t)(value >> 16); p[3] = (uint8_t)(value >> 24);
    return 4;
}

static int write_f32(uint8_t * p, float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits)); // the float's bit pattern, not the message
    return write_u32(p, bits);
}

const int max_bytes = 1 + 4 + 2 * 4;
if (!rocketnet_client_can_send_message(client, 0, ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED, max_bytes))
    return 0; // skip this replaceable update
uint8_t * out = rocketnet_client_start_message(client, 0, ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED, max_bytes);
if (out)
{
    uint8_t * p = out;
    *p++ = 1; // message type tag
    p += write_u32(p, tick);
    p += write_f32(p, move_x);
    p += write_f32(p, move_y);

    uint32_t serial = rocketnet_client_finish_message(client, (int)(p - out));
    (void)serial;
}
```
