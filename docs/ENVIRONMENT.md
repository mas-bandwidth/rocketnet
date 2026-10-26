# Environment variables

The client and server config fields that have environment forms list their
variable names beside each field in `sdk/rocketnet.h`. Their values override
the config at the documented lifecycle point, and invalid or out-of-range
values are refused rather than clamped. `docs/ABI-EVOLUTION.md` describes the
config-size rules.

`rocketnet_client_create` and `rocketnet_server_create` read every variable
once: the defaults, then the config struct, then the environment, before the
config is validated and the endpoint's memory is sized. Client connect and
server start use that resolved config and never read the environment again,
so a variable changed after create has no effect on that endpoint. The
variables below are read the same way, at create. None of them is a field of
the public config structs:

| Variable | Meaning |
| --- | --- |
| `ROCKETNET_SERVER_IPV4_ADDRESS` | Overrides the `ipv4_address` argument of `rocketnet_server_create`, read at create. Form `a.b.c.d:port`. |
| `ROCKETNET_SERVER_IPV6_ADDRESS` | Overrides the `ipv6_address` argument of `rocketnet_server_create`, read at create. Form `[v6]:port`. |
| `ROCKETNET_CLIENT_GAME_CLOCK_EPOCH` | Unix seconds at the client's game clock zero, for the connect token's time check; 0 reads the wall clock. |
| `ROCKETNET_SERVER_GAME_CLOCK_EPOCH` | Unix seconds at the server's game clock zero, for the connect token's time check; 0 reads the wall clock. |
| `ROCKETNET_CLIENT_STEP_DRIVEN` | 1 runs the client's I/O step inside update and send_packets on the caller's thread, with no network thread; 0 (the default) keeps the network thread. |
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
| `ROCKETNET_CLIENT_MEMORY_BUDGET_BYTES` | The operator memory budget: allocator bytes the client's pre-allocation plan must fit; 0 is no budget. |
| `ROCKETNET_SERVER_MEMORY_BUDGET_BYTES` | The operator memory budget: allocator bytes a server transport candidate's pre-allocation plan must fit; 0 is no budget. |

The `ROCKETNET_DEV_` network simulator variables exist in a development
library only and are read at create; in code, the same settings are
`rocketnet_client_network_simulator` and `rocketnet_server_network_simulator`.
Tests, tools and benches set the step-driven client and the receive stamp
clock in code through `sdk/rocketnet_testing.h`.

Process-wide and platform-specific controls:

| Variable | Meaning |
| --- | --- |
| `ROCKETNET_LOG_LEVEL` | Maximum emitted log level before initialization logs: 0 .. 5 (`ROCKETNET_LOG_LEVEL_NONE` .. `ROCKETNET_LOG_LEVEL_SPAM`). A value outside that range is ignored with a warning. |
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
     `packets_in_flight < reliable_messages_in_flight`) fails with
     `ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE`. Values are never silently clamped.
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
    lifecycle peak and does not return `0` merely because this transport budget
    is smaller than the create-time endpoint/message block.
- **Compile-time CMake caps (`BUILDING.md` and [`docs/LOAD.md`](LOAD.md))**:
  - `-DROCKETNET_MAX_CLIENTS` (default `1000`) and `-DROCKETNET_MAX_CHANNELS`
    (default `32`) set the compile-time upper bounds for `max_clients` and
    `num_channels`.
  - `-DROCKETNET_REASSEMBLY_MEMORY_MIB` (default `512` MiB) bounds total
    fragment reassembly memory at `create` (`ROCKETNET_STATUS_MEMORY_BUDGET_EXCEEDED`
    when the transport's fixed payload reassembly plan for `max_clients` and
    `max_fragmented_messages` exceeds the cap). The default build scales this
    cap as 512 MiB per 1,000 compile-time clients.
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
  the server instance's actual player cap (`1 .. ROCKETNET_MAX_CLIENTS`) when
  the binary leaves `config.max_clients` at `0`. Above 1,000 players, callers
  must research, stabilize, and qualify their own configuration and host
  deployment; no arbitrary client count or validated-above-1000 guarantee is
  provided.
- **Keeping client and server protocol geometry aligned**: if you override
  `ROCKETNET_SERVER_NUM_CHANNELS`, `ROCKETNET_SERVER_MAX_MESSAGE_BYTES`,
  `ROCKETNET_SERVER_RECEIVE_MESSAGE_BUFFER_BYTES`, or
  `ROCKETNET_SERVER_SEND_MESSAGE_BUFFER_BYTES`, apply matching compatible
  values on the client (`ROCKETNET_CLIENT_NUM_CHANNELS`, etc.) so both sides
  agree on channel count and maximum message size.
