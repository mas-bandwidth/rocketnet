# Windows transport guide

This page describes how RocketNet runs on Windows: the three server transport
backends (`portable`, `iocp`, and `rio`), the client's dedicated I/O thread,
driver selection, Winsock and operating-system requirements, UDP segmentation
and receive coalescing offloads (`USO` and `URO`), the Registered I/O (`RIO`)
pinned memory layout, dual-stack IPv4/IPv6 socket behavior, Winsock lifetime,
Windows Defender Firewall configuration, and building and running the test
suites on Windows.

For the cross-platform driver contract and custom driver callbacks, see
[DRIVERS.md](../DRIVERS.md). For client thread mechanics across all platforms,
see [CLIENT-IO.md](CLIENT-IO.md). For the full build and CMake reference, see
[BUILDING.md](../BUILDING.md).

## 1. Server transports and the client on Windows

### Server transports

A Windows server has three built-in UDP socket backends, declared in
[sdk/rocketnet_drivers.inc](../sdk/rocketnet_drivers.inc) and implemented in
[sdk/rocketnet_server_transport.cpp](../sdk/rocketnet_server_transport.cpp),
[sdk/rocketnet_server_iocp_windows.cpp](../sdk/rocketnet_server_iocp_windows.cpp),
and [sdk/rocketnet_server_rio_windows.cpp](../sdk/rocketnet_server_rio_windows.cpp):

| Driver | Ladder rank | Threading model | I/O path | Best fit |
| --- | --- | --- | --- | --- |
| `portable` | `30` | 1 recv thread + 1 send thread | Non-blocking Winsock `sendto` / `recvfrom` with `select` wait | Default ladder choice; smallest memory footprint; universal fallback |
| `iocp` | `40` | `1..32` IOCP worker threads (`num_xdp_queues`, default `1`) | Overlapped socket (`WSA_FLAG_OVERLAPPED`), `CreateIoCompletionPort`, `WSASendMsg` (`USO`) / `WSASendTo` and `WSARecvMsg` (`URO`) / `WSARecvFrom` | High packet-rate multi-core servers; kernel batching via UDP segmentation and receive coalescing offloads |
| `rio` | `45` | `0` background threads (main-thread polled) | Registered I/O (`WSA_FLAG_REGISTERED_IO`), `RIOSendEx` (`RIO_MSG_DEFER` + `RIO_MSG_COMMIT_ONLY`), `RIOReceiveEx`, `RIODequeueCompletion` | Low-jitter tick loops that want zero worker threads and pre-pinned kernel buffer descriptors |

An in-memory `loopback` driver (`ROCKETNET_DRIVER_PIN_ONLY`, rank `1000`) is
also compiled in for process-local tests and never participates in `auto`.

### Client on Windows

Windows clients always use the `portable` client driver (rank `30` in
[sdk/rocketnet_drivers.inc](../sdk/rocketnet_drivers.inc)). On Windows,
[sdk/rocketnet_platform.h](../sdk/rocketnet_platform.h) defines
`ROCKETNET_PLATFORM_HAS_WAKE`, which activates the dedicated client I/O thread
in [sdk/rocketnet_client_transport.cpp](../sdk/rocketnet_client_transport.cpp)
(see [CLIENT-IO.md](CLIENT-IO.md) for the full design):

- **Main thread stays off Winsock:** `rocketnet_client_update` never calls
  `sendto` or `recvfrom`. Outgoing payloads are staged into one of two lock-free
  payload rings (`payload_ring[2]`), and incoming datagrams are drained from
  one of two lock-free receive buffers (`receive_buffer[2]`).
- **Time-critical I/O worker:** `rocketnet_client_transport_create` starts a
  dedicated worker thread and raises it to `THREAD_PRIORITY_TIME_CRITICAL` in
  `rocketnet_platform_thread_realtime`
  ([sdk/rocketnet_platform.cpp](../sdk/rocketnet_platform.cpp)).
- **Dual-handle wake:** `rocketnet_platform_wake_t` pairs a manual-reset event
  (`CreateEventA`) for outgoing main-thread notifications with a socket event
  (`WSACreateEvent` bound via `WSAEventSelect(s, sock_event, FD_READ)`).
  `rocketnet_platform_wake_wait` blocks in `WaitForMultipleObjects` on both
  handles so the worker wakes immediately when either the game queues a packet
  or the NIC delivers a datagram.
