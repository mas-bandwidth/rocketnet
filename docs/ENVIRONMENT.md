# Environment variables

The client and server config fields that have environment forms list their
variable names beside each field in `sdk/rocketnet.h`. Their values override
the config at the documented lifecycle point, and invalid or out-of-range
values are refused rather than clamped. `docs/ABI-EVOLUTION.md` describes the
config-size rules.

`rocketnet_client_create` and `rocketnet_server_create` resolve endpoint
configuration once: defaults, then the config struct, then environment
overrides, before the config is validated and the endpoint's memory is sized. Client connect and
server start use that resolved config and do not re-read those endpoint overrides,
so a variable changed after create has no effect on that endpoint. The
variables below are read the same way, at create, except
`ROCKETNET_DEV_SESSION_SEQUENCE_START`, which is read once per process. Each is
either an environment form of a public config field (`token_epoch_unix_seconds`,
`step_driven`) or has no field at all:

| Variable | Meaning |
| --- | --- |
| `ROCKETNET_SERVER_IPV4_ADDRESS` | Overrides the `ipv4_address` argument of `rocketnet_server_create`, read at create. Form `a.b.c.d:port`. |
| `ROCKETNET_SERVER_IPV6_ADDRESS` | Overrides the `ipv6_address` argument of `rocketnet_server_create`, read at create. Form `[v6]:port`. |
| `ROCKETNET_CLIENT_TOKEN_EPOCH_UNIX_SECONDS` | Sets `token_epoch_unix_seconds`: Unix seconds at the client's game clock zero, for the connect token's expire check at connect, which reads this epoch plus the time of the client's most recent update (zero if it has never been updated); 0 reads the wall clock once at connect. |
| `ROCKETNET_SERVER_TOKEN_EPOCH_UNIX_SECONDS` | Sets `token_epoch_unix_seconds`: Unix seconds at the server's game clock zero, for the connect token's time check; 0 reads the wall clock once at the first update after start and subtracts that update's time to establish the epoch. |
| `ROCKETNET_CLIENT_STEP_DRIVEN` | Development libraries only: 1 runs the client's I/O step inside update and send_packets on the caller's thread, with no network thread; 0 (the default) keeps the network thread. Production libraries do not recognize this variable. |
| `ROCKETNET_DEV_SESSION_SEQUENCE_START` | Development libraries only: the first session-phase sequence every session in this process sends, in place of 1, so a run reaches the wire's 32-bit sequence wrap in seconds. Read once per process, at the first create that asks, and fixed from then on. Decimal, or hex with `0x`; range `1 .. 2^62`. Unset or empty is 1; any other value is refused with one error line and 1 is used. Production libraries do not read it. |
| `ROCKETNET_DEV_CLIENT_PACKET_LOSS_PERCENT` | Development only: outgoing client loss, 0 .. 100; 0 is off. |
| `ROCKETNET_DEV_SERVER_PACKET_LOSS_PERCENT` | Development only: outgoing server loss, 0 .. 100; 0 is off. |
| `ROCKETNET_DEV_CLIENT_LATENCY_MS` | Development only: outgoing client latency, 0 .. 60000 milliseconds; 0 is off. |
| `ROCKETNET_DEV_SERVER_LATENCY_MS` | Development only: outgoing server latency, 0 .. 60000 milliseconds; 0 is off. |
| `ROCKETNET_DEV_CLIENT_JITTER_MS` | Development only: outgoing client jitter, 0 .. 60000 milliseconds; 0 is off. |
| `ROCKETNET_DEV_SERVER_JITTER_MS` | Development only: outgoing server jitter, 0 .. 60000 milliseconds; 0 is off. |
| `ROCKETNET_DEV_CLIENT_DUPLICATE_PERCENT` | Development only: outgoing client duplication, 0 .. 100; 0 is off. |
| `ROCKETNET_DEV_SERVER_DUPLICATE_PERCENT` | Development only: outgoing server duplication, 0 .. 100; 0 is off. |
| `ROCKETNET_CLIENT_SIMULATED_DROP_EVERY_NTH` | Development only: drop every Nth outgoing client datagram, 2 .. 1000000; 0 is off. |
| `ROCKETNET_SERVER_SIMULATED_DROP_EVERY_NTH` | Development only: drop every Nth outgoing server datagram, 2 .. 1000000; 0 is off. |
| `ROCKETNET_CLIENT_SIMULATED_REVERSE_WINDOW` | Development only: reverse each window of W outgoing client datagrams, 2 .. 64; 0 is off. |
| `ROCKETNET_SERVER_SIMULATED_REVERSE_WINDOW` | Development only: reverse each window of W outgoing server datagrams, 2 .. 64; 0 is off. |
| `ROCKETNET_CLIENT_MEMORY_BUDGET_BYTES` | Maximum bytes for each client transport candidate's plan, excluding the create-time endpoint/message block; 0 is no budget. See below. |
| `ROCKETNET_SERVER_MEMORY_BUDGET_BYTES` | Maximum bytes for each server transport candidate's plan, including UMEM but excluding the create-time endpoint/message block; 0 is no budget. See below. |

