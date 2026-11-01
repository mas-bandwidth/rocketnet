<a id="native-transports"></a>

# Choosing and deploying transports

## Choosing a transport

Most games never name a transport: leave `transport` `NULL` and each endpoint
takes `auto`, the platform's order below, and the first transport that starts
wins. Read the result with `rocketnet_server_transport_name` or
`rocketnet_client_transport_name`. The ranks are the registry's
(`sdk/rocketnet_drivers.inc`); a lower rank is tried first, "pin only" means
`auto` never picks it and only a name selects it, and "-" means that endpoint
does not have the transport.

| Transport | Platform | Server rank | Client rank | What it needs from the host |
| --- | --- | --- | --- | --- |
| `xdp` | Linux | 10 | - | effective UID 0, permission to raise `RLIMIT_MEMLOCK` for the 4096-byte UMEM frames, a NIC driver with AF_XDP, a real port (not 0), and a build with XDP compiled in; see [docs/LINUX.md](LINUX.md), section 6 |
| `gso` | Linux | 20 | 10 | nothing privileged; uses UDP GSO and GRO where the kernel has them, plain `sendmmsg`/`recvmmsg` where it does not; `num_xdp_queues` at most 32 |
| `io_uring` | Linux | 25 | - | a build configured with liburing, and a kernel that allows io_uring; `num_xdp_queues` at most 32 |
| `msgx` | macOS | 10 | 10 | the Darwin `sendmsg_x`/`recvmsg_x` batch calls; see [docs/MACOS.md](MACOS.md) |
| `portable` | every platform | development 30, production 50 | 30 | UDP sockets only |
| `iocp` | Windows | 40 | 10 | Winsock overlapped I/O; uses UDP send and receive offload where the stack has them; see [docs/WINDOWS.md](WINDOWS.md) |
| `rio` | Windows | 45 | - | the Winsock Registered I/O extension, and one registered memory region; see [docs/WINDOWS.md](WINDOWS.md) |
| `loopback` | every platform | pin only | pin only | nothing: both ends in one process, no socket; for tests and tools |

So `auto` resolves to:

| Platform | Server, development build | Server, production build | Client |
| --- | --- | --- | --- |
| Linux | `xdp`, `gso`, `io_uring`, `portable` | `xdp`, `gso`, `io_uring`, `portable` | `gso`, `portable` |
| macOS | `msgx`, `portable` | `msgx`, `portable` | `msgx`, `portable` |
| Windows | `portable`, `iocp`, `rio` | `iocp`, `rio`, `portable` | `iocp`, `portable` |

A Linux build without liburing has no `io_uring` row. `rio` ranks after
`iocp`, so `auto` reaches it only when `iocp` refuses; pin it to run it.

**Pinning.** Set `transport` in `rocketnet_server_config_t` or
`rocketnet_client_config_t`, or set `ROCKETNET_SERVER_TRANSPORT` or
`ROCKETNET_CLIENT_TRANSPORT`, which overrides the config and is read once, at
create. The value is `auto`, one name, or a comma list such as `gso,portable`,
tried in that order with nothing appended.

**What a pin that cannot be met returns:**

| Case | Result |
| --- | --- |
| An unknown name, an empty element, or `auto` inside a list | `rocketnet_server_create` or `rocketnet_client_create` fails with `ROCKETNET_STATUS_INVALID_TRANSPORT_LIST`; the log lists the valid names |
| A known name this platform or endpoint does not have (`iocp` on Linux, `gso` on macOS, any server-only transport for a client) | skipped with an info line; the rest of the list is the pin |
| Every name in the list skipped | the endpoint uses its own `auto`, with a warning, so one setting can be deployed to mixed hosts |
| `xdp` named in a build that compiled XDP out | `rocketnet_server_start` fails with `ROCKETNET_STATUS_TRANSPORT_NOT_BUILT` |
| Every listed transport refused to start (no privilege, no RIO, memory budget) | `rocketnet_server_start` or `rocketnet_client_connect` fails with `ROCKETNET_STATUS_TRANSPORT_LADDER_EXHAUSTED`; the log names each refusal |
| A public address with port 0 and only transports that cannot report the port they were given (`xdp`) | `ROCKETNET_STATUS_TRANSPORT_PORT_ZERO_UNSUPPORTED` |
| A transport that cannot serve both requested address families | `ROCKETNET_STATUS_TRANSPORT_FAMILIES_UNSUPPORTED` |

A pin never falls back silently: a list with at least one name this endpoint
has is tried exactly as written, or the call fails. The per-platform guides
show the pins in use: [docs/LINUX.md](LINUX.md),
[docs/MACOS.md](MACOS.md) and [docs/WINDOWS.md](WINDOWS.md). For implementation details, use the [transport internals reference](TRANSPORT-INTERNALS.md).
For provisioning and bind steps, use [Deployment](DEPLOYING.md).

## The registry and the backend interface

See [the implementation reference](TRANSPORT-INTERNALS.md#the-registry-and-the-backend-interface).

## Client transports

See [the implementation reference](TRANSPORT-INTERNALS.md#client-transports).

## Server capabilities and ownership

See [the implementation reference](TRANSPORT-INTERNALS.md#server-capabilities-and-ownership).

## Per-update send and receive budgets

GSO has 4096 slots in its current shared send bank; io_uring has 2048 shared
send slots, including outstanding sends. More receive queues do not increase
these capacities; under staging pressure the message-layer rotation and the
disconnect bursts spread across updates as the transport's send room reports.
Receive drains bound work spent on both kept and discarded datagrams using
backend-specific budgets. See [staging and drain bounds](TRANSPORT-INTERNALS.md#staging-and-drain-bounds),
and the
[capacity table](TRANSPORT-INTERNALS.md#per-update-send-and-receive-budgets)
for each backend's storage geometry.

## UDP coalescing

See [the implementation reference](TRANSPORT-INTERNALS.md#udp-coalescing).

## Creation and shutdown

See [the implementation reference](TRANSPORT-INTERNALS.md#creation-and-shutdown).

## ICMP errors

See [the implementation reference](TRANSPORT-INTERNALS.md#icmp-errors).

## Server network threads and their priority

See [the implementation reference](TRANSPORT-INTERNALS.md#server-network-threads-and-their-priority).

## Adding a backend without editing the transport

See [the implementation reference](TRANSPORT-INTERNALS.md#adding-a-backend-without-editing-the-transport).

## The RIO server transport

See [the implementation reference](TRANSPORT-INTERNALS.md#the-rio-server-transport).

## Windows connection reset

See [the implementation reference](TRANSPORT-INTERNALS.md#windows-connection-reset).

## The io_uring server transport

See [the implementation reference](TRANSPORT-INTERNALS.md#the-io_uring-server-transport).

## The XDP receive filter

See [the implementation reference](TRANSPORT-INTERNALS.md#the-xdp-receive-filter).

## Embedded XDP source maintenance

See [the implementation reference](TRANSPORT-INTERNALS.md#embedded-xdp-source-maintenance).

## Bounded server payload sends

See [the implementation reference](TRANSPORT-INTERNALS.md#bounded-server-payload-sends).

## Receive lengths and truncated datagrams

See [the implementation reference](TRANSPORT-INTERNALS.md#receive-lengths-and-truncated-datagrams).
