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
`step_driven`, `bind_ipv4_address`, `bind_ipv6_address`) or has no field at all:

| Variable | Meaning |
| --- | --- |
| `ROCKETNET_SERVER_IPV4_ADDRESS` | Overrides the `ipv4_address` argument of `rocketnet_server_create`, read at create. Form `a.b.c.d:port`. |
| `ROCKETNET_SERVER_IPV6_ADDRESS` | Overrides the `ipv6_address` argument of `rocketnet_server_create`, read at create. Form `[v6]:port`. |
| `ROCKETNET_SERVER_BIND_IPV4_ADDRESS` | Sets `bind_ipv4_address`: the local IPv4 address the server binds, `a.b.c.d` with no port (behind NAT, the private address). Applies only to a server that serves IPv4 alone; a server of IPv6 or of both families ignores it with one log line. A value that is not a usable bind address (it does not parse, carries a port, or is multicast or broadcast) is refused at start with `INVALID_ADDRESS`; a valid address the system will not bind (not on this machine, or in use) is `TRANSPORT_BIND_REFUSED` ([the status table](TRANSPORTS.md#choosing-a-transport)). Unset leaves the field; with both empty the server binds the wildcard. |
| `ROCKETNET_SERVER_BIND_IPV6_ADDRESS` | Sets `bind_ipv6_address`: the local IPv6 address the server binds, with no port. Applies only to a server that serves IPv6 alone; otherwise ignored with one log line. Refused at start as the IPv4 form is. Unset leaves the field; with both empty the server binds the wildcard. |
| `ROCKETNET_CLIENT_TOKEN_EPOCH_UNIX_SECONDS` | Sets `token_epoch_unix_seconds`: Unix seconds at the client's game clock zero, for the connect token's expire check at connect, which reads this epoch plus the time of the client's most recent update (zero if it has never been updated, so a connect before the first update is checked against the epoch alone); 0 reads the wall clock once at connect. |
| `ROCKETNET_SERVER_TOKEN_EPOCH_UNIX_SECONDS` | Sets `token_epoch_unix_seconds`: Unix seconds at the server's game clock zero, for the connect token's time check; 0 reads the wall clock once at the first update after start and subtracts that update's time to establish the epoch. |
| `ROCKETNET_CLIENT_STEP_DRIVEN` | Development libraries only: 1 runs the client's I/O step inside update and send_packets on the caller's thread, with no network thread; 0 (the default) keeps the network thread. Production libraries do not recognize this variable. |
| `ROCKETNET_DEV_SESSION_SEQUENCE_START` | Development libraries only: the first session-phase sequence every session in this process sends, in place of 1, so a run reaches the wire's 32-bit sequence wrap in seconds. Read once per process, at the first create that asks, and fixed from then on. Decimal, or hex with `0x`; range `1 .. 2^62`. Unset or empty is 1; any other value is refused with one error line and 1 is used. Production libraries do not read it. |
| `ROCKETNET_DEV_CLIENT_PACKET_LOSS_PERCENT` | Development libraries only: outgoing client loss, 0 .. 100; 0 is off. A production library does not read it: a value there is ignored, with no log line and no refusal. |
| `ROCKETNET_DEV_SERVER_PACKET_LOSS_PERCENT` | Development libraries only: outgoing server loss, 0 .. 100; 0 is off. A production library does not read it: a value there is ignored, with no log line and no refusal. |
| `ROCKETNET_DEV_CLIENT_LATENCY_MS` | Development libraries only: outgoing client latency, 0 .. 60000 milliseconds; 0 is off. A production library does not read it: a value there is ignored, with no log line and no refusal. |
| `ROCKETNET_DEV_SERVER_LATENCY_MS` | Development libraries only: outgoing server latency, 0 .. 60000 milliseconds; 0 is off. A production library does not read it: a value there is ignored, with no log line and no refusal. |
| `ROCKETNET_DEV_CLIENT_JITTER_MS` | Development libraries only: outgoing client jitter, 0 .. 60000 milliseconds; 0 is off. A production library does not read it: a value there is ignored, with no log line and no refusal. |
| `ROCKETNET_DEV_SERVER_JITTER_MS` | Development libraries only: outgoing server jitter, 0 .. 60000 milliseconds; 0 is off. A production library does not read it: a value there is ignored, with no log line and no refusal. |
| `ROCKETNET_DEV_CLIENT_DUPLICATE_PERCENT` | Development libraries only: outgoing client duplication, 0 .. 100; 0 is off. A production library does not read it: a value there is ignored, with no log line and no refusal. |
| `ROCKETNET_DEV_SERVER_DUPLICATE_PERCENT` | Development libraries only: outgoing server duplication, 0 .. 100; 0 is off. A production library does not read it: a value there is ignored, with no log line and no refusal. |
| `ROCKETNET_CLIENT_SIMULATED_DROP_EVERY_NTH` | Development use: drop every Nth outgoing client datagram, 2 .. 1000000; 0 is off. A value outside that range is refused as out of range in either build. A production library reads it and refuses any other nonzero value at create with `CONFIG_OUT_OF_RANGE`. |
| `ROCKETNET_SERVER_SIMULATED_DROP_EVERY_NTH` | Development use: drop every Nth outgoing server datagram, 2 .. 1000000; 0 is off. A value outside that range is refused as out of range in either build. A production library reads it and refuses any other nonzero value at create with `CONFIG_OUT_OF_RANGE`. |
| `ROCKETNET_CLIENT_SIMULATED_REVERSE_WINDOW` | Development use: reverse each window of W outgoing client datagrams, 2 .. 64; 0 is off. A value outside that range is refused as out of range in either build. A production library reads it and refuses any other nonzero value at create with `CONFIG_OUT_OF_RANGE`. |
| `ROCKETNET_SERVER_SIMULATED_REVERSE_WINDOW` | Development use: reverse each window of W outgoing server datagrams, 2 .. 64; 0 is off. A value outside that range is refused as out of range in either build. A production library reads it and refuses any other nonzero value at create with `CONFIG_OUT_OF_RANGE`. |
| `ROCKETNET_CLIENT_MEMORY_BUDGET_BYTES` | Maximum bytes for each client transport candidate's plan, excluding the create-time endpoint/message block; 0 is no budget. See below. |
| `ROCKETNET_SERVER_MEMORY_BUDGET_BYTES` | Maximum bytes for each server transport candidate's plan, including UMEM but excluding the create-time endpoint/message block; 0 is no budget. See below. |

The two simulator families behave oppositely in a production library. The
`ROCKETNET_DEV_` network simulator variables are read at create by a
development library only; a production library never reads them, so one left
in a host's environment silently does nothing. In code, the same settings are
`rocketnet_client_network_simulator` and `rocketnet_server_network_simulator`,
which a production header does not declare. The
`ROCKETNET_CLIENT_SIMULATED_` and `ROCKETNET_SERVER_SIMULATED_` variables are
read by every library, and a production library refuses a nonzero value: one
left in a host's environment stops every create there with
`CONFIG_OUT_OF_RANGE` and an `ERROR` line naming the field.
In code, the step-driven client and the receive stamp clock are the public
client config's `step_driven` and `time_callback`, development builds only; a
production build refuses either at create, and production client I/O remains
worker-owned.

Process-wide and platform-specific controls:

| Variable | Meaning |
| --- | --- |
| `ROCKETNET_TEST_XDP_FAIL_AT_QUEUE` | Development libraries only (DEV=1): fail XDP queue creation at the named queue index. Production libraries do not recognize this variable. |
| `ROCKETNET_TEST_GSO_PLAIN` | Read only under `ROCKETNET_DEV` at `sdk/rocketnet_server_gso_linux.cpp`: `1` forces the plain GSO send path. A production build (`ROCKETNET_DEV=0`) does not recognize this variable. |
| `ROCKETNET_TEST_RIO_LANES` | Read only under `ROCKETNET_DEV` at `sdk/rocketnet_server_rio_windows.cpp`: `2` to `32` runs every RIO server that leaves the lane count to the transport (`num_rio_lanes` unset) on that many lanes in place of the default, except a wildcard bind, which keeps one; one lane asked for explicitly stays one. A production build (`ROCKETNET_DEV=0`) does not recognize this variable. |
| `ROCKETNET_TEST_RIO_LANES_PLANT` | Read only under `ROCKETNET_DEV` at `sdk/rocketnet_server_rio_windows.cpp`: set to anything, every RIO transport makes one lane whatever was asked. The plant control of a hand run of the RIO server cases on four lanes: every check of the lanes actually made must fail under it (no CI step runs it). A production build does not recognize this variable. |
| `ROCKETNET_TEST_RIO_PIN` | Read only under `ROCKETNET_DEV` at `sdk/rocketnet_server_rio_windows.cpp`: places a RIO lane's worker for a measurement: `rss` on the RSS processor, `core` on the whole core, `none` unpinned; unset is the default (the core's other logical processors). A production build does not recognize this variable. |
| `ROCKETNET_TEST_RIO_LAYOUT` | Read only under `ROCKETNET_DEV` at `sdk/rocketnet_server_rio_windows.cpp`: `R0,R1,..` `/` `W0,W1,..` places RIO lane i's socket on processor Ri (SIO_CPU_AFFINITY) and its worker on processor Wi alone, processor group 0, with more than one lane, for a measurement in the layout a game runs; a shorter list or a processor past group 0 keeps the default placement and says so. A production build does not recognize this variable. |
| `ROCKETNET_TEST_IOCP_PIN` | Read only under `ROCKETNET_DEV` at `sdk/rocketnet_server_iocp_windows.cpp`, with more than one completion worker: places the workers for a measurement: `sibling` on the core's other logical processors (the RSS processor keeps the receive work), `core` on the whole core, `none` unpinned; unset is the RSS processor. A production build does not recognize this variable. |
| `ROCKETNET_TEST_IOCP_LAYOUT` | Read only under `ROCKETNET_DEV` at `sdk/rocketnet_server_iocp_windows.cpp`, with more than one completion worker: `R0,R1,..` `/` `W0,W1,..` places worker i's socket on processor Ri (SIO_CPU_AFFINITY) and the worker on processor Wi alone, processor group 0, for a measurement in the layout a game runs; a shorter list or a processor past group 0 keeps the default placement and says so. A production build does not recognize this variable. |
| `ROCKETNET_LOG_LEVEL` | Maximum emitted log level before initialization logs: 0 .. 5 (`ROCKETNET_LOG_LEVEL_NONE` .. `ROCKETNET_LOG_LEVEL_SPAM`). Only a whole-string decimal integer is accepted; names such as `info` and values outside that range are ignored with a warning. |
| `ROCKETNET_GATEWAY_ETHERNET_ADDRESS` | Linux XDP only: overrides gateway MAC discovery in `xx:xx:xx:xx:xx:xx` form. A malformed value refuses socket creation. |

Development token support is an API helper, not an environment variable.
Development builds expose `ROCKETNET_DEV_BACKEND_PUBLIC_KEY` and
`rocketnet_create_dev_connect_token`; see `sdk/rocketnet.h`. The public
development key must not be used for production.

## Network threads, per transport

Each server transport that has a thread count to choose has its own config
field and its own variable; no setting is shared between transports, and a
transport reads only its own. Zero, or the variable unset, is the transport's
default: a count that held the measured peak workload, 1000 players at 60 Hz
and about 900,000 packets a second from the server (an estimated 9.39 Gb/s,
not a byte count), on the machine it was measured on.
[PERFORMANCE.md](PERFORMANCE.md) has the rows for `xdp`, `gso`, `io_uring`,
`iocp` and `rio`. The
variable wins over the field, with an `INFO` line, like every other override.
A value outside the range, or a variable that is not a number, is refused at
create (`CONFIG_OUT_OF_RANGE` or `CONFIG_ENV_INVALID`), never clamped.

| Variable | Config field | Transport | Default | Range |
| --- | --- | --- | --- | --- |
| `ROCKETNET_SERVER_NUM_XDP_THREADS` | `num_xdp_threads` | `xdp` (Linux) | 1 | 1 .. 1024 |
| `ROCKETNET_SERVER_NUM_GSO_WORKERS` | `num_gso_workers` | `gso` (Linux) | 1 | 1 .. 32 |
| `ROCKETNET_SERVER_NUM_IO_URING_WORKERS` | `num_io_uring_workers` | `io_uring` (Linux) | 2 | 1 .. 32 |
| `ROCKETNET_SERVER_NUM_RIO_LANES` | `num_rio_lanes` | `rio` (Windows) | 5 on a bind address; 1 on the wildcard | 1 .. 32 |
| `ROCKETNET_SERVER_NUM_IOCP_COMPLETION_WORKERS` | `num_iocp_completion_workers` | `iocp` (Windows) | 2 on a bind address; 1 on the wildcard | 1 .. 32 |
| `ROCKETNET_SERVER_NUM_IOCP_SEND_THREADS` | `num_iocp_send_threads` | `iocp` (Windows) | 2, or the send lanes when fewer | 1 .. 16, and at most the send lanes (8 at 1000 clients); above the lanes is refused at start, below |
| `ROCKETNET_SERVER_NUM_MSGX_TRANSMIT_WORKERS` | `num_msgx_transmit_workers` | `msgx` (macOS) | 3, plus one receive worker | 1 .. 16 |

One check is not made at create: an `iocp` send-thread count inside 1 .. 16
but above the send lanes the server's `max_clients` gives. `iocp` refuses it
at start with a warning naming the limit, and start tries the next transport
in the list; with `iocp` alone, start fails with `NO_TRANSPORT_FOUND`.

`portable` and `loopback` have no count to choose: `portable` runs two fixed
threads, `loopback` none.

The one variable these replace is retired: it is not read, and a server create
that finds it set logs one warning naming the variables above.

| Variable | Status |
| --- | --- |
| `ROCKETNET_SERVER_NUM_XDP_QUEUES` | retired, no effect: a warning at server create when set |

The machine must match on one transport only: `xdp` runs one thread per NIC
queue, and refuses to start when the NIC's combined queue count is not
`num_xdp_threads` (`scripts/rocketnet-provision` sets it; see
[`docs/DEPLOYING.md`](DEPLOYING.md)). The others need no machine setting.

### Cpu 0, per Linux transport

The recommended layout keeps cpu 0 for the operating system, puts the network
threads on the cpus after it (from `ROCKETNET_SERVER_NUM_OS_CPUS`, default 1),
and leaves the rest to the game, its main thread first. Each Linux transport
whose threads the library places from cpu 0 up can give cpu 0 to its threads
instead:

| Variable | Config field | Transport | Default | Values |
| --- | --- | --- | --- | --- |
| `ROCKETNET_SERVER_GSO_RESERVE_CPU_0` | `gso_reserve_cpu_0` | `gso` (Linux) | reserved | 1 reserved, 2 not reserved |
| `ROCKETNET_SERVER_IO_URING_RESERVE_CPU_0` | `io_uring_reserve_cpu_0` | `io_uring` (Linux) | reserved | 1 reserved, 2 not reserved |
| `ROCKETNET_SERVER_PORTABLE_RESERVE_CPU_0` | `portable_reserve_cpu_0` | `portable` on Linux (and `msgx` there) | reserved | 1 reserved, 2 not reserved |

Zero is the default and reads as reserved (`ROCKETNET_CPU_0_RESERVED`): network
thread q runs on cpu `num_os_cpus + q`. Not reserved
(`ROCKETNET_CPU_0_NOT_RESERVED`) puts thread q on cpu q and does not apply
`num_os_cpus` to that transport; a server create with `num_os_cpus` set logs an
info line saying so. Any other value is refused with
`ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE`. A pin the machine refuses never stops
the server: the thread is warned about in the log, counted in that
transport's `*_PIN_REFUSALS` row (an SDK-internal counter no public call
reads), and asks for no real-time priority.