- **High-resolution sleep without global timer side effects:** `rocketnet_sleep`
  uses a per-thread high-resolution waitable timer
  (`CreateWaitableTimerExW` with `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`,
  Windows 10 1803+) for the coarse wait and finishes the last `<= 50 us` with
  a `QueryPerformanceCounter` + `SwitchToThread()` yield loop, without calling
  process-wide `timeBeginPeriod(1)` when the high-resolution timer is present.

## 2. Selecting or pinning a server transport

The driver registry orders candidates by ascending rank, and `auto` (or an
empty `transport` string) walks the ladder from lowest rank to highest:
`portable` (`30`), then `iocp` (`40`), then `rio` (`45`). Because `portable`
always succeeds on a healthy Winsock stack, `auto` selects `portable` unless you
name `iocp`, `rio`, or an explicit fallback list.

You can select or pin a transport in code via `rocketnet_server_config_t`
([sdk/rocketnet.h](../sdk/rocketnet.h)) or from the environment via
`ROCKETNET_SERVER_TRANSPORT` ([sdk/rocketnet_config.h](../sdk/rocketnet_config.h)).
Precedence across all configuration knobs is `default < struct < environment`.

### In `rocketnet_server_config_t`

Pin a single backend so `rocketnet_server_create` fails if that backend is
unavailable:

```c
rocketnet_server_config_t config = rocketnet_server_config_default();
config.transport = "iocp";        // or "rio", or "portable"
config.num_xdp_queues = 4;        // IOCP worker thread count (1..32; 0 means 1)
```

Or provide a comma-separated preference list so RocketNet tries `rio` first,
falls back to `iocp`, and finally falls back to `portable`:

```c
rocketnet_server_config_t config = rocketnet_server_config_default();
config.transport = "rio,iocp,portable";
```

### In `ROCKETNET_SERVER_TRANSPORT`

Override the server transport at launch without recompiling. In `cmd.exe`:

    set ROCKETNET_SERVER_TRANSPORT=iocp
    bin\Release\functional.exe --run test_iocp_uso_uro_probe_and_roundtrip

Or in PowerShell:

    $env:ROCKETNET_SERVER_TRANSPORT = "rio"
    bin\Release\functional.exe --run test_rio_roundtrip

For `iocp`, `ROCKETNET_SERVER_NUM_XDP_QUEUES` overrides `config.num_xdp_queues`
(`1..32` worker threads; `0` selects `1` worker).

### Inspecting the granted transport

At runtime, query `rocketnet_server_transport_name(server)`
([sdk/rocketnet.h](../sdk/rocketnet.h)), which returns `"portable"`, `"iocp"`,
`"rio"`, `"loopback"`, or `"custom"`. At `ROCKETNET_LOG_LEVEL_INFO`, startup
also logs the granted backend:

- `iocp`: `iocp transport: <N> workers on port <port>, USO <yes|no ...>, URO <yes|no ...>`
- `rio`: `rio transport: port <port>, 4096 receive slices, 16384 send frames per phase, 61079552 registered bytes`
- All backends: `server transport started on <address> [<driver>]`

## 3. Windows version and Winsock requirements

RocketNet targets `x86_64` Windows (`_WIN32_WINNT = 0x0601` at compile time in
[CMakeLists.txt](../CMakeLists.txt)) and loads newer Winsock extensions
dynamically per socket so the same binary runs across desktop and server
releases:

| Rung / feature | Minimum Windows version | Winsock / kernel requirement | Runtime availability probe |
| --- | --- | --- | --- |
| `portable` (server & client) | Any supported Windows (`x86_64`) | Winsock 2.2 (`ws2_32.dll`) | Always available (`rocketnet_server_portable_available` returns `true`) |
| `iocp` baseline | Any supported Windows (`x86_64`) | `WSASocketW(..., WSA_FLAG_OVERLAPPED)`, `CreateIoCompletionPort`, `WSASendTo`, `WSARecvFrom`, `GetQueuedCompletionStatusEx` | Always available (`rocketnet_server_iocp_available` returns `true`) |
| `iocp` + `USO` (UDP Segmentation Offload) | Windows 11 / Windows Server 2022+ | `IPPROTO_UDP` `UDP_SEND_MSG_SIZE` (`2`) socket option + `WSASendMsg` (`WSAID_WSASENDMSG`) | Probed per bound socket at `rocketnet_server_iocp_create` |
| `iocp` + `URO` (UDP Receive Segment Coalescing Offload) | Windows 11 / Windows Server 2022+ | `IPPROTO_UDP` `UDP_RECV_MAX_COALESCED_SIZE` (`3`) socket option + `WSARecvMsg` (`WSAID_WSARECVMSG`) + `UDP_COALESCED_INFO` cmsg | Probed per bound socket at `rocketnet_server_iocp_create` |
| `rio` (Registered I/O) | Windows 10 version 1709 / Windows Server 2016+ | `WSASocketW(..., WSA_FLAG_REGISTERED_IO)` + `SIO_GET_MULTIPLE_EXTENSION_FUNCTION_POINTER` (`WSAID_MULTIPLE_RIO`) with `RIOSendEx`, `RIOReceiveEx`, `RIODequeueCompletion` | `rocketnet_server_rio_available()` opens a probe socket and checks the extension table before `rocketnet_server_rio_create` |

