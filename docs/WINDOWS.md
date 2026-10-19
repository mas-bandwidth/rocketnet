# RocketNet on Windows

This page is for a team shipping a game client and a dedicated server on
Windows. It covers which server transport to run, what each one needs from
the host, what the client does, and how to prove a machine is ready. Each
claim names the source file it is true in. Every command on this page runs
from the repository root in `cmd.exe` unless it says PowerShell.

## Choosing a server transport

A Windows build carries three UDP server transports and the in-process
`loopback` transport. The registry is `sdk/rocketnet_drivers.inc`; the
adapters that create each one are in `sdk/rocketnet_driver_builtin.cpp`.

| Transport | What it is | Threads | Source |
| --- | --- | --- | --- |
| `portable` | Plain Winsock `sendto` and `recvfrom` on one UDP socket. | none of its own | `sdk/rocketnet_platform.cpp` |
| `iocp` | One overlapped UDP socket on an I/O completion port, with UDP segmentation offload (USO) on send and UDP receive offload (URO) on receive where the host grants them. | 1 completion worker by default, up to 32 | `sdk/rocketnet_server_iocp_windows.cpp`, `sdk/rocketnet_server_iocp.h` |
| `rio` | Registered I/O: one UDP socket opened with `WSA_FLAG_REGISTERED_IO`, one registered memory region, polled completion queues. | none; it runs on the thread that calls the server | `sdk/rocketnet_server_rio_windows.cpp`, `sdk/rocketnet_server_rio.h` |
| `loopback` | In-memory, one process, for tests. Never chosen by `auto`; pin it by name. | none | `sdk/rocketnet_driver_loopback.cpp` |

### The default and why

The transport field of `rocketnet_server_config_t` is an ordered candidate
list, comma separated, tried left to right at `rocketnet_server_start`; the
first transport that starts wins (`sdk/rocketnet.h`). `NULL`, the empty string
and `"auto"` mean the platform's order, which `sdk/rocketnet_driver.cpp` builds
from the server ranks in `sdk/rocketnet_drivers.inc`, lower rank first:
`portable` (30), then `iocp` (40), then `rio` (45). `portable` starts on any
working Winsock stack, so `auto` on Windows runs `portable`, and `iocp` or
`rio` runs only when you name it. `BUILDING.md` ("Windows IOCP server") states
the reason: IOCP is an explicit selection, and Windows `auto` keeps `portable`
first.

### Pinning a transport

A one-element list is a pin: that transport or a loud failure, never a silent
fall to another (`sdk/rocketnet.h`). In code:

```c
struct rocketnet_server_config_t config;
rocketnet_server_default_config( &config );
config.transport = "iocp";              /* a pin */
/* or: config.transport = "rio,iocp,portable";  try rio, then iocp, then portable */
```

`rocketnet_server_start` returns `ROCKETNET_STATUS_TRANSPORT_LADDER_EXHAUSTED`
when every candidate refuses (`sdk/rocketnet.h`). After a start,
`rocketnet_server_transport_name` returns the transport that won, and the
log has `server transport started on <address> [<transport>]`
(`sdk/rocketnet_server_transport.cpp`).

The environment overrides the struct: precedence is default < struct <
environment (`BUILDING.md`, "Server transports"; `sdk/rocketnet_config.cpp`).
`ROCKETNET_SERVER_TRANSPORT` holds the same list. In `cmd.exe`:

    set ROCKETNET_SERVER_TRANSPORT=iocp
    bin\Release\functional.exe --run driver_env_pin_is_honoured
    set ROCKETNET_SERVER_TRANSPORT=

In PowerShell:

    $env:ROCKETNET_SERVER_TRANSPORT = "rio"; bin\Release\functional.exe --run driver_env_pin_is_honoured; Remove-Item Env:ROCKETNET_SERVER_TRANSPORT

`driver_env_pin_is_honoured` starts a server with the default config under
the pin and checks that the pinned transport is the one that started
(`functional/rocketnet_functional_tests.cpp`). A name that is no transport
anywhere fails loudly and lists the valid names (`sdk/rocketnet.h`).