`xdp` has no such setting: its provisioning keeps cpu 0 as the housekeeping and
interrupt cpu and isolates the cpus its threads run on. `rio` and `iocp`
(Windows) put lane or worker i on core i to match the NIC's receive-side
scaling, and `msgx` on macOS leaves placement to the OS.

On Windows, more than one RIO lane or IOCP completion worker needs a specific
bind address. With nothing asked and a wildcard bind (no
`ROCKETNET_SERVER_BIND_IPV4_ADDRESS` or `ROCKETNET_SERVER_BIND_IPV6_ADDRESS`),
`rio` and `iocp` run one lane or one completion worker and log one warning
naming the bind address variables; a count above 1 asked for on the wildcard is
refused with `INVALID_ADDRESS`.

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
     `max_message_bytes > receive_message_buffer_bytes` or
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
    walks its resolved transport list, each candidate's `total_bytes`
    (`allocator_bytes + umem_bytes`, including page-aligned AF_XDP UMEM) is
    checked against the stored budget before that transport allocates. A
    candidate over budget is skipped; if no candidate can be granted,
    connect/start returns `ROCKETNET_STATUS_NO_TRANSPORT_FOUND`.
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
  - `-DROCKETNET_XDP_MEMORY_MIB` (default `8192` MiB) bounds planned AF_XDP
    transport + UMEM memory across `num_xdp_threads`.

## Operational tuning with environment overrides

For full workload sizing formulas and deterministic memory tables, see
[`docs/LOAD.md`](LOAD.md). Common operational uses of the environment layer
include:

- **Pinning or narrowing the transport list per host**: set
  `ROCKETNET_SERVER_TRANSPORT=gso` (or `portable`, the fallback, not a
  production server transport) on
  machines or containers that do not use `xdp` or `io_uring`, so both
  `rocketnet_server_required_memory` and `rocketnet_server_create` size for the
  intended transport rather than reserving worst-case `xdp` allocator headroom.
  Each transport's network thread count is its own variable (the table in
  [Network threads, per transport](#network-threads-per-transport)), and
  `ROCKETNET_SERVER_NUM_OS_CPUS` (`1 .. 256`) sets the cores left to the
  operating system before the Linux transports' threads. Preserve the pinned
  AF_XDP geometry: 4,096-byte frames, a 16,384-entry send queue, 32,768-entry
  receive queue and fill set, a 65,536-entry fill ring, and 98,304 UMEM frames
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