## 4. IOCP, `USO`/`URO` per-socket probing, and live fallback

The `iocp` driver ([sdk/rocketnet_server_iocp.h](../sdk/rocketnet_server_iocp.h),
[sdk/rocketnet_server_iocp_windows.cpp](../sdk/rocketnet_server_iocp_windows.cpp))
binds one overlapped UDP socket, attaches it to an I/O completion port, posts
`128` receives per worker (`ROCKETNET_IOCP_RECEIVES_PER_WORKER`), and pre-allocates
`16,384` send slots (`ROCKETNET_IOCP_SEND_QUEUE_SIZE`) and `2 x 1,024` receive
staging slots per worker (`ROCKETNET_IOCP_RECEIVE_QUEUE_SIZE`). Completions are
drained in batches of up to `64` entries via `GetQueuedCompletionStatusEx`.

### Per-socket `USO` and `URO` probing at create

When `rocketnet_server_iocp_create` opens and binds the UDP socket, it probes
both offloads on that bound socket rather than trusting an OS version check:

1. **`USO` probe:** Queries the `WSASendMsg` extension pointer via
   `WSAIoctl(SIO_GET_EXTENSION_FUNCTION_POINTER, WSAID_WSASENDMSG)` and tests
   `setsockopt(s, IPPROTO_UDP, UDP_SEND_MSG_SIZE, &probe_seg, sizeof(probe_seg))`
   with `probe_seg = 1200`. If both succeed, `ROCKETNET_IOCP_FEATURE_USO`
   (`1u << 0`) is set in the transport's feature mask.
2. **`URO` probe:** Queries the `WSARecvMsg` extension pointer via
   `WSAIoctl(SIO_GET_EXTENSION_FUNCTION_POINTER, WSAID_WSARECVMSG)` and calls
   `setsockopt(s, IPPROTO_UDP, UDP_RECV_MAX_COALESCED_SIZE, &max_coalesced, sizeof(max_coalesced))`
   with `max_coalesced = 16384` (`ROCKETNET_IOCP_URO_BUFFER_BYTES`). If both
   succeed, `ROCKETNET_IOCP_FEATURE_URO` (`1u << 1`) is set, each posted receive
   uses a `16,384`-byte receive buffer with `WSARecvMsg`, and completions read
   the `IPPROTO_UDP` / `UDP_COALESCED_INFO` control message to split coalesced
   super-datagrams into individual frames by stride.

If either probe is refused by the kernel or Winsock stack, that direction stays
on per-datagram `WSASendTo` or `WSARecvFrom` (with `1,536`-byte receive buffers
when `URO` is absent) and the server starts normally.

### Silent refusal on the wire and fallback to `WSASendTo`

A socket can pass the `setsockopt(UDP_SEND_MSG_SIZE)` probe at create time and
still have a multi-segment `WSASendMsg` refused on the first actual send (for
example, when routing across loopback or a virtual adapter whose miniport does
not support segmentation).

During `rocketnet_server_iocp_flush_send`:

- Consecutive frames queued to the same destination with an equal segment stride
  (and an optional shorter final segment) are grouped by
  `rocketnet_coalesce_send_run` ([sdk/rocketnet_coalesce.h](../sdk/rocketnet_coalesce.h))
  up to `64` segments (`ROCKETNET_IOCP_USO_MAX_SEGMENTS`) and `65,000` total
  bytes (`ROCKETNET_IOCP_USO_MAX_SUPER_BYTES`).
- Single-frame runs always take the plain `WSASendTo` fast path without calling
  `setsockopt`.
- Multi-frame runs update `UDP_SEND_MSG_SIZE` when the segment stride changes
  and submit one scatter/gather `WSASendMsg` across the run's in-place frame
  buffers (`WSABUF[64]`, zero-copy).
