<a id="native-transports"></a>

# Choosing and deploying transports

## Choosing a transport

Most games never name a transport: leave `transport` `NULL` and each endpoint
takes `auto`, the platform's order below, and the first transport that starts
wins. Read the result with `rocketnet_server_transport_name` or
`rocketnet_client_transport_name`. The ranks are the registry's
(`sdk/rocketnet_drivers.inc`); a lower rank is tried first, "pin only" means
`auto` never picks it and only a list of that one name selects it (in a
longer list it is skipped), and "-" means that endpoint
does not have the transport.

| Transport | Platform | Server rank | Client rank | What it needs from the host |
| --- | --- | --- | --- | --- |
| `xdp` | Linux | 10 | - | effective UID 0, permission to raise `RLIMIT_MEMLOCK` for the 4096-byte UMEM frames, a NIC driver with AF_XDP, a real port (not 0), and a build with XDP compiled in; see [docs/LINUX.md](LINUX.md), section 6 |
| `gso` | Linux | 20 | 10 | nothing privileged; uses UDP GSO and GRO where the kernel has them, plain `sendmmsg`/`recvmmsg` where it does not; `num_gso_workers` at most 32 |
| `io_uring` | Linux | 25 | - | a build configured with liburing, and a kernel that allows io_uring; `num_io_uring_workers` at most 32 |
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
| Every listed transport not compiled into this build (`xdp` alone, in a build that compiled XDP out); in a longer list it refuses and the next is tried | `rocketnet_server_start` fails with `ROCKETNET_STATUS_TRANSPORT_NOT_BUILT` |
| Every listed transport refused to start (no privilege, no RIO, memory budget) | `rocketnet_server_start` or `rocketnet_client_connect` fails with `ROCKETNET_STATUS_NO_TRANSPORT_FOUND`; the log names each refusal |
| A public address with port 0 and only transports that cannot report the port they were given (`xdp`) | `ROCKETNET_STATUS_TRANSPORT_PORT_ZERO_UNSUPPORTED` |
| Every listed transport unable to serve both requested address families | `ROCKETNET_STATUS_TRANSPORT_FAMILIES_UNSUPPORTED` |
| The platform refused the bind (another socket holds the address and port, the bind address is not this machine's, or permission was denied) on every socket transport listed, any `xdp` refusal aside | `rocketnet_server_start` fails with `ROCKETNET_STATUS_TRANSPORT_BIND_REFUSED` |
| A bind address start cannot use (for example one that does not parse, carries a port, names a family the server does not serve, is multicast or broadcast, or a `bind_*` field set on a server of both families), or every listed transport refusing a Windows lane or worker count above 1 on the wildcard | `rocketnet_server_start` fails with `ROCKETNET_STATUS_INVALID_ADDRESS` |

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

GSO and io_uring stage sends per queue: each worker has its own send banks,
and a destination always stages in its own queue. A GSO queue has a fixed
size, so more GSO workers add send staging. io_uring sizes its queues at
create from `max_clients`, so at 1000 clients its total stays about the same
from 2 to 8 workers and each queue gets smaller; more io_uring workers add
send work in parallel, not room. Under staging pressure the message-layer rotation and the
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

Plan cores in this order: the operating system first, then the transport's
network threads, then the main thread and the game threads. Each transport
that has a count to choose has its own config field and environment variable
([ENVIRONMENT.md](ENVIRONMENT.md#network-threads-per-transport)); the default,
with nothing set, is a count that held the measured peak workload: 1000
players at 60 Hz, about 900,000 packets a second from the server (an estimated
9.39 Gb/s), on the machine in [PERFORMANCE.md](PERFORMANCE.md) (which has
no `msgx` rows yet).

| Transport | Network threads (default) | Where they run | Machine setting that must match |
| --- | --- | --- | --- |
| `xdp` | `num_xdp_threads` (1), one per NIC queue | thread `q` on cpu `num_os_cpus + q`, never cpu 0 | the NIC's combined queue count: [below](#xdp-match-the-nic-queues-to-the-threads) |
| `gso` | `num_gso_workers` (1), one per socket | worker `q` on cpu `num_os_cpus + q`, not cpu 0 by default (`gso_reserve_cpu_0`) | none |
| `io_uring` | `num_io_uring_workers` (2), one per socket and ring | worker `q` on cpu `num_os_cpus + q`, not cpu 0 by default (`io_uring_reserve_cpu_0`) | none |
| `iocp` | `num_iocp_completion_workers` (2 on a bind address, 1 on the wildcard) and `num_iocp_send_threads` (2) | worker `w` on the first processor of core `w`, the send threads on the cores after | none; a bind address for more than one worker ([WINDOWS.md](WINDOWS.md#to-reach-10-gbs-on-windows-set-a-bind-address)) |
| `rio` | `num_rio_lanes` (5 on a bind address, 1 on the wildcard), one worker a lane | lane `l` on core `l` from core 0 | none; a bind address for more than one lane |
| `msgx` | `num_msgx_transmit_workers` (3) and one receive worker | where macOS puts them | none |
| `portable` | 2, fixed | Linux: send worker on cpu `num_os_cpus`, receive worker on `num_os_cpus + 1`, not cpu 0 by default (`portable_reserve_cpu_0`); elsewhere where the OS puts them | none |

A value past a transport's limit is refused at create, never clamped. The
one check made later is `iocp`'s send threads against its send lanes (8 at
1000 clients): above them, `iocp` refuses at start with a warning, and start
tries the next transport in the list (with `iocp` alone, start fails with
`NO_TRANSPORT_FOUND`). On
Linux, `gso`, `io_uring` and `portable` each have a setting that gives cpu 0
to their threads instead, thread `q` on cpu `q`
([ENVIRONMENT.md](ENVIRONMENT.md#cpu-0-per-linux-transport)); `xdp` keeps cpu 0
for its provisioning's housekeeping and interrupts. A pin the machine refuses
never stops a server: it is warned, counted and the thread asks for no
real-time priority. The priorities each thread takes are in
[the implementation reference](TRANSPORT-INTERNALS.md#server-network-threads-and-their-priority).

### XDP: match the NIC queues to the threads

`xdp` runs one thread per NIC queue, so the NIC's combined queue count must
equal `num_xdp_threads`; the library checks and never changes it.
`scripts/rocketnet-provision` sets it, and given no count it takes the
server's: `ROCKETNET_SERVER_NUM_XDP_THREADS` from its environment, or the
default, 1. `sudo` drops an exported variable by default, so give the count
as an argument, as below, or set the variable on the `sudo` line.

1. Choose the thread count, or take the default of 1:
   `ROCKETNET_SERVER_NUM_XDP_THREADS=2` (or `num_xdp_threads = 2`).
2. Provision the NIC with the same count, before the server starts:
   `sudo scripts/rocketnet-provision eth0 2`, then
   `sudo scripts/rocketnet-provision eth0 2 verify`.
3. Start the server: it runs 2 XDP threads, on cpus 1 and 2 with the default
   `num_os_cpus` of 1.
4. When the counts differ the server does not start `xdp`; the log says

   ```text
   xdp pre-flight: eth0 has 4 combined queues and the server runs 2 XDP threads (num_xdp_threads; R7): provision the NIC with `sudo scripts/rocketnet-provision eth0 2`, or set num_xdp_threads (ROCKETNET_SERVER_NUM_XDP_THREADS) to 4
   ```

   and `auto` falls to `gso` (with `xdp` alone, start returns
   `NO_TRANSPORT_FOUND`).
5. Read the NIC's current count with `ethtool -l eth0`: the `Combined` line
   under "Current hardware settings".

The full condition list and a worked machine layout are in
[docs/LINUX.md](LINUX.md), section 6.

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