The `ROCKETNET_DEV_` network simulator variables exist in a development
library only and are read at create; in code, the same settings are
`rocketnet_client_network_simulator` and `rocketnet_server_network_simulator`.
In code, the step-driven client and the receive stamp clock are the public
client config's `step_driven` and `time_callback`, development builds only; a
production build refuses either at create, and production client I/O remains
worker-owned.

Process-wide and platform-specific controls:

| Variable | Meaning |
| --- | --- |
| `ROCKETNET_TEST_XDP_FAIL_AT_QUEUE` | Development libraries only (DEV=1): fail XDP queue creation at the named queue index. Production libraries do not recognize this variable. |
| `ROCKETNET_TEST_GSO_PLAIN` | Read only under `ROCKETNET_DEV` at `sdk/rocketnet_server_gso_linux.cpp`: `1` forces the plain GSO send path. A production build (`ROCKETNET_DEV=0`) does not recognize this variable. |
| `ROCKETNET_LOG_LEVEL` | Maximum emitted log level before initialization logs: 0 .. 5 (`ROCKETNET_LOG_LEVEL_NONE` .. `ROCKETNET_LOG_LEVEL_SPAM`). Only a whole-string decimal integer is accepted; names such as `info` and values outside that range are ignored with a warning. |
| `ROCKETNET_GATEWAY_ETHERNET_ADDRESS` | Linux XDP only: overrides gateway MAC discovery in `xx:xx:xx:xx:xx:xx` form. A malformed value refuses socket creation. |

Development token support is an API helper, not an environment variable.
Development builds expose `ROCKETNET_DEV_BACKEND_PUBLIC_KEY` and
`rocketnet_create_dev_connect_token`; see `sdk/rocketnet.h`. The public
development key must not be used for production.

## Resolution order, freeze point, and planner parity

1. **Three-step precedence (`defaults -> config struct -> environment`)**:
   `rocketnet_client_create`, `rocketnet_server_create`,
   `rocketnet_client_required_memory`, and `rocketnet_server_required_memory`
   apply the same resolution sequence:
   - start from `rocketnet_client_default_config` / `rocketnet_server_default_config`;
   - copy the caller's initialized `rocketnet_*_config_t` prefix (bounded by
     `config_bytes`);
   - apply any set `ROCKETNET_CLIENT_*` or `ROCKETNET_SERVER_*` environment
     overrides.
   Every applied override emits an `INFO` log line (`<endpoint> config override NAME=value`,
   with secret key material redacted).
2. **Strict parsing vs. range validation**:
   - An empty environment value is ignored and logged. Other malformed syntax
     (leading or trailing whitespace, `+` sign, non-decimal characters,
     integer overflow, or base64 that does not decode to the key/filter's exact
     byte count) fails before validation
     with `ROCKETNET_STATUS_CONFIG_ENV_INVALID` (and `rocketnet_*_required_memory`
     returns `0`).
   - A syntactically valid number that falls outside the field's legal range or
     violates a cross-field invariant (such as
     `max_message_bytes > receive_message_buffer_bytes`,
     a send buffer whose large-slab share cannot hold one maximum message, or
     `packets_in_flight < reliable_messages_in_flight`) usually fails with
     `ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE`. Some fields have
     a specific status: for example, a negative send budget returns
     `ROCKETNET_STATUS_INVALID_ARGUMENT`, and the published test filter in a
     production library returns `ROCKETNET_STATUS_TEST_PACKET_FILTER_IN_PRODUCTION`.
     Values are never silently clamped.