- If `setsockopt(UDP_SEND_MSG_SIZE)` fails or `WSASendMsg` fails with
  `WSAEINVAL` or `WSAEOPNOTSUPP`, the driver immediately:
  1. clears `ROCKETNET_IOCP_FEATURE_USO` from the live feature mask,
  2. resets the socket's `UDP_SEND_MSG_SIZE` option to `0`,
  3. increments `ROCKETNET_IOCP_USO_FALLBACKS`, and
  4. transmits every frame in the refused run individually via `WSASendTo` so
     no datagram is dropped.

Subsequent flushes on that transport go straight to `WSASendTo`. The internal
feature bitmask and counters (`ROCKETNET_IOCP_USO_SENDS`,
`ROCKETNET_IOCP_URO_BATCHES`, `ROCKETNET_IOCP_USO_FALLBACKS`) are exposed via
`rocketnet_server_iocp_features` and `rocketnet_server_iocp_counters` in
[sdk/rocketnet_server_iocp.h](../sdk/rocketnet_server_iocp.h).

## 5. `RIO` single-registration layout and memory footprint

The `rio` driver ([sdk/rocketnet_server_rio.h](../sdk/rocketnet_server_rio.h),
[sdk/rocketnet_rio_layout.h](../sdk/rocketnet_rio_layout.h),
[sdk/rocketnet_server_rio_windows.cpp](../sdk/rocketnet_server_rio_windows.cpp))
eliminates per-syscall page pinning and background worker threads by registering
a single contiguous memory region with the Windows kernel at startup and driving
send and receive completions on the caller's thread.

### Single `VirtualAlloc` + `RIORegisterBuffer` layout

`rocketnet_rio_compute_layout` ([sdk/rocketnet_rio_layout.h](../sdk/rocketnet_rio_layout.h))
packs four 64-byte-aligned sub-arrays into one allocation returned by
`VirtualAlloc(..., MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)` and registers the
entire block in a single `RIORegisterBuffer` call, producing one `RIO_BUFFERID`
shared by every `RIO_BUF` descriptor:

| Sub-array | Count | Element stride | Offset (bytes) | Size (bytes) | Purpose |
| --- | --- | --- | --- | --- | --- |
| Send frame payloads | `32,768` (`2` phases x `16,384`) | `1,536` B (`1,488` B aligned to `64`) | `0` | `50,331,648` | Two alternating phases of `16,384` wire frames (`ROCKETNET_RIO_SEND_FRAMES`) |
| Send `SOCKADDR_INET` slots | `32,768` (`2` phases x `16,384`) | `64` B (`28` B aligned to `64`) | `50,331,648` | `2,097,152` | Per-frame destination `SOCKADDR_INET` for `RIOSendEx` |
| Receive payload slices | `4,096` (`ROCKETNET_RIO_RECEIVES`) | `2,048` B (`ROCKETNET_RIO_RECEIVE_SLICE_BYTES`) | `52,428,800` | `8,388,608` | Pre-posted `RIOReceiveEx` packet buffers |
| Receive `SOCKADDR_INET` slots | `4,096` (`ROCKETNET_RIO_RECEIVES`) | `64` B (`28` B aligned to `64`) | `60,817,408` | `262,144` | Per-slice source `SOCKADDR_INET` filled by `RIOReceiveEx` |
| **Total registered region** | | | | **`61,079,552`** (~58.25 MiB) | Single `RIO_BUFFERID` pinned for the life of the server |

Because `RIORegisterBuffer` pins the region in non-paged physical memory for
kernel DMA, Windows checks the process working-set quota at creation. If
`VirtualAlloc` or `RIORegisterBuffer` fails, `rocketnet_server_rio_create`
deregisters any partial handle, frees the region with `VirtualFree`, and returns
`NULL` so a ladder specification such as `"rio,iocp,portable"` falls back
cleanly to `iocp`.

### Main-thread polling and deferred send commit

- **Two completion queues, zero worker threads:** `rio` creates a receive
  completion queue (`4,096` entries) and a send completion queue (`32,768`
  entries) with `RIOCreateCompletionQueue` and attaches both to the request
  queue created by `RIOCreateRequestQueue`.
- **Receive ring:** All `4,096` receive slices are posted upfront via
  `RIOReceiveEx`. On each `rocketnet_server_update`, `rocketnet_server_rio_swap_receive`
  drains completions in batches of up to `256` (`ROCKETNET_RIO_DEQUEUE_BATCH`)
  via `RIODequeueCompletion`, copies valid datagrams into the active receive
  queue, and reposts each slice immediately. If `RIOReceiveEx` transiently
  refuses a repost, the slice's `receive_posted` flag stays `0` and the next
  poll retries reposting every unposted slot before dequeuing completions.