The IOCP worker count is the config's `num_xdp_queues` field, or
`ROCKETNET_SERVER_NUM_XDP_QUEUES`: 0 is one worker, 1 through 32 are valid,
and anything else refuses the `iocp` start with a log line
(`sdk/rocketnet_driver_builtin.cpp`, `sdk/rocketnet_server_iocp_windows.cpp`).
`rio` takes no worker count.

### Decision table

| You want | Use |
| --- | --- |
| A server that starts on every Windows machine with nothing to tune | `auto` (runs `portable`) |
| Overlapped I/O and the host's USO and URO offloads | pin `iocp` |
| No transport threads, with a fixed registered memory region | pin `rio` |
| One config line for a fleet whose hosts differ | a list, for example `rio,iocp,portable` |
| A test with no sockets | pin `loopback` |

## IOCP offloads

USO lets one send carry a run of same-size datagrams that the stack splits on
the way out; URO lets the stack hand one receive a run of datagrams from one
source, which the transport splits at the stride the stack reports
(`sdk/rocketnet_coalesce.h`, `sdk/rocketnet_server_iocp_windows.cpp`).
`sdk/rocketnet_server_iocp.h` gives Windows 11 and Windows Server 2022 or
later as the hosts that have them.

Neither is assumed from the Windows version. `iocp` probes both on its bound
socket each time it is created (`sdk/rocketnet_server_iocp_windows.cpp`):

- USO: setting the `UDP_SEND_MSG_SIZE` socket option succeeds.
- URO: the `WSARecvMsg` extension loads and setting
  `UDP_RECV_MAX_COALESCED_SIZE` to 16384 succeeds.

A refused probe is a tier, not an error: that direction uses one `WSASendTo`
or `WSARecvFrom` per datagram and the server starts normally. A USO send that
the host refuses later with `WSAEINVAL` or `WSAEOPNOTSUPP` turns USO off for
that server and sends the run again one datagram at a time, so no datagram is
lost (`sdk/rocketnet_server_iocp_windows.cpp`).

What the host gave is in the info line `iocp` logs when it starts
(`sdk/rocketnet_server_iocp_windows.cpp`); info is the default log level
(`sdk/rocketnet.cpp`):

    iocp transport: 1 workers on port 40000, USO yes, URO yes

A refused probe reads `USO no (WSASendTo per datagram)` or
`URO no (WSARecvFrom per datagram)`. Behind the line, the transport keeps
`ROCKETNET_IOCP_USO_SENDS`, `ROCKETNET_IOCP_URO_BATCHES` and
`ROCKETNET_IOCP_USO_FALLBACKS` counters (`sdk/rocketnet_server_iocp.h`). They
are internal to the SDK, not part of `sdk/rocketnet.h`; the functional case
`test_server_iocp_uso_uro_probe` prints this host's answer:

    bin\Release\functional.exe --run test_server_iocp_uso_uro_probe

## RIO

`rio` needs Windows 10 version 1709 or later, for `RIOSendEx` and
`RIOReceiveEx` (`sdk/rocketnet_server_rio.h`). It opens its socket with
`WSA_FLAG_REGISTERED_IO` and loads the RIO function table; if either is
refused, its start fails and a candidate list moves on to the next transport
(`sdk/rocketnet_server_rio_windows.cpp`).

At start it makes one `VirtualAlloc` region and registers it once with
`RIORegisterBuffer`. `sdk/rocketnet_rio_layout.h` carves it into 2 phases of
16384 send frames, a destination address per send frame, 4096 receive slices
of 2048 bytes, and a source address per receive slice, each element rounded
up to 64 bytes and the total rounded up to a 4096-byte page. The region is
held for the life of the server, and its size is in the info line `rio` logs
when it starts (`sdk/rocketnet_server_rio_windows.cpp`):

    rio transport: port 40000, 4096 receive slices, 16384 send frames per phase, 61079552 registered bytes

