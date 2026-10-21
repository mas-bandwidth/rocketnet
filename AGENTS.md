# AGENTS.md: Guide for AI Coding Agents Integrating RocketNet

This guide is the technical reference for an AI coding agent integrating RocketNet into a C or C++ game client and dedicated server. Every function, struct, macro, enum, and configuration variable named here matches [sdk/rocketnet.h](sdk/rocketnet.h).

- [What RocketNet is](#what-rocketnet-is)
- [Building and linking on Linux, Windows and macOS](#building-and-linking-on-linux-windows-and-macos)
- [The integration loop](#the-integration-loop)
- [Choosing a transport](#choosing-a-transport)
- [The eight channel types and how they map from RakNet's PacketReliability](#the-eight-channel-types-and-how-they-map-from-raknets-packetreliability)
- [Connect tokens and security](#connect-tokens-and-security)
- [Errors and refusals](#errors-and-refusals)
- [Threading rules](#threading-rules)
- [Testing an integration](#testing-an-integration)
- [Common mistakes](#common-mistakes)
- [Zero-copy messages: an intentional difference from RakNet](#zero-copy-messages-an-intentional-difference-from-raknet)

---

## What RocketNet is

RocketNet is a low-level, connection-oriented client/server UDP transport and message library written in C++ with a pure C public interface:

- **Real-time contract:** No `rocketnet.h` call blocks. Public calls do bounded
  work; the main frame calls and create are intended to be unnoticeable in a
  game profiler. Long-running soak, load and profiling work belongs to the
  planned hardening phase and is not a claim of this deliverable.

- **One public header:** [sdk/rocketnet.h](sdk/rocketnet.h). Include it directly from C or C++ (`#include "rocketnet.h"`); the header provides its own `extern "C"` guards when compiled as C++.
- **One link library:** `RocketNet::rocketnet` (or the `rocketnet` CMake target when embedded via `add_subdirectory`).
- **Version macros:** `ROCKETNET_VERSION` is one comparable integer, `ROCKETNET_VERSION_MAJOR * 10000 + ROCKETNET_VERSION_MINOR * 100 + ROCKETNET_VERSION_PATCH` (`10000` for 1.0.0, and for v1.0.0-preview-1, since the prerelease is not in the integer; a development build uses `255` for each part). The text form is `ROCKETNET_VERSION_FULL` (`"v1.0.0-preview-1"` for this release, or `"dev"` in a development build). `rocketnet_version()` returns the integer the library was built with; compare it with `ROCKETNET_VERSION` to catch a header and a shared library from different releases.
- **Up to 1000 clients per server (`ROCKETNET_MAX_CLIENTS`):** Clients are addressed by slot index `0 .. max_clients - 1`. Each client and server allocates its connection memory once at creation (`rocketnet_client_required_memory`, `rocketnet_server_required_memory`, aligned to `ROCKETNET_ALLOCATION_ALIGNMENT`) and performs zero dynamic allocations while connections are live.
- **Up to 32 channels (`ROCKETNET_MAX_CHANNELS`) and 8 delivery modes:** Every message is sent on a channel `0 .. num_channels - 1` with one of eight reliability modes matching RakNet's `PacketReliability`.

---

## Building and linking on Linux, Windows and macOS

Full build instructions live in [BUILDING.md](BUILDING.md).

### Prerequisites

- **Linux (Ubuntu 24.04):**
  ```sh
  sudo apt update
  sudo apt install -y cmake build-essential libbpf-dev libxdp-dev liburing-dev xxd
  ```
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

Pass `-DCMAKE_PREFIX_PATH=/path/to/prefix` when configuring the consuming project. For a shared library build, configure RocketNet with `-DBUILD_SHARED_LIBS=ON`.

When embedding the source tree with `add_subdirectory`:

```cmake
add_subdirectory(external/rocketnet)
target_link_libraries(game PRIVATE rocketnet)
```

---

## The integration loop

A RocketNet integration follows a strict lifecycle and per-frame call order:

1. **Initialize the library once per process:** Call `rocketnet_init(NULL)`; it returns `ROCKETNET_STATUS_OK` on success or a `rocketnet_status_t` refusal.
2. **Create the server and client:**
   - Fill `struct rocketnet_server_config_t` with `rocketnet_server_default_config(&server_config)`, set `protocol_id`, `backend_public_key`, `server_public_key`, and `server_secret_key` (or use `rocketnet_server_dev_config` in local development builds), call `rocketnet_server_create` with the address and a status out-parameter (it writes the refusal's code on NULL; `rocketnet_status_name` names it), then call `rocketnet_server_start(server)`. Create binds nothing and starts stopped; start binds the transport and accepts clients.
   - Fill `struct rocketnet_client_config_t` with `rocketnet_client_default_config(&client_config)`, call `rocketnet_client_create` with a status out-parameter (it writes the refusal's code on NULL), then call `rocketnet_client_connect(client, connect_token)` with a `ROCKETNET_CONNECT_TOKEN_BYTES`-byte connect token. The client connects using the token alone; the handshake begins on the first `rocketnet_client_update`.
3. **Run the five-step frame loop in exact order every tick** (using the caller's monotonic `double time` in seconds):
   - **Step 1 — Update:** Call `rocketnet_client_update(client, time)` and `rocketnet_server_update(server, time)` first to pump the transport, process incoming packets and ACKs, and advance connection state.
   - **Step 2 — Receive:** Drain the one receive queue with `rocketnet_client_receive_messages` / `rocketnet_server_receive_messages`. Each call returns one `rocketnet_message_batch_t`; keep calling until it returns false. Process each entry's client index, channel, reliability, byte count and data pointer before the next receive or update on that endpoint. Read ACK/loss receipts with `rocketnet_client_receipts` / `rocketnet_server_receipts`.
   - **Step 3 — Simulate:** Run the game simulation for the frame.
   - **Step 4 — Queue outgoing messages:** Reserve space in place with `rocketnet_client_start_message` / `rocketnet_server_start_message`, write the message bytes directly into the returned pointer, and commit with `rocketnet_client_finish_message` / `rocketnet_server_finish_message` (or cancel with `rocketnet_client_abort_message` / `rocketnet_server_abort_message`). To send one message to every connected client, loop over the slots with `rocketnet_server_client_connected` and start and finish a message for each.
   - **Step 5 — Send packets last:** Call `rocketnet_client_send_packets(client)` and `rocketnet_server_send_packets(server)` **last** so every message queued during the frame goes out on the wire in the same frame instead of waiting until the next tick.
4. **Disconnect, destroy, and terminate:** Call `rocketnet_client_disconnect` (or `rocketnet_server_disconnect_client`), destroy every client and server with `rocketnet_client_destroy` and `rocketnet_server_destroy`, and call `rocketnet_term()`.

### Compilable loopback integration example

<!-- check_agents_md:example:begin -->
```c
#include "rocketnet.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main( void )
{
    if ( rocketnet_init( NULL ) != ROCKETNET_STATUS_OK )
        return 1;

    uint8_t backend_public_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret_key[ROCKETNET_BACKEND_SECRET_KEY_BYTES];
    uint8_t server_public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    uint8_t server_secret_key[ROCKETNET_SERVER_SECRET_KEY_BYTES];

    if ( rocketnet_generate_backend_keypair( backend_public_key, backend_secret_key ) != ROCKETNET_STATUS_OK )
        return 1;
    if ( rocketnet_generate_server_keypair( server_public_key, server_secret_key ) != ROCKETNET_STATUS_OK )
        return 1;

    const uint64_t protocol_id = 0x1122334455667788ULL;
    const char * server_address = "127.0.0.1:40000";

    struct rocketnet_server_config_t server_config;
    if ( rocketnet_server_default_config( &server_config ) != ROCKETNET_STATUS_OK )
        return 1;
    server_config.protocol_id = protocol_id;
    server_config.max_clients = 8;
    server_config.transport = "loopback";
    memcpy( server_config.backend_public_key, backend_public_key, sizeof( backend_public_key ) );
    memcpy( server_config.server_public_key, server_public_key, sizeof( server_public_key ) );
    memcpy( server_config.server_secret_key, server_secret_key, sizeof( server_secret_key ) );

    enum rocketnet_status_t create_status = ROCKETNET_STATUS_OK;
    struct rocketnet_server_t * server = rocketnet_server_create_sized(
        NULL, server_address, &server_config, sizeof( server_config ), NULL, 0, &create_status );
    if ( !server || create_status != ROCKETNET_STATUS_OK )
        return 1;
    if ( rocketnet_server_start( server ) != ROCKETNET_STATUS_OK )
    {
        rocketnet_server_destroy( server );
        return 1;
    }

    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    if ( rocketnet_write_production_connect_token(
             backend_secret_key,
             server_public_key,
             protocol_id,
             &server_address,
             1,
             42ULL,
             1ULL,
             4102444800ULL,
             15,
             NULL,
             connect_token,
             sizeof( connect_token ) ) != ROCKETNET_STATUS_OK )
    {
        rocketnet_server_destroy( server );
        return 1;
    }

    struct rocketnet_client_config_t client_config;
    if ( rocketnet_client_default_config( &client_config ) != ROCKETNET_STATUS_OK )
    {
        rocketnet_server_destroy( server );
        return 1;
    }
    client_config.transport = "loopback";
    client_config.step_driven = 1;

    create_status = ROCKETNET_STATUS_OK;
    struct rocketnet_client_t * client = rocketnet_client_create_sized(
        NULL, &client_config, sizeof( client_config ), NULL, 0, &create_status );
    if ( !client || create_status != ROCKETNET_STATUS_OK )
    {
        rocketnet_server_destroy( server );
        return 1;
    }
    if ( rocketnet_client_connect( client, connect_token ) != ROCKETNET_STATUS_OK )
    {
        rocketnet_client_destroy( client );
        rocketnet_server_destroy( server );
        return 1;
    }

    double time = 1000.0;
    const double dt = 1.0 / 60.0;
    int client_sent = 0;
    int server_received_and_replied = 0;
    int client_received_reply = 0;

    for ( int tick = 0; tick < 120 && !client_received_reply; tick++, time += dt )
    {
        if ( rocketnet_client_update( client, time ) != ROCKETNET_UPDATE_OK )
            return 1;
        if ( rocketnet_server_update( server, time ) != ROCKETNET_UPDATE_OK )
            return 1;

        struct rocketnet_message_batch_t batch;
        while ( rocketnet_client_receive_messages( client, &batch ) )
        {
            for ( int i = 0; i < batch.num_messages; i++ )
                if ( batch.bytes[i] == 4 && memcmp( batch.data[i], "pong", 4 ) == 0 )
                    client_received_reply = 1;
        }

        int reply_pending = 0;
        uint64_t reply_index = 0;
        while ( rocketnet_server_receive_messages( server, &batch ) )
        {
            for ( int i = 0; i < batch.num_messages; i++ )
            {
                if ( batch.bytes[i] == 4 && memcmp( batch.data[i], "ping", 4 ) == 0 )
                {
                    reply_pending = 1;
                    reply_index = (uint64_t) batch.client_index[i];
                }
            }
        }

        if ( reply_pending )
        {
            uint8_t * reply = rocketnet_server_start_message( server, reply_index, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 4 );
            if ( !reply )
                return 1;
            memcpy( reply, "pong", 4 );
            if ( rocketnet_server_finish_message( server, reply_index, 4 ) == 0 )
                return 1;
            server_received_and_replied = 1;
        }

        if ( !client_sent && rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED )
        {
            uint8_t * out = rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 4 );
            if ( !out )
                return 1;
            memcpy( out, "ping", 4 );
            if ( rocketnet_client_finish_message( client, 4 ) == 0 )
                return 1;
            client_sent = 1;
        }

        rocketnet_client_send_packets( client );
        rocketnet_server_send_packets( server );
    }

    if ( !client_sent || !server_received_and_replied || !client_received_reply )
        return 1;

    rocketnet_client_disconnect( client );
    rocketnet_client_destroy( client );
    rocketnet_server_destroy( server );

    if ( rocketnet_term() != ROCKETNET_STATUS_OK )
        return 1;

    return 0;
}
```
<!-- check_agents_md:example:end -->

---

## Choosing a transport

Both `rocketnet_client_config_t` and `rocketnet_server_config_t` expose a `transport` field (overridable in the environment via `ROCKETNET_CLIENT_TRANSPORT` and `ROCKETNET_SERVER_TRANSPORT`).

- **Ordered candidate list:** `transport` accepts a comma-separated list of transport names tried left-to-right at creation (for example, `"xdp,gso,portable"`). The first candidate that initializes on the host wins. A single name pins that transport and fails creation loudly if unavailable.
- **`"auto"` (the default when `transport` is `NULL` or `""`):** Selects the platform's recommended preference order:
  - **Linux server:** tries `"xdp"`, `"gso"`, `"io_uring"`, `"portable"`.
  - **macOS server and client:** tries `"msgx"`, `"portable"`.
  - **Windows server:** tries `"portable"`, `"iocp"`, `"rio"`.
  - **Linux and Windows client:** uses `"portable"`.
- **No-root high-performance Linux server path:** When running without root or `CAP_BPF` / `CAP_NET_ADMIN`, `"xdp"` is skipped and `"gso"` (UDP `UDP_SEGMENT` / `UDP_GRO` coalescing with `sendmmsg` / `recvmmsg` worker threads) or `"io_uring"` (asynchronous batched ring I/O) provides high-throughput multi-queue UDP without kernel privileges.
- **AF_XDP (`"xdp"`):** Kernel-bypass eBPF/AF_XDP server dataplane on Linux when root (or `CAP_BPF` and `CAP_NET_ADMIN`) and NIC support are present.
- **In-process `"loopback"`:** Carries packets in memory with no sockets and is pin-only (`"auto"` never selects `"loopback"`).
- **Inspecting the active transport:** Call `rocketnet_client_transport_name(client)` and `rocketnet_server_transport_name(server)` after creation to read the winning transport name.

### Public configuration names and defaults

The public client and server structs have one revision: the whole struct.
Unsized calls read or write that revision; `_sized` calls take
`sizeof(*config)`. The header documents every range and environment name. The
frame and retransmission controls use these final names:

| Field | Client environment | Server environment | Default |
| --- | --- | --- | --- |
| `packets_per_update` | `ROCKETNET_CLIENT_PACKETS_PER_UPDATE` | `ROCKETNET_SERVER_PACKETS_PER_UPDATE` | `256` packets per connection per update |
| `resend_delay_initial_ms` | `ROCKETNET_CLIENT_RESEND_DELAY_INITIAL_MS` | `ROCKETNET_SERVER_RESEND_DELAY_INITIAL_MS` | `2000` ms |
| `resend_delay_min_ms` | `ROCKETNET_CLIENT_RESEND_DELAY_MIN_MS` | `ROCKETNET_SERVER_RESEND_DELAY_MIN_MS` | `0` (no sampled floor) |
| `resend_delay_max_ms` | `ROCKETNET_CLIENT_RESEND_DELAY_MAX_MS` | `ROCKETNET_SERVER_RESEND_DELAY_MAX_MS` | `2000` ms |
| `transport` | `ROCKETNET_CLIENT_TRANSPORT` | `ROCKETNET_SERVER_TRANSPORT` | `NULL` (`"auto"`) |

The server's `public_address` and optional `second_public_address` advertise
one IPv4 address, one IPv6 address, or one of each. Their environment names are
`ROCKETNET_SERVER_PUBLIC_ADDRESS` and
`ROCKETNET_SERVER_SECOND_PUBLIC_ADDRESS`. A transport that cannot serve both
configured families refuses create with
`ROCKETNET_STATUS_TRANSPORT_FAMILIES_UNSUPPORTED`.

Other key defaults are `num_channels = 32`, `window_messages = 1024`,
`receive_ring_bytes = 262144`, `max_message_bytes = 262144`,
`send_buffer_bytes = 800000`, `send_queue_messages = 1024`,
`packets_in_flight = 256`, `unreliable_deliveries = 256`,
`sequenced_holds = 16`, `receipt_ring = 256`,
`reliable_timeout_ms = 10000`, client `send_kbps = 1000`, server
`per_client_send_kbps = 10000`, and server `max_clients = 1000`. The four
`simulated_*` fields default to zero and are development-only. Server key
fields default to zero; start refuses them until the application supplies
production keys or explicitly uses the development config.

---

## The eight channel types and how they map from RakNet's PacketReliability

RocketNet implements all eight RakNet `PacketReliability` modes across up to `ROCKETNET_MAX_CHANNELS` (`32`) independent channels (`0 .. num_channels - 1`). Details of the wire codes and channel state machines live in [docs/RAKNET-RELIABILITY.md](docs/RAKNET-RELIABILITY.md) and [docs/CHANNELS.md](docs/CHANNELS.md).

| RakNet `PacketReliability` | RocketNet macro | What the receiver sees | Receipt delivered via `rocketnet_client_receipts` / `rocketnet_server_receipts` |
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

- **Who mints connect tokens:** A client never connects to a raw IP address. Instead, your authentication or matchmaking backend mints a `ROCKETNET_CONNECT_TOKEN_BYTES`-byte (`768`-byte) connect token using `rocketnet_write_production_connect_token` and hands it to the client over HTTPS. For local development and tests built with the development library, `rocketnet_write_dev_connect_token` mints a token against the published development keys.
- **Address binding:** A token carries one IPv4 address, one IPv6 address, or
  one of each. A dual-family client selects the family it can open. The server
  accepts either advertised family it serves, and an active token is bound to
  one source address; replaying that token from another address is refused.
- **Keys and key generation:**
  - **Backend signing keypair (Ed25519):** Generated via `rocketnet_generate_backend_keypair` or deterministically via `rocketnet_generate_backend_keypair_from_seed` (`ROCKETNET_BACKEND_SEED_BYTES` = `32`, `ROCKETNET_BACKEND_PUBLIC_KEY_BYTES` = `32`, `ROCKETNET_BACKEND_SECRET_KEY_BYTES` = `64`). The backend keeps `backend_secret_key`; each game server receives `backend_public_key` (`ROCKETNET_SERVER_BACKEND_PUBLIC_KEY`).
  - **Server sealing keypair (X25519):** Generated via `rocketnet_generate_server_keypair` (`ROCKETNET_SERVER_PUBLIC_KEY_BYTES` = `32`, `ROCKETNET_SERVER_SECRET_KEY_BYTES` = `32`). The backend seals private token data to `server_public_key`; the server opens it with `server_secret_key` (`ROCKETNET_SERVER_SERVER_PUBLIC_KEY`, `ROCKETNET_SERVER_SERVER_SECRET_KEY`).
  - **No default production keys:** `rocketnet_server_default_config` leaves the three key fields all-zero, and `rocketnet_server_start` refuses to bind until non-zero keys are provided. `rocketnet_server_dev_config` populates the development keys explicitly.
  - **User data:** Each token carries `ROCKETNET_CONNECT_TOKEN_USER_DATA_BYTES` (`256` bytes) of signed application metadata.
- **What the server checks on every handshake:**
  1. Packet size, `filter_value` (`ROCKETNET_SERVER_FILTER_VALUE` / `ROCKETNET_CLIENT_FILTER_VALUE`), wire version, `protocol_id` (`ROCKETNET_SERVER_PROTOCOL_ID`), and `create_timestamp <= expire_timestamp`.
  2. Current timestamp within `[create_timestamp, expire_timestamp)` and the server's advertised `public_address` (`ROCKETNET_SERVER_PUBLIC_ADDRESS`) present in the token's address list.
  3. Stateless cookie challenge before spending asymmetric cryptography under load, plus a per-update signature verification budget (`ROCKETNET_SERVER_COUNTER_HANDSHAKES_OVER_BUDGET`).
  4. Ed25519 signature verification over the signed token fields under `backend_public_key`.
  5. Single-use token replay history and active client/address tables (`rocketnet_server_client_id`).
  6. X25519 sealed-box decryption of the private token block under `server_secret_key` and `server_public_key`, binding the 32-byte session keys to the authenticated client.

---

## Errors and refusals

RocketNet never clamps invalid configuration or silently ignores API misuse; every refusal is explicit and counted.

- **Status codes (`enum rocketnet_status_t` and `rocketnet_status_name`):**
  - `ROCKETNET_STATUS_OK` (`0`): operation succeeded (`ROCKETNET_STATUS_COUNT` bounds the enum).
  - `ROCKETNET_STATUS_ALREADY_INITIALIZED`, `ROCKETNET_STATUS_NOT_INITIALIZED`, `ROCKETNET_STATUS_LIVE_ALLOCATIONS`, `ROCKETNET_STATUS_PLATFORM_INIT_FAILED`, `ROCKETNET_STATUS_CRYPTO_INIT_FAILED`: lifecycle and allocator state codes for `rocketnet_init`, `rocketnet_term`, and `rocketnet_set_allocator`.
  - `ROCKETNET_STATUS_INVALID_ARGUMENT`, `ROCKETNET_STATUS_CONFIG_ENV_INVALID`, `ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE`, `ROCKETNET_STATUS_MEMORY_BUDGET_EXCEEDED`, `ROCKETNET_STATUS_MISSING_KEYS`, `ROCKETNET_STATUS_INVALID_PROTOCOL_ID`, `ROCKETNET_STATUS_DEV_KEYS_IN_RELEASE`, `ROCKETNET_STATUS_INVALID_ADDRESS`, `ROCKETNET_STATUS_INVALID_TRANSPORT_LIST`: argument and configuration validation failures (`rocketnet_client_validate_config`, `rocketnet_server_validate_config`).
  - `ROCKETNET_STATUS_INVALID_CONNECT_TOKEN`, `ROCKETNET_STATUS_CONNECT_TOKEN_EXPIRED`: client connect-token pre-send checks.
  - `ROCKETNET_STATUS_TRANSPORT_PORT_ZERO_UNSUPPORTED`, `ROCKETNET_STATUS_TRANSPORT_LADDER_EXHAUSTED`, `ROCKETNET_STATUS_OUT_OF_MEMORY`, `ROCKETNET_STATUS_MUTEX_CREATE_FAILED`, `ROCKETNET_STATUS_THREAD_CREATE_FAILED`, `ROCKETNET_STATUS_TIMER_CREATE_FAILED`: transport candidate ladder and OS resource failures.
  - `ROCKETNET_STATUS_CLIENT_NOT_DISCONNECTED`, `ROCKETNET_STATUS_SERVER_NOT_STOPPED`, `ROCKETNET_STATUS_FLOW_ID_RESERVED`: lifecycle and destination refusals from `rocketnet_client_connect`, `rocketnet_server_start`, and server sends.
- **`NULL` and `0` returns as refused calls:** Functions returning pointers (`rocketnet_client_create`, `rocketnet_server_create`, `rocketnet_client_start_message`, `rocketnet_server_start_message`) return `NULL` when refused. Functions returning counts, booleans, or receipt serials (`rocketnet_client_finish_message`, `rocketnet_server_finish_message`, `rocketnet_client_receive_messages`, `rocketnet_server_receive_messages`) return `0` when refused. `rocketnet_client_set_send_kbps` and `rocketnet_server_set_send_kbps` return `enum rocketnet_status_t` like the lifecycle calls: `0` is `ROCKETNET_STATUS_OK` (the new rate took), and a refusal is a nonzero status (`ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE` above `ROCKETNET_SEND_BUDGET_MAX_KBPS`, `ROCKETNET_STATUS_INVALID_ARGUMENT` for a NULL handle or a connection that is not CONNECTED, client handshake states and a server slot with no CONNECTED client included, `ROCKETNET_STATUS_FLOW_ID_RESERVED` for a server flow id). API misuse that the library records increments the process-wide counter returned by `rocketnet_api_misuse_count()`; the comment on `rocketnet_api_misuse_count` in sdk/rocketnet.h is the full list. It includes a `NULL` handle passed to a call, a server start or finish to an index at or past `max_clients`, a destination id with `ROCKETNET_FLOW_ID_BIT` set, an in-place start left open across `update` or `send_packets`, and lifecycle or allocator misuse (a create before `rocketnet_init`, a repeated server start, a refused allocator replacement, an unusable update time). A send on a connection that is not CONNECTED (a client in any state but `ROCKETNET_CLIENT_STATE_CONNECTED`, a server index below `max_clients` with no CONNECTED client, a stopped server) is a normal drop, not misuse: it is counted in `ROCKETNET_CLIENT_COUNTER_NOT_CONNECTED_DROPS` or `ROCKETNET_SERVER_COUNTER_NOT_CONNECTED_DROPS`. A second `start`, an out-of-range channel or an oversized message is counted in the library's internal send-refusal counters instead, not in `rocketnet_api_misuse_count()`; a bad `bytes_written` is refused with `0` and leaves the start pending, with no counter of its own.
- **Update return codes:** `rocketnet_client_update` and `rocketnet_server_update` return `ROCKETNET_UPDATE_OK` (`0`) after pumping the endpoint.
- **Client states (`enum rocketnet_client_state_t`, `rocketnet_client_state`, `rocketnet_client_state_name`):**
  - Progress states: `ROCKETNET_CLIENT_STATE_DISCONNECTED` (`0`), `ROCKETNET_CLIENT_STATE_REQUESTING_COOKIE` (`1`), `ROCKETNET_CLIENT_STATE_SENDING_CONNECTION_REQUEST` (`2`), `ROCKETNET_CLIENT_STATE_SENDING_CONNECTION_RESPONSE` (`3`), `ROCKETNET_CLIENT_STATE_CONNECTED` (`4`).
  - Negative terminal failure states: `ROCKETNET_CLIENT_STATE_CONNECT_TOKEN_EXPIRED` (`-7`), `ROCKETNET_CLIENT_STATE_INVALID_CONNECT_TOKEN` (`-6`), `ROCKETNET_CLIENT_STATE_CONNECTION_TIMED_OUT` (`-5`), `ROCKETNET_CLIENT_STATE_CONNECTION_RESPONSE_TIMED_OUT` (`-4`), `ROCKETNET_CLIENT_STATE_CONNECTION_REQUEST_TIMED_OUT` (`-3`), `ROCKETNET_CLIENT_STATE_COOKIE_REQUEST_TIMED_OUT` (`-2`), `ROCKETNET_CLIENT_STATE_CONNECTION_DENIED` (`-1`).
- **Disconnect reasons (`rocketnet_client_disconnect_reason`, `rocketnet_server_disconnect_reason`):**
  - `ROCKETNET_DISCONNECT_REASON_NONE` (`0`): no message-layer error.
  - `ROCKETNET_DISCONNECT_REASON_CONFIG_MISMATCH` (`1`): wire-checked channel configuration fields (`num_channels`, `window_messages`, `receive_ring_bytes`, `max_message_bytes`) differed between client and server.
  - `ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION` (`3`): peer sent a malformed message record or violated window/ring bounds.
  - `ROCKETNET_DISCONNECT_REASON_RELIABLE_TIMEOUT` (`4`): a reliable message went unacknowledged past `reliable_timeout_ms`.
  - `ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED` (`5`): connection send descriptors (`send_queue_messages`) or send buffer (`send_buffer_bytes`) ran out of room for a send that could not be dropped.
  - `ROCKETNET_DISCONNECT_REASON_SERVER_DISCONNECTED` (`6`): client only. The client accepted the server's disconnect packet: the server called `rocketnet_server_disconnect_client`, `rocketnet_server_stop` or `rocketnet_server_destroy`. Kept until the next connection reaches CONNECTED. A deliberate `rocketnet_client_disconnect` and a connection timeout read `NONE`.
- **Counters and stats to watch:**
  - Read client counters with `rocketnet_client_counter` (`enum rocketnet_client_counter_t`: `ROCKETNET_CLIENT_COUNTER_PACKETS_SENT`, `ROCKETNET_CLIENT_COUNTER_PACKETS_RECEIVED`, `ROCKETNET_CLIENT_COUNTER_BYTES_SENT`, `ROCKETNET_CLIENT_COUNTER_BYTES_RECEIVED`, `ROCKETNET_CLIENT_COUNTER_PAYLOADS_RECEIVED`, `ROCKETNET_CLIENT_COUNTER_MESSAGES_RESENT`, `ROCKETNET_CLIENT_COUNTER_MESSAGES_DUPLICATE`, `ROCKETNET_CLIENT_COUNTER_RECEIPTS_ACKED`, `ROCKETNET_CLIENT_COUNTER_RECEIPTS_LOST`, `ROCKETNET_CLIENT_COUNTER_BUDGET_DROPS`, `ROCKETNET_CLIENT_COUNTER_SEND_DEFERRED`, `ROCKETNET_CLIENT_COUNTER_MESSAGES_NOT_RELEASED`, `ROCKETNET_CLIENT_COUNTER_NOT_CONNECTED_DROPS`).
  - Read server-wide counters with `rocketnet_server_counter` (`enum rocketnet_server_counter_t`: `ROCKETNET_SERVER_COUNTER_PACKETS_SENT`, `ROCKETNET_SERVER_COUNTER_PACKETS_RECEIVED`, `ROCKETNET_SERVER_COUNTER_BYTES_SENT`, `ROCKETNET_SERVER_COUNTER_BYTES_RECEIVED`, `ROCKETNET_SERVER_COUNTER_CLIENTS_CONNECTED`, `ROCKETNET_SERVER_COUNTER_CLIENTS_DENIED_FULL`, `ROCKETNET_SERVER_COUNTER_HANDSHAKES_OVER_BUDGET`, `ROCKETNET_SERVER_COUNTER_PAYLOADS_RECEIVED`, `ROCKETNET_SERVER_COUNTER_MESSAGES_RESENT`, `ROCKETNET_SERVER_COUNTER_MESSAGES_DUPLICATE`, `ROCKETNET_SERVER_COUNTER_RECEIPTS_ACKED`, `ROCKETNET_SERVER_COUNTER_RECEIPTS_LOST`, `ROCKETNET_SERVER_COUNTER_BUDGET_DROPS`, `ROCKETNET_SERVER_COUNTER_MESSAGES_NOT_RELEASED`, `ROCKETNET_SERVER_COUNTER_NOT_CONNECTED_DROPS`) and per-slot counters with `rocketnet_server_client_counter` (`enum rocketnet_server_client_counter_t`: `ROCKETNET_SERVER_CLIENT_COUNTER_PACKETS_SENT`, `ROCKETNET_SERVER_CLIENT_COUNTER_PACKETS_RECEIVED`, `ROCKETNET_SERVER_CLIENT_COUNTER_BYTES_SENT`, `ROCKETNET_SERVER_CLIENT_COUNTER_BYTES_RECEIVED`).
  - Read latency, jitter, packet loss, and bandwidth gauges with `rocketnet_client_stat` (`enum rocketnet_client_stat_t`, such as `ROCKETNET_CLIENT_STAT_RTT`, `ROCKETNET_CLIENT_STAT_JITTER`, `ROCKETNET_CLIENT_STAT_PACKET_LOSS`, `ROCKETNET_CLIENT_STAT_BANDWIDTH_SENT`, `ROCKETNET_CLIENT_STAT_BANDWIDTH_RECEIVED`), `rocketnet_server_stat` (`enum rocketnet_server_stat_t`), and `rocketnet_server_client_stat` (`enum rocketnet_server_client_stat_t`, such as `ROCKETNET_SERVER_CLIENT_STAT_RTT`, `ROCKETNET_SERVER_CLIENT_STAT_JITTER`, `ROCKETNET_SERVER_CLIENT_STAT_PACKET_LOSS`, `ROCKETNET_SERVER_CLIENT_STAT_BANDWIDTH_ACKED`).

---

## Threading rules

- **Main-thread ownership:** A `rocketnet_client_t` or `rocketnet_server_t` instance is owned by one caller thread for its frame loop. Run `rocketnet_client_update` / `rocketnet_server_update`, `rocketnet_client_receive_messages` / `rocketnet_server_receive_messages`, `rocketnet_client_start_message` / `rocketnet_server_start_message`, `rocketnet_client_finish_message` / `rocketnet_server_finish_message`, `rocketnet_client_abort_message` / `rocketnet_server_abort_message`, `rocketnet_client_send_packets` / `rocketnet_server_send_packets`, `rocketnet_client_receipts` / `rocketnet_server_receipts`, and `rocketnet_client_disconnect` / `rocketnet_server_disconnect_client` sequentially on that owning thread—never concurrently on the same object.
- **What the background I/O thread does:** By default (`step_driven == 0`), socket transports run background I/O threads that pull UDP datagrams off the NIC, decrypt and authenticate packets off the main thread, and drain staged outgoing batches when `rocketnet_client_send_packets` / `rocketnet_server_send_packets` flushes them. The caller thread never touches locks during message reads or writes; handoff happens inside `update` and `send_packets`.
- **Step-driven client mode (`step_driven`):** Setting `client_config.step_driven = 1` (or `ROCKETNET_CLIENT_STEP_DRIVEN=1` in the environment) disables the client's background I/O thread. The client then runs its socket/transport receive step inside `rocketnet_client_update` and its send step inside `rocketnet_client_send_packets` directly on the caller's thread. Use `step_driven = 1` in deterministic simulations, single-threaded harnesses, or when the host engine manages every thread explicitly.

---

## Testing an integration

- **Use the `"loopback"` transport:** Set `server_config.transport = "loopback"` and `client_config.transport = "loopback"` (with `client_config.step_driven = 1`). The `"loopback"` transport routes datagrams in memory across up to `ROCKETNET_LOOPBACK_MAX_ENDPOINTS` (`1001`) endpoints per process without opening any OS socket or touching the network stack.
- **Pass synthetic monotonic `time`:** Both `rocketnet_client_update(client, time)` and `rocketnet_server_update(server, time)` take `double time` in seconds as an explicit parameter. Start test clocks at a fixed value such as `1000.0` and advance by `1.0 / 60.0` per tick—never sleep or read wall-clock time in unit tests.
- **Simulate packet loss, latency, jitter, and duplicates:** In builds linked against the development library, set `simulated_loss_percent` (`0..100`), `simulated_latency_ms`, `simulated_jitter_ms`, and `simulated_duplicate_percent` (`0..100`) on `rocketnet_client_config_t` and `rocketnet_server_config_t` to exercise retransmission, ordering, and `ROCKETNET_RECEIPT_STATUS_LOSS` handling deterministically over `"loopback"`.

---

## Common mistakes

1. **Holding a received message pointer too long:** Each `rocketnet_message_batch_t::data` entry is a zero-copy view valid until the next receive call or the next update on that endpoint, whichever comes first. Process or copy every entry before either call.
2. **Calling a second `start` before `finish` or `abort`:** Only one `rocketnet_client_start_message` (per client) or `rocketnet_server_start_message` (per server) may be pending at a time. Calling `start` again while one is open returns `NULL` and leaves the first reservation open; the refusal is counted inside the library (as a refused send), not in `rocketnet_api_misuse_count()`.
3. **Keeping the `start` pointer after `finish` or `abort`:** Once `rocketnet_client_finish_message` / `rocketnet_server_finish_message` or `rocketnet_client_abort_message` / `rocketnet_server_abort_message` is called, the buffer belongs to the transport queue or free pool. Never read or write through the pointer after committing or aborting.
4. **Passing a dishonest maximum to `start` or writing past it:** `start` reserves room for `bytes` bytes (`1 .. max_message_bytes`). Writing past `bytes` corrupts adjacent region memory; passing `bytes_written < 1` or `bytes_written > bytes` to `finish` is refused (`0` returned, `start` stays pending).
5. **Calling `send_packets` before queuing messages:** Calling `rocketnet_client_send_packets` or `rocketnet_server_send_packets` at the top of the frame instead of the end leaves every message queued during the frame sitting in the region until the next frame, adding a full tick of latency.
6. **Passing wall-clock time in deterministic tests:** Mixing wall-clock time with simulated ticks causes non-deterministic timeouts (`ROCKETNET_DISCONNECT_REASON_RELIABLE_TIMEOUT`). Pass a controlled monotonic `time` variable instead.
7. **Forgetting keys on `rocketnet_server_default_config`:** `rocketnet_server_default_config` zeroes `backend_public_key`, `server_public_key`, and `server_secret_key`; `rocketnet_server_start` returns `ROCKETNET_STATUS_MISSING_KEYS` unless valid keys are populated or `rocketnet_server_dev_config` is used in development.
8. **Leaving a `start` open across `update` or `send_packets`:** An in-place start (a fitting unreliable message, written into the packet buffer) still pending when `rocketnet_client_update` / `rocketnet_server_update` or `rocketnet_client_send_packets` / `rocketnet_server_send_packets` runs is aborted there and counted as API misuse (`rocketnet_api_misuse_count()`); nothing is sent, and its later `finish` returns `0`. Which path `start` took is not visible to the caller, so finish or abort every start before the next `update` or `send_packets`.
9. **Sending before the connection is up:** a send before the connection is `ROCKETNET_CLIENT_STATE_CONNECTED` is dropped: start returns NULL, finish returns 0, nothing is queued, on the client and the server alike. It is a normal drop, never API misuse: `rocketnet_api_misuse_count()` is unchanged, and each dropped start adds 1 to `ROCKETNET_CLIENT_COUNTER_NOT_CONNECTED_DROPS` on the client or `ROCKETNET_SERVER_COUNTER_NOT_CONNECTED_DROPS` on the server. Gate the first send on `rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED`, or on `rocketnet_server_client_connected` for the slot, as the example above does.

---

## Zero-copy messages: an intentional difference from RakNet

In RakNet, `RakPeerInterface::Send` takes a `const char *` or `RakNet::BitStream *` and copies the caller's payload into internal `BitStream` and `InternalPacket` allocations. RocketNet has no copying send call. Its only per-connection send path is the start/finish pair:

- `rocketnet_client_start_message(client, channel, reliability, bytes)` and `rocketnet_server_start_message(server, destination_id, channel, reliability, bytes)` reserve room for up to `bytes` bytes and return a `uint8_t *` the caller writes the message at, once, in place.
- The caller serializes or writes the message into that pointer in place.
- `rocketnet_client_finish_message(client, bytes_written)` and `rocketnet_server_finish_message(server, destination_id, bytes_written)` commit `bytes_written` bytes (`1 <= bytes_written <= bytes`) and return the message's nonzero `uint32_t` receipt serial (`0` if refused).
- `rocketnet_client_abort_message(client)` and `rocketnet_server_abort_message(server, destination_id)` cancel an open reservation without sending and give its room back.
- There is no public broadcast call: to send one message to every connected client, loop over the slots with `rocketnet_server_client_connected` and start and finish a message for each.

Where the returned pointer lies depends on the message:

- **A fitting unreliable message is written straight into the transport's packet buffer.** Only a `ROCKETNET_RELIABILITY_UNRELIABLE`, `ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED` or `ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT` message whose declared `bytes` fits one packet with every header (up to `1411` bytes for `ROCKETNET_RELIABILITY_UNRELIABLE` and `ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT`; up to `1405` bytes for `ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED`, `1404` on a channel of `256` or more) takes this path. `start` returns a pointer inside the packet the transport will seal and send; there is no copy between the message layer and the transport.
- **Every other message takes one copy into its packet.** A reliable message (and an unreliable one that is promoted to its reliable twin, or that falls back to the queue, below) is written into the connection's preallocated region, and the packet writer copies it from there into a packet at `rocketnet_client_send_packets` / `rocketnet_server_send_packets`. A reliable record has to outlive its packet for resends, so it stays in the region until it is acknowledged.

### Why RocketNet works this way

This API is an intentional design choice of RocketNet's author:

1. **A message that fits in one packet goes out with no copy:** a fitting unreliable message is written once, by the caller, into the packet itself. A message on the region path is still written once by the caller and copied once into its packet, never through an intermediate `BitStream`.
2. **Room for the headers is reserved in front of it:** for an in-place message, `start` asks the transport for a packet with the message header's room in front of the caller's bytes and one byte after them for the form byte; the transport's own headers are already in front of that. `finish` writes the header and form byte around the bytes the caller wrote, so nothing is moved.
3. **Start's declared maximum size lets the library choose the right buffer:** for an unreliable message, `bytes` sets the packet size requested from the transport (`header + bytes + 1`), so a message declared to fit one packet gets a real packet buffer. For a message on the region path, `bytes` picks the region storage: a fixed send-store chunk (`128`, `512` or `1152` bytes) up to `1152` bytes, or a contiguous span in the connection's send spans above that (up to `max_message_bytes`), which is fragmented into packets without the caller ever building the pieces.

In practice: no per-message copy or allocation for unreliable traffic that takes the in-place path, no per-message heap allocation for any traffic (all room comes from memory sized at create), lower CPU cost per client at `1000`+ connected clients, and predictable per-connection memory (`rocketnet_client_required_memory` and `rocketnet_server_required_memory`).

### Using `start`, `finish`, and `abort` correctly

- **Declare an honest maximum `bytes`:** pass the upper bound (`1 .. max_message_bytes`) your serializer may write, and write only within `out[0 .. bytes - 1]`. Over-declaring an unreliable message can push it off the in-place path (it no longer fits one packet) and asks the send budget for more than it needs.
- **What `bytes_written` may be:** pass the actual serialized length (`1 <= bytes_written <= bytes`) to `rocketnet_client_finish_message` or `rocketnet_server_finish_message`. You may reserve `256` bytes and finish `72`. A `bytes_written` below `1` or above `bytes` returns `0` and leaves the start pending until it is finished with a valid size or cancelled with `abort`.
- **One `start` pending per object at a time:** finish or abort the current reservation before calling `start` again on the same client or server, and never keep the `uint8_t *` after `finish` or `abort`.
- **Finish before `update` and `send_packets`:** an in-place start still pending when `rocketnet_client_update` / `rocketnet_server_update` or `rocketnet_client_send_packets` / `rocketnet_server_send_packets` runs is aborted there and counted as API misuse (`rocketnet_api_misuse_count()`); its later `finish` returns `0`. A start on the region path is not aborted there, but the caller cannot tell the paths apart, so finish or abort every start before either call.
- **The in-place rules for a fitting unreliable message:**
  - Each fitting message gets its own packet, sealed and staged at `finish` and flushed by the next `send_packets` with everything else. Acks ride in it as in every packet; other messages do not. A game that wants several small unreliable messages in one packet writes them as one message.
  - Those packets count against `packets_per_update` (one allowance per update, shared with the packets `send_packets` writes). While reliable work is due, an in-place message never takes the last slot of the allowance: one is kept for the reliable work.
  - The message falls back to the queue (written to the region and sent by the next `send_packets`, one copy) when the transport has no packet buffer to give, or one smaller than the request, when unreliable messages are already queued on that connection (so it cannot overtake them; for `ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED` also when its channel's queue or a reliable block is still waiting), when the update's packet allowance is spent, or when the send budget cannot pay the declared maximum now (or a reliable message from the last pass is still waiting for budget). Each fallback is counted inside the library (the message layer's `MESSAGE_UNRELIABLE_IN_PLACE_FALLBACK` counter, which is not one of the public client or server counters). `start` then returns a pointer into the region, or `NULL` if the region has no room (below).
  - If the send budget can no longer pay at `finish`, the message is dropped there, counted in `ROCKETNET_CLIENT_COUNTER_BUDGET_DROPS` / `ROCKETNET_SERVER_COUNTER_BUDGET_DROPS`, and `finish` still returns its serial; a `ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT` message reports `ROCKETNET_RECEIPT_STATUS_LOSS` for it.
- **A reliable message:** `ROCKETNET_RELIABILITY_RELIABLE`, `ROCKETNET_RELIABILITY_RELIABLE_ORDERED`, `ROCKETNET_RELIABILITY_RELIABLE_SEQUENCED`, `ROCKETNET_RELIABILITY_RELIABLE_WITH_ACK_RECEIPT` and `ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT` are always written into the region (a store chunk up to `1152` bytes, a contiguous span above that) and copied into their packet, or into `1152`-byte fragments, at `send_packets`. A reliable message waits for send budget and is never dropped for it.
- **An oversize unreliable message:** when the declared `bytes` exceeds one packet (up to `max_message_bytes`), `start` accepts it and the message is promoted on the wire to its reliable twin (`ROCKETNET_RELIABILITY_UNRELIABLE` to `ROCKETNET_RELIABILITY_RELIABLE`, `ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED` to `ROCKETNET_RELIABILITY_RELIABLE_SEQUENCED`, `ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT` to reliable-with-receipt delivery, keeping its ACKED-only receipt), as RakNet's `ReliabilityLayer` does. If `finish` then commits a `bytes_written` that fits one packet, the message is not promoted: the same region record moves to the queued unreliable route without a copy and goes out with the next `send_packets`. Above `max_message_bytes`, `start` refuses in every mode.
- **What a `NULL` return from `start` means:**
  - **Caller misuse:** another `start` is already pending, `channel` is outside `0 .. num_channels - 1`, `reliability` is not one of the eight channel types, `bytes` is outside `1 .. max_message_bytes`, or the server's `destination_id` is outside `0 .. max_clients - 1` or has `ROCKETNET_FLOW_ID_BIT` set. `start` returns `NULL` and the connection stays up.
  - **No connection yet, or no longer:** a send before the connection is `ROCKETNET_CLIENT_STATE_CONNECTED` is dropped: start returns NULL, finish returns 0, nothing is queued. This holds on the client and the server alike, handshake states included; nothing is held for the end of the handshake, and a stopped server drops the same way for an index below `max_clients`. It is a normal drop, never API misuse: each dropped start adds 1 to `ROCKETNET_CLIENT_COUNTER_NOT_CONNECTED_DROPS` or `ROCKETNET_SERVER_COUNTER_NOT_CONNECTED_DROPS`. Send only once `rocketnet_client_state` returns `ROCKETNET_CLIENT_STATE_CONNECTED` on the client, or `rocketnet_server_client_connected` returns true for the slot on the server.
  - **Connection send memory exhausted:** a connected peer's region has no room for the reservation. `start` returns `NULL` and ends that connection with `ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED` (the server disconnects that client slot; the client disconnects itself).

### Porting RakNet-style code: before and after

**Before (RakNet `BitStream` + copy into `Send`):**

```cpp
// RakNet: serializes into a temporary BitStream, then copies inside Send()
RakNet::BitStream bs;
bs.Write((uint8_t)ID_PLAYER_INPUT);
bs.Write(tick);
bs.Write(move_x);
bs.Write(move_y);
peer->Send(&bs, HIGH_PRIORITY, UNRELIABLE_SEQUENCED, 0, server_addr, false);
```

**After (RocketNet zero-copy `start` / `finish`):**

```cpp
// RocketNet: reserves the outgoing buffer first and writes in place
const int max_bytes = 1 + (int)sizeof(uint32_t) + 2 * (int)sizeof(float);
uint8_t * out = rocketnet_client_start_message(client, 0, ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED, max_bytes);
if (out)
{
    int offset = 0;
    out[offset++] = 1; // message type tag
    memcpy(out + offset, &tick, sizeof(tick));
    offset += (int)sizeof(tick);
    memcpy(out + offset, &move_x, sizeof(move_x));
    offset += (int)sizeof(move_x);
    memcpy(out + offset, &move_y, sizeof(move_y));
    offset += (int)sizeof(move_y);

    uint32_t serial = rocketnet_client_finish_message(client, offset);
    (void)serial;
}
```