- **Deferred sends (`RIO_MSG_DEFER` + `RIO_MSG_COMMIT_ONLY`):** Outgoing frames
  are written directly into the current send phase (`0` or `1`). On flush, the
  driver drains completed sends from the send CQ, queues each new frame with
  `RIOSendEx(..., RIO_MSG_DEFER)`, and issues a single
  `RIOSendEx(rq, NULL, 0, NULL, NULL, NULL, NULL, RIO_MSG_COMMIT_ONLY, NULL)`
  to kick the entire batch to the kernel in one transition before flipping
  `send_phase ^= 1`.

## 6. Dual-stack IPv6 (`IPV6_V6ONLY = 0`) and `SIO_UDP_CONNRESET`

Windows defaults differ from Linux on two UDP socket behaviors that affect game
servers. All three Windows server drivers (`portable`, `iocp`, and `rio`) and
the portable socket layer normalize both at socket creation:

### Dual-stack IPv6 (`IPV6_V6ONLY = 0`)

On Windows, a newly created `AF_INET6` socket has `IPV6_V6ONLY` enabled by
default, which rejects IPv4 traffic on `[::]:<port>`. Before calling `bind` on
an `AF_INET6` socket, RocketNet clears `IPV6_V6ONLY`:

```c
DWORD v6only = 0;
setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&v6only, sizeof(v6only));
```

With `IPV6_V6ONLY = 0`:

- Binding `[::]:40000` (or the default wildcard server address) accepts both
  native IPv6 datagrams and IPv4 datagrams delivered as IPv4-mapped IPv6
  addresses (`::ffff:a.b.c.d`).
- On receive, `rocketnet_sockaddr_to_address`
  ([sdk/rocketnet_address.cpp](../sdk/rocketnet_address.cpp)) normalizes
  `IN6_IS_ADDR_V4MAPPED` addresses back to `ROCKETNET_ADDRESS_IPV4`, so peer
  addresses and connection tables see canonical IPv4 addresses regardless of
  whether the socket was bound to `0.0.0.0` or `[::]`.
- On send, when an `AF_INET6` dual-stack socket transmits to a
  `ROCKETNET_ADDRESS_IPV4` peer, RocketNet encodes the destination as an
  IPv4-mapped `sockaddr_in6` (`::ffff:a.b.c.d`) so one dual-stack socket serves
  both address families.

### Suppressing `SIO_UDP_CONNRESET` (`WSAECONNRESET` on UDP)

By default on Windows, if a UDP send reaches a local or remote host whose port
is closed and an ICMP "Port Unreachable" packet returns, Winsock surfaces
`WSAECONNRESET` (`10054`) on the next `recvfrom` / `WSARecvFrom` / `RIOReceiveEx`
on that socket. On a multi-client UDP server socket, a single disconnected peer
returning ICMP Port Unreachable would otherwise poison posted receives for all
other clients.

Every UDP socket created by [sdk/rocketnet_platform.cpp](../sdk/rocketnet_platform.cpp),
[sdk/rocketnet_server_iocp_windows.cpp](../sdk/rocketnet_server_iocp_windows.cpp),
and [sdk/rocketnet_server_rio_windows.cpp](../sdk/rocketnet_server_rio_windows.cpp)
disables `SIO_UDP_CONNRESET` immediately after `socket` / `WSASocketW`:

```c
BOOL new_behavior = FALSE;
DWORD bytes_returned = 0;
WSAIoctl(s, SIO_UDP_CONNRESET, &new_behavior, sizeof(new_behavior),
         NULL, 0, &bytes_returned, NULL, NULL);
```

In addition, if a transient `WSAECONNRESET` ever completes on an IOCP or RIO
receive slot, the completion handler drops that error completion and reposts the
receive descriptor without tearing down the transport.

## 7. `WSAStartup` / `WSACleanup` lifetime in `rocketnet_init` / `rocketnet_term`

RocketNet manages its own Winsock 2.2 reference inside `rocketnet_init` (and
`rocketnet_init_ex`) and `rocketnet_term` (and `rocketnet_term_ex`) via
`rocketnet_platform_init` and `rocketnet_platform_term` in
[sdk/rocketnet_platform.cpp](../sdk/rocketnet_platform.cpp) and
[sdk/rocketnet.cpp](../sdk/rocketnet.cpp):