`rio` sends with `RIO_MSG_DEFER` and one commit per flush, and polls its send
and receive completion queues from `rocketnet_server_update` and
`rocketnet_server_send_packets` on the caller's thread
(`sdk/rocketnet_server_rio.h`). Choose it when the server's own threads
must be the only threads: it has no worker of its own. No measurement in this
repository yet shows it faster than `iocp` on a network card; `auto` never
puts it ahead of `iocp` (`sdk/rocketnet_drivers.inc`).

## The client on Windows

The Windows client has one transport, `portable`: its client rank is the only
nonzero one on Windows in `sdk/rocketnet_drivers.inc`. Windows defines
`ROCKETNET_PLATFORM_HAS_WAKE` (`sdk/rocketnet_platform.h`), so a connected
client runs its socket work on its own I/O thread, created at connect
(`sdk/rocketnet_client_transport.cpp`). That thread waits on a manual-reset
event and on the socket's `FD_READ` through `WSAEventSelect`, so it wakes when
the game queues a packet or a datagram arrives (`sdk/rocketnet_platform.h`).
Set `step_driven = 1` in `rocketnet_client_config_t`, or
`ROCKETNET_CLIENT_STEP_DRIVEN=1`, to run that work inside
`rocketnet_client_update` and `rocketnet_client_send_packets` on the caller's
thread with no I/O thread (`sdk/rocketnet.h`).

`rocketnet_sleep` (`sdk/rocketnet.cpp`) waits on a per-thread high-resolution
waitable timer (`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`, Windows 10 version
1803 or later), and yields with `SwitchToThread` for a remainder of at most
50 microseconds. It leaves the system-wide timer resolution alone. Where that
timer cannot be made, it raises the timer resolution to 1 ms with
`timeBeginPeriod` only for the length of that one `Sleep`
(`sdk/rocketnet_platform.cpp`).

## IPv4, IPv6 and dual stack

A server serves one family, the family of its address (`sdk/rocketnet.h`),
and binds the wildcard address of that family on its port
(`sdk/rocketnet_server_transport.cpp`). Every Windows server transport clears
`IPV6_V6ONLY` on an IPv6 socket (`sdk/rocketnet_platform.cpp`,
`sdk/rocketnet_server_iocp_windows.cpp`,
`sdk/rocketnet_server_rio_windows.cpp`), and a server created at `[::]:40000`
also advertises its IPv4 wildcard (`sdk/rocketnet_server_transport.cpp`), so
it takes IPv4 clients too, as v4-mapped addresses that the `iocp` and `rio`
transports report as plain IPv4 (`sdk/rocketnet_server_iocp.h`,
`sdk/rocketnet_server_rio.h`). A server created at an IPv4 address takes IPv4
only. The functional case `test_transport_dual_stack_ipv4_client` creates a
server at `[::]` and connects a client to it at `127.0.0.1`
(`functional/rocketnet_functional_tests.cpp`).

The client picks its socket family from the server's address: an IPv6 server
needs an IPv6 stack, and an IPv4 server is reached over a dual-stack socket
where the host has IPv6 (`test_unit_client_bind_family.cpp`).

Every Windows socket also turns off `SIO_UDP_CONNRESET`, so an ICMP port
unreachable from one departed client does not fail the receives of the rest
(`sdk/rocketnet_platform.cpp`, `sdk/rocketnet_server_iocp_windows.cpp`,
`sdk/rocketnet_server_rio_windows.cpp`).

## Winsock lifetime

`rocketnet_init` calls `WSAStartup(MAKEWORD(2,2))`, and a refusal fails the
init with `ROCKETNET_STATUS_PLATFORM_INIT_FAILED`. `rocketnet_term` calls
`WSACleanup` once every client and server is destroyed; while any is still
alive it refuses, logs an error and leaves Winsock started
(`sdk/rocketnet.cpp`, `sdk/rocketnet_platform.cpp`). A second `rocketnet_init`
without a `rocketnet_term` between them returns false
(`ROCKETNET_STATUS_ALREADY_INITIALIZED`) and starts nothing.

Winsock counts `WSAStartup` and `WSACleanup` per process, so a game that
makes its own pair keeps it balanced around its own use and the two pairs do
not interfere. Call `rocketnet_term` before your own last `WSACleanup` if
RocketNet's lifetime nests inside yours.