3. **Frozen at `create` (never re-read at `connect` or `start`)**:
   `rocketnet_client_create` and `rocketnet_server_create` store the resolved
   transport config, message config, wire send budget, and
   `ROCKETNET_*_MEMORY_BUDGET_BYTES` inside the endpoint struct. Subsequent
   `rocketnet_client_connect` (including reconnects) and
   `rocketnet_server_start` use that stored configuration and do not read the
   process environment again. To guarantee that memory planning matches
   execution, call `rocketnet_*_required_memory` and `rocketnet_*_create` under
   the exact same environment.

## Operator memory budgets (`ROCKETNET_*_MEMORY_BUDGET_BYTES`) and compile-time caps

- **`ROCKETNET_CLIENT_MEMORY_BUDGET_BYTES` and `ROCKETNET_SERVER_MEMORY_BUDGET_BYTES`**:
  - A value of `0` (or unset) imposes no operator runtime budget beyond the
    compile-time caps.
  - The budget applies to each transport candidate's connect/start plan, not
    to the endpoint and message block allocated at create. As the endpoint
    walks its resolved transport ladder, each candidate's `total_bytes`
    (`allocator_bytes + umem_bytes`, including page-aligned AF_XDP UMEM) is
    checked against the stored budget before that transport allocates. A
    candidate over budget is skipped; if no candidate can be granted,
    connect/start returns `ROCKETNET_STATUS_TRANSPORT_LADDER_EXHAUSTED`.
    `rocketnet_*_required_memory` still reports the complete caller-allocator
    lifecycle peak (plus its 1 MiB slack) and does not return `0` merely because this transport budget
    is smaller than the create-time endpoint/message block.
- **Compile-time CMake caps (`BUILDING.md` and [`docs/LOAD.md`](LOAD.md))**:
  - `-DROCKETNET_MAX_CLIENTS` (default `1000`) sets the compile-time upper
    bound for `max_clients`.
  - `ROCKETNET_MAX_CHANNELS` (default `32`, 1 to 65536) bounds `num_channels`.
    It is a preprocessor definition, not a CMake option: an embedding build
    defines it for the library and for every file that includes
    `rocketnet.h`, with the same value.
  - `-DROCKETNET_XDP_MEMORY_MIB` (default `4096` MiB) bounds planned AF_XDP
    transport + UMEM memory across `num_xdp_queues`.

## Operational tuning with environment overrides

For full workload sizing formulas and deterministic memory tables, see
[`docs/LOAD.md`](LOAD.md). Common operational uses of the environment layer
include:

- **Pinning or narrowing the transport ladder per host**: set
  `ROCKETNET_SERVER_TRANSPORT=gso` or `ROCKETNET_SERVER_TRANSPORT=portable` on
  machines or containers that do not use `xdp` or `io_uring`, so both
  `rocketnet_server_required_memory` and `rocketnet_server_create` size for the
  intended transport rather than reserving worst-case `xdp` allocator headroom.
  When using `xdp`, tune queue and CPU count only through the supported
  `ROCKETNET_SERVER_NUM_XDP_QUEUES` (`1 .. 1024`) and
  `ROCKETNET_SERVER_NUM_OS_CPUS` (`1 .. 256`) overrides. Preserve the pinned
  AF_XDP geometry: 4,096-byte frames, a 4,096-entry send queue, 16,384-entry
  receive queue and fill set, a 32,768-entry fill ring, and 49,152 UMEM frames
  per queue.
- **Right-sizing server match capacity**: set `ROCKETNET_SERVER_MAX_CLIENTS` to
  the server instance's actual player cap (`1 .. ROCKETNET_MAX_CLIENTS`),
  regardless of `config.max_clients`; environment values take
  precedence over the struct. Above 1,000 players, callers
  must research, stabilize, and qualify their own configuration and host
  deployment; no arbitrary client count or validated-above-1000 guarantee is
  provided.
- **Keeping client and server protocol geometry aligned**: if you override
  `ROCKETNET_SERVER_NUM_CHANNELS`, `ROCKETNET_SERVER_RELIABLE_MESSAGES_IN_FLIGHT`,
  `ROCKETNET_SERVER_RECEIVE_MESSAGE_BUFFER_BYTES`, or
  `ROCKETNET_SERVER_MAX_MESSAGE_BYTES`, set the matching client field to the
  same value. Local send buffers, queues, and rates may differ; each endpoint
  must still satisfy its own cross-field constraints.