1. **`rocketnet_init()` / `rocketnet_init_ex()`:** Calls
   `WSAStartup(MAKEWORD(2, 2), &WsaData)` before initializing libsodium. If
   `WSAStartup` returns non-zero, initialization fails with
   `ROCKETNET_STATUS_PLATFORM_INIT_FAILED` (`ROCKETNET_ERROR` in the simple
   API). Calling `rocketnet_init()` when already initialized is a safe no-op.
2. **Coexistence with engine Winsock calls:** Winsock reference-counts
   `WSAStartup` and `WSACleanup` per process. An engine or application that
   calls `WSAStartup` before `rocketnet_init` and `WSACleanup` after
   `rocketnet_term` works without conflict because each layer balances its own
   pair.
3. **`rocketnet_term()` / `rocketnet_term_ex()`:** Refuses to tear down Winsock
   while any client or server allocation is still live. If
   `rocketnet_allocations != 0`, `rocketnet_term_ex` logs an error and returns
   `ROCKETNET_STATUS_LIVE_ALLOCATIONS` without calling `WSACleanup()`. Always
   destroy every `rocketnet_client_t` and `rocketnet_server_t` before calling
   `rocketnet_term()`.

## 8. Windows Defender Firewall for CI and dedicated servers

Every RocketNet server binds a UDP wildcard address (`[::]:40000` or
`0.0.0.0:40000`). On an interactive Windows desktop, the first time an unknown
binary such as `server.exe`, `functional.exe`, or your dedicated server binds a
wildcard port, Windows Defender Firewall raises a modal prompt. Loopback traffic
(`127.0.0.1` and `[::1]`) succeeds regardless of whether that dialog is accepted
or dismissed, but remote LAN or internet clients are blocked until an inbound
UDP rule allows the binary or port, and unattended CI runners or headless
dedicated servers should register an explicit rule in an elevated shell.

To add and remove a non-interactive inbound UDP rule for a dedicated server port
(or test runner) from an elevated `cmd.exe` or PowerShell session, use
`netsh advfirewall firewall`:

    netsh advfirewall firewall add rule name="RocketNet UDP 40000" dir=in action=allow protocol=UDP localport=40000
    netsh advfirewall firewall delete rule name="RocketNet UDP 40000" protocol=UDP localport=40000

`add rule` prints `Ok.` when the inbound rule is installed, and `delete rule`
prints `Deleted 1 rule(s).` followed by `Ok.` when removing it. In production,
scope the rule to the server executable (`program="C:\path\to\server.exe"`) or
the network profiles (`profile=private,public`) from which remote clients dial.

## 9. Building and running the unit and functional suites on Windows

Building on Windows requires Visual Studio 18 (Visual Studio 2026) with the
**Desktop development with C++** workload, CMake 4.2 or later, and Python 3
(`py -3`); see [BUILDING.md](../BUILDING.md) for prerequisite details.

From the repository root in `cmd.exe` or PowerShell, configure the x64 Visual
Studio solution, build `Release`, and run the unit and functional suites:

    cmake -B build -A x64
    cmake --build build --config Release --parallel 4
    bin\Release\test.exe
    bin\Release\functional.exe

What each step prints:

- `cmake -B build -A x64` selects the Visual Studio x64 toolchain and ends with
  `-- Configuring done`, `-- Generating done`, and
  `-- Build files have been written to: .../build`.
- `cmake --build build --config Release --parallel 4` compiles the vendored
  `sodium`, `rocketnet`, `rocketnet_shared`, examples, `test.exe`, and
  `functional.exe` into `bin\Release\`.
- `bin\Release\test.exe` runs the socket-free unit and simulator suite in
  parallel, printing each `[RUN] test_...` line and finishing with the ten
  slowest `test_timing_... [SKIPPED ...]` lines.
- `bin\Release\functional.exe` exercises real loopback sockets across
  `portable`, `iocp` (including `USO` and `URO` probing, round-trip, and
  coalescing), and `rio` (including availability, round-trip, and completion
  poll bounds), then prints the `slowest 10 of ...:` summary table.

To run a single IOCP or RIO functional test case directly:

    bin\Release\functional.exe --run test_iocp_uso_uro_probe_and_roundtrip
    bin\Release\functional.exe --run test_rio_roundtrip

Each single-case command prints `[RUN] <test_name>`, executes that test on a
live Windows loopback socket, and exits `0` after printing the 1-test summary
table.