## Firewall and first run

A server binds the wildcard address of its family on its port
(`sdk/rocketnet_server_transport.cpp`), so on an interactive desktop the first
start of a server in a program raises the Windows Defender Firewall prompt
(`BUILDING.md`). Loopback
traffic (`127.0.0.1`, `[::1]`) works whatever is chosen there, so local tests
need no rule (`BUILDING.md`).

A dedicated server that remote clients reach needs an inbound UDP rule. From
an elevated prompt:

    netsh advfirewall firewall add rule name="RocketNet UDP 40000" dir=in action=allow protocol=UDP localport=40000
    netsh advfirewall firewall delete rule name="RocketNet UDP 40000"

`add rule` prints `Ok.` and `delete rule` prints `Deleted 1 rule(s).` then
`Ok.`. In production, narrow
the rule with `program=` to the server executable and `profile=` to the
networks clients connect from.

## Building

Windows builds with CMake and Visual Studio 18 (MSVC x64);
[BUILDING.md](../BUILDING.md) lists what to install once.

    cmake -B build -A x64
    cmake --build build --config Release --parallel 4

The build makes the static library `rocketnet` and the DLL
`rocketnet_shared`, plus `_dev` variants of each that the tests and examples
link (`CMakeLists.txt`). The DLL is written as `rocketnet.dll` with the import
library `rocketnet_import.lib`, so it never collides with the static
`rocketnet.lib` (`CMakeLists.txt`). The CMake package exports
`RocketNet::rocketnet` and `RocketNet::rocketnet_shared`; `BUILDING.md`
("Installing and embedding the SDK") shows the install and the
`target_link_libraries` line. In a build tree the DLLs land in `bin\Release\`
and the libraries in `build\Release\`:

    dir /b bin\Release\rocketnet*.dll
    dir /b build\Release\rocketnet*.lib

[PLATFORMS.md](PLATFORMS.md) is the support claim. Windows x86_64 with MSVC is
supported, with `portable`, `iocp` and `rio`. clang-cl is not named there, so
it is not supported. Windows arm64 is unsupported.

## Verifying a machine

Build, then run the unit suite and the functional suite once each:

    bin\Release\test.exe
    bin\Release\functional.exe

`test.exe` uses no sockets; `functional.exe` runs real loopback sockets and
the Windows-only cases (`functional/rocketnet_functional_tests.cpp`). Then
prove each transport on its own:

| Transport | Cases |
| --- | --- |
| `iocp` | `test_server_driver_pin_iocp`, `test_server_iocp_uso_uro_probe`, `test_server_iocp_uso_wire` |
| `rio` | `test_server_driver_pin_rio`, `test_server_rio_probe`, `test_server_rio_wire_ipv4`, `test_server_rio_wire_ipv6`, `test_server_rio_wire_dual_stack`, `test_server_rio_send_full` |
| each of `portable`, `iocp`, `rio` | `test_transport_udp_loopback`, `test_transport_ipv6_loopback`, `test_transport_dual_stack_ipv4_client`, `driver_env_pin_is_honoured`, with `ROCKETNET_SERVER_TRANSPORT` set to that transport |

Run one case with `--run`:

    bin\Release\functional.exe --run test_server_rio_probe

The last row is the transport family matrix that `.github/workflows/ci.yml`
runs. In PowerShell:

    foreach ($t in "portable","iocp","rio") { foreach ($c in "test_transport_udp_loopback","test_transport_ipv6_loopback","test_transport_dual_stack_ipv4_client","driver_env_pin_is_honoured") { $env:ROCKETNET_SERVER_TRANSPORT = $t; bin\Release\functional.exe --run $c; "$t $c exit $LASTEXITCODE" } }; Remove-Item Env:ROCKETNET_SERVER_TRANSPORT

Every line must end `exit 0`. `test_iocp.exe` holds the IOCP and RIO
completion, failure and shutdown tests (`test_iocp.cpp`):

    bin\Release\test_iocp.exe
