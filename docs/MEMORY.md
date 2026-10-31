# Memory, capacity, and host resources

Use this guide before choosing a server size or allocating a fixed arena.
The public config and planning functions in [rocketnet.h](../sdk/rocketnet.h)
define the inputs; [LOAD.md](LOAD.md) explains the detailed storage model.
Commands and C examples here run from the repository root.

## Planning order

1. Choose the workload: clients, channels, largest message, packet rate, and
   send/receive budgets. Keep server and client channel configurations compatible.
2. Select a transport or candidate list. Keep configuration and environment
   unchanged between planning and creation.
3. Query the required allocator memory. A zero result is a refused configuration,
   not a zero-cost endpoint. Account separately for OS resources and custom transports.
4. Check host plans, descriptor/memlock limits, and shutdown peak memory.
5. Measure the full lifecycle and active workload on the target hosts.

## Qualification and scale

The default build supports a runtime `max_clients` of up to 1000. The
compile-time ceiling can be raised as described under [Maximum clients](#maximum-clients).
Recorded 1000-client
correctness and LAN runs do not qualify every workload or establish the
100-microsecond main-call target. Above 1000 clients, users must research,
stabilize, and qualify their own configuration. Large configurations can also
spike in shutdown memory; the 1000-client example soak can run out of memory
on Linux.

The [LAN proof index](runs/lan-1000-proof.md) records runs on 2026-10-25:
Linux GSO and io_uring at `1ca477339e150220f90e96bbe7c61be5b175eb51`, macOS msgx
at `813b4adbe3dec2eee496577e3da2c0d7d66b8063`, and AF_XDP copy mode at
`534cce2e10159b52074ea9b10f7a2f0b074f7298`. They retained 1000 connected clients
for 600 seconds (300 for AF_XDP), with minimum per-client rates of 276–283 kbps.
Read the individual records for topology, workload, measurements, and timing
failures. The [preview-2 record](QUALIFICATION-v1.0.0-preview-2.md) separately
records 60-second non-timing correctness rungs. None of these earlier-head
results is a measurement of your current build. See [Testing](TESTING.md).

## Server memory footprint

An illustrative default 1000-client estimate recorded in this guide is about
20.9 GB (20,908,851,166 bytes), with a marginal cost of about 19.0 MB
(19,033,780 bytes) per slot. These are sizing examples, not portable constants:
build options, platform, transport availability, and configuration affect the
result. Query your own build with `rocketnet_server_required_memory` before
allocating. It reports the planned allocator peak over create, start, stop,
restart, and destroy for the largest candidate transport:

```c
#include "rocketnet.h"
#include <stdio.h>

int main( void )
{
    struct rocketnet_server_config_t config;
    rocketnet_server_default_config( &config );

    config.max_clients = 1000;
    const size_t bytes_1000 = rocketnet_server_required_memory( &config );
    config.max_clients = 1;
    const size_t bytes_1 = rocketnet_server_required_memory( &config );
    config.max_clients = 2;
    const size_t bytes_2 = rocketnet_server_required_memory( &config );

    printf( "1000 clients: %zu\n1 client: %zu\n2 clients: %zu\ndelta: %zu\n",
            bytes_1000, bytes_1, bytes_2, bytes_2 - bytes_1 );
    return 0;
}
```

Build a Release tree, save the program as `measure_server_memory.c`, and run:

```sh
cc -I sdk measure_server_memory.c build/librocketnet.a -lbpf -lxdp -luring -lstdc++ -lpthread -lm -o measure_server_memory
./measure_server_memory
```

On Linux x86-64 with io_uring and XDP built (the default development build) it
prints `1000 clients: 20908851166`, `1 client: 1894073074`, `2 clients: 1913106854`
and `delta: 19033780` in the build used for this recorded example. The figures
differ by platform and
build, because each counts the largest transport that platform's ladder can grant.

The byte counts in this section are recorded sizing examples, not a fresh
measurement of the current source. Some predate the figures in
[LOAD.md](LOAD.md#deterministic-memory-planning-and-regenerated-sizing-examples).
Use the planner from the exact library you will deploy.

A light-client server uses `rocketnet_server_light_config`, which fills the
defaults and then sizes for one 1,500-byte MTU packet per 100 ms to each
client at 10 Hz (10 * 1,500 * 8 = 120,000 bits/s, about 120 kbps). The
per-client send budget is 256 kbps (256 / 120 = 2.13). A message holds one
packet's payload, `ROCKETNET_MAX_PACKET_PAYLOAD_BYTES - 1` (1,415 bytes), and
`max_fragmented_messages` is 1. Over the 10,000 ms reliable timeout left at
its default, that rate is 100 messages; a 1,415-byte message can take two
window numbers, so the reliable window is 256. The receive buffer is 142,400
bytes, the send buffer is 141,500 bytes, the send queue is 128 messages,
packets in flight are 256, each update may send 4 packets and 4 unreliable
messages, 100 sequenced messages are held, 128 receipts are kept, and there
are eight channels. At `max_clients` 1000, `rocketnet_server_required_memory`
reports 3,231,507,666 bytes (about 3.2 GB) on Linux x86-64 with io_uring
and XDP built. The matching `rocketnet_client_light_config` makes
`rocketnet_client_required_memory` report 22,896,245 bytes.

Both figures are the lifecycle peak the caller's allocator sees: create, the
transport connect or start allocates for the largest transport the ladder can
grant, disconnect or stop, the next cycle, and destroy. They exclude the
memory the operating system owns (worker thread stacks, XDP UMEM, io_uring
rings, kernel socket buffers) and process overhead. In a production build
(`ROCKETNET_DEV=OFF`) they are 3,225,111,302 and 22,110,441 bytes. Other
platforms report their own figures (macOS has no io_uring or XDP, so its
server figure is about half). On Windows x64 with MSVC, the server/client
figures are 1,591,206,044 / 22,968,879 bytes in development builds and
1,584,809,680 / 22,183,075 bytes with `ROCKETNET_DEV=OFF`. The default profile
remains the stock acceptance.

```c
#include "rocketnet.h"

#include <stdio.h>

struct rocketnet_client_t * start_client( const char * token, const struct rocketnet_client_config_t * config )
{
    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
    struct rocketnet_client_t * client = rocketnet_client_create( NULL, config, &status );
    if ( !client )
        printf( "create refused: %s\n", rocketnet_status_name( status ) );
    if ( client && rocketnet_client_connect( client, token ) != ROCKETNET_STATUS_OK )
    {
        rocketnet_client_destroy( client );
        return NULL;
    }
    return client;
}

struct rocketnet_server_t * start_server( const struct rocketnet_server_config_t * config )
{
    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
    struct rocketnet_server_t * server = rocketnet_server_create( NULL, "0.0.0.0:40000", NULL, config, &status );
    if ( !server )
        printf( "create refused: %s\n", rocketnet_status_name( status ) );
    if ( server && rocketnet_server_start( server ) != ROCKETNET_STATUS_OK )
    {
        rocketnet_server_destroy( server );
        return NULL;
    }
    return server;
}

void request_stop( struct rocketnet_client_t * client, struct rocketnet_server_t * server )
{
    rocketnet_client_disconnect( client );
    rocketnet_server_stop( server );
}

/* Call once per frame after request_stop; keep the event loop running. */
bool finish_stop( struct rocketnet_client_t * client, struct rocketnet_server_t * server )
{
    if ( !rocketnet_client_disconnected( client ) || !rocketnet_server_stopped( server ) )
        return false;
    rocketnet_client_destroy( client );
    rocketnet_server_destroy( server );
    return true; /* Both pointers are now invalid; do not call again. */
}
```

To check a config, create with it and read the status out-parameter, then
destroy the result if it was never connected or started. For an active object,
request shutdown and poll completion before destroy. Destroy requires completed
shutdown and then joins and frees resources synchronously. Its diagnostic
fallback for an early destroy can retain the object while a worker owns memory;
do not use that misuse path as a cleanup strategy. Keep allocator/callback
contexts and all borrowed storage alive through completion and the subsequent
destroy. A completion query neither frees the object nor promises receipt by
the other side of the connection.

## Receive buffer sizing

Between two updates the server's socket holds every packet that arrives.
The portable transport's receive worker reads the socket into one of three
banks and publishes a bank after every read. While main holds the other
banks the worker has no free bank, stops reading the socket and waits
(`PORTABLE_RX_WAIT_FULL`), so the traffic that arrives in that time waits in
the kernel's receive buffer, and a buffer too small for it drops datagrams
before the server sees them. Portable and msgx server sockets therefore ask for:

```
receive buffer bytes = ceil( max_clients x 1000 kbps x 125 x 2 / 10 Hz )
                       at least 1,000,000, at most INT_MAX
```

that is, `max_clients` at the default client send budget
(`ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS`, 125,000 bytes/s per client), over
the slowest supported update interval (0.1 s at 10 Hz), times 2 for the
kernel's per-packet accounting, which charges more than the packet bytes. At
1000 clients that is 25,000,000 bytes; at 64 clients, 1,600,000. Servers
updating faster than 10 Hz need proportionally less; the request stays sized
for 10 Hz. The io_uring and iocp transports use the same formula with the build's
`ROCKETNET_MAX_CLIENTS`. GSO instead requests `ROCKETNET_SOCKET_RECEIVE_BUFFER_SIZE`
(1,000,000 bytes) on each socket; it does not use this client-count formula.
The request is an upper bound: the kernel charges only for what it holds.

The kernel may grant less. Linux clamps the request to `net.core.rmem_max`
(about 208 KB by default) without an error; macOS and the BSDs refuse a
request above `kern.ipc.maxsockbuf` and the server walks down by halves. The
server reads the grant back after create and logs one line with what it
granted and what it asked for; when the grant is below the request that line
is a warning naming the limit to raise. On Linux, for 1000 clients:

```
sudo sysctl -w net.core.rmem_max=25000000
```

A grant below one update's inbound traffic drops packets in the kernel
before the server sees them; at 1000 clients sending 256 kbps each, one 10 Hz
update carries 3.2 MB of payload, a 20 Hz update 1.6 MB and a 30 Hz update
1.07 MB (measured on the loopback, tests/functional/server_receive_low_tick.cpp:
3.46 MB, 1.83 MB and 1.30 MB of wire bytes per update, with 0 kernel
receive drops). The portable server is a portable implementation with no
performance guarantee: that case reports the kernel's receive drop count and
never fails on it, and runs when `ROCKETNET_LOW_TICK_MEASURE=1` names it.

## Maximum clients

`ROCKETNET_MAX_CLIENTS` (`sdk/rocketnet.h`) is the most clients one server can
hold. It is 1000 by default, the supported server size, and a build may set
another value from 512 through 32768:

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DROCKETNET_MAX_CLIENTS=10000

or `-DROCKETNET_MAX_CLIENTS=10000` on every compile when building the sources
into your own build. CMake hands the value to the libraries, their consumers
(a `PUBLIC` definition, so the header a game compiles matches its library),
the tests, the tools and the embedded XDP program; build `server_xdp.o` by hand
with `make ROCKETNET_MAX_CLIENTS=10000`. Rebuild everything together. A build
that embeds the XDP program with its own copy of `cmake/EmbedXdp.cmake` passes
nothing at the defaults and `-DMAX_CLIENTS=` (and `-DHISTORY_PACKETS=`) at
another value; the generated `rocketnet_server_xdp.h` refuses to compile into a
library whose `ROCKETNET_MAX_CLIENTS` or `ROCKETNET_HISTORY_PACKETS` differs
from the program's.

An earlier 10000-client build has a bounded correctness record in
[QUALIFICATION-max-clients-10000.md](QUALIFICATION-max-clients-10000.md).
[`.github/workflows/nightly-max-clients.yml`](../.github/workflows/nightly-max-clients.yml)
is configured to repeat that build/load tier on the Linux fleet runner:
zero-warning builds with gcc and clang at DEV ON and OFF, the unit suite, and
a bounded loopback load of all 10000 clients for 30 seconds
(`LOAD_PROFILE=light ./bin/load 10000 30`). The load uses the light
per-client config, about 25 GB planned for the server and its 10000
in-process clients, because the default per-client config plans about
183 GB for the server alone (below); the runner checks host room against the
plan before the load runs. Those byte figures belong to the recorded build;
query the current binary. This is a same-process loopback correctness tier,
not a performance or general workload qualification above 1000 clients.

The value raises the ceiling, not the default: `max_clients` still defaults
to 1000 (or the build's maximum when that is smaller), and a server asks for
more in its config or with `ROCKETNET_SERVER_MAX_CLIENTS`. The following limits
scale with it at compile time. Backend staging also has fixed limits, described
below; raising the client ceiling does not make every transport buffer larger.

| Limit | Rule | 1000 | 10000 |
|---|---|---|---|
| client slots, loopback endpoints | one per client (endpoints: plus the server) | 1000, 1001 | 10000, 10001 |
| per-update receive budget, payload batch | 16 packets per client | 16000 | 160000 |
| portable send batch | one per client | 1000 | 10000 |
| server-wide send staging (loopback queue, IOCP send frames, RIO send phase) | two packets per client, rounded up to a power of two | 2048 | 32768 |
| IOCP receive staging per worker, RIO posted receives | twice the send staging | 4096 | 65536 |
| XDP kernel replay map, userspace replay marks | one entry per client, rounded up to a multiple of 1024 | 1024 | 10240 |
| XDP reply MAC table | four slots per client, rounded up to a power of two | 4096 | 65536 |
| reassembly memory budget (`ROCKETNET_REASSEMBLY_MEMORY_MIB` left empty) | 512 MiB per 1000 clients | 512 MiB | 5120 MiB |

The server packet lease table is fixed at 131072 entries and 65536 leases per
update, enough for two packets per client up to the 32768 build ceiling.
XDP has 4096 send slots per queue and fixed UMEM geometry; increasing its
queue count adds staging, subject to memory and host limits.

GSO and io_uring differ: GSO has three shared send banks of 4096 slots, with
one bank available to producers at a time; io_uring has one shared 2048-slot
send array, including slots still held by pending completions. These are
server-wide capacities. Adding receive queues does not multiply either
producer's capacity. A fill can therefore refuse before it has offered one
packet to every client in a large server. The current staged-capacity query
also prevents the message layer's full-buffer rotation from providing a
fairness guarantee on these backends. Reliable messages remain queued, but
later clients can be deferred indefinitely under sustained earlier-client
backlog. See [Current staging and drain limitations](TRANSPORT-INTERNALS.md#current-staging-and-drain-limitations).
The replay-history depth remains a separate setting (below).

Memory is the caller's choice, made through the config. The library sizes
what the config asks for and sets no budget on the total: every field is held
to its own range (and to what its layout can represent), and a config whose
fields are all in range is accepted however much memory they add up to. The
only limits on the total are ones the caller sets or can raise: the CMake
component budgets and the operator memory budgets below.

Memory grows with the clients a server asks for, at the per-client depth the
config sets: `rocketnet_server_required_memory` gives the lifecycle allocator
peak before create, including the largest planned built-in transport in the
configured ladder. Query the exact build and configuration for its requirement;
a 10000-client server can set lighter per-client limits (send and receive
buffers, queues, windows, receipts and reassembly slots) for its game.
A build for more clients also carries larger fixed per-server tables (the
receive arrays and payload batches above), about 0.6 GB more at 10000 whatever
the server's `max_clients`.

## History memory budget

`cmake -B build -DROCKETNET_HISTORY_PACKETS=8192` chooses the per-connection
sequence-history depth. The minimum and default are 4096, the maximum 32768;
values must be multiples of 64. CMake propagates the value consistently to both
libraries and consumers of internal headers. Rebuild all such consumers together.
This does not change packet encoding and does not configure a send or ACK rate.
Increasing it increases both per-connection memory and bounded ACK processing.

Separate build trees may select `CMAKE_RUNTIME_OUTPUT_DIRECTORY` and
`CMAKE_LIBRARY_OUTPUT_DIRECTORY` to keep sanitizer/configuration outputs apart.

Large allocation components have independent CMake budgets:
`ROCKETNET_REASSEMBLY_MEMORY_MIB` defaults to 512 MiB per 1000 clients (512 in
the default build; see "Maximum clients") and `ROCKETNET_XDP_MEMORY_MIB`
to 4096. Both accept 1 through 1048576 MiB. Reassembly includes its slot records,
arena and delivered mirror; XDP includes userspace queue structs and UMEM.
Oversized geometry is refused with required and permitted byte counts before
allocation. These bounds are not an available-RAM check: reserve additional
memory for ACK history, other transport objects, allocator overhead, stacks,
kernel buffers and your application. Each default-geometry XDP queue has
192 MiB of raw UMEM, in addition to
its allocator storage and other host resources. Query the exact build and
queue count rather than using older total-memory figures.

## Pre-allocation server resource plans

`rocketnet_server_required_memory(config)` returns the peak bytes held by the
allocator over create, start, stop, restart and destroy for `config` at its
`max_clients`, including planned built-in transport allocations and the
development link simulator in development builds. The bound assumes unchanged
configuration and environment overrides; OS-owned memory and custom-transport
allocations are excluded. It works before
`rocketnet_init`, opens no sockets, starts no workers and allocates no memory.
Pass `NULL` config for defaults. The query reads the supplied configuration and
applies environment overrides (matching create), and performs the create-time configuration checks, including the reassembly
component budget. It does not validate start-time keys, probe hardware, or
promise that a transport will pass its own resource checks at start. `rocketnet_client_required_memory` is
the client form. The detailed per-transport plan and the kernel-facing host memory
estimate are internal to the transport layer (`sdk/rocketnet_layers.h`), not part
of the public client and server API.

The client's corresponding transport plan is applied at connect, after the
token selects an address and the transport ladder. For portable and msgx it
includes the client transport block, reassembly storage, send arena, socket,
wake object and one SDK thread allocation. The normal client runs one background
network thread; its operating-system stack reserve, kernel socket queues and
application memory are outside that allocator plan. The
`ROCKETNET_CLIENT_MEMORY_BUDGET_BYTES` operator limit compares candidates with
this allocator plan before opening them.

The internal transport plan, allocated at start, includes the transport, protocol tables and ACK history, reassembly and
its delivered mirror, send arena, native transport queues and SDK worker allocations,
plus the SDK allocator's ownership/alignment overhead. XDP UMEM is counted
separately and included in the total. Worker startup shims are included even if
some have already been freed by the time create returns. OS thread stacks, kernel
socket/ring storage, libbpf/libxdp allocations, libc bookkeeping, loader and
application memory are outside this count. Provision room for those costs; a
plan is not an available-RAM check or a whole-host peak estimate. Existing
component budgets, queue defaults and history limits are unchanged.

The internal transport layer's host memory estimate adds the kernel-facing
memory a transport requests to the plan, under the same config and transport
rules, with no allocation, socket or thread. Each field is a
request, never what the kernel grants: `allocator_bytes` (the plan without
UMEM), `umem_bytes` (XDP frame memory, the plan's UMEM), `socket_send_buffer_bytes`
and `socket_receive_buffer_bytes` (the `SO_SNDBUF` and `SO_RCVBUF` sizes asked of
every socket the transport opens), `xdp_ring_bytes` (the AF_XDP RX and TX
descriptor rings of every queue) and `thread_stack_bytes` (`worker_threads` x
`thread_stack_bytes_per_thread`; transports create threads with the platform
default stack, so on POSIX this is the default thread attribute's stack size,
set by `ulimit -s` on glibc, and on Windows the executable's 1 MiB default
reserve). `total_bytes` is their sum and is never below `required_memory`.
`exclusions` names what is not counted: kernel socket accounting beyond the
requests (Linux doubles each buffer request), the AF_XDP fill and completion
rings (the server asks for `ROCKETNET_XDP_FILL_RING_SIZE`, 32768 fill entries,
twice the receive frames it keeps on the ring, and for
`XSK_RING_CONS__DEFAULT_NUM_DESCS`, 2048 completion entries, in
`rocketnet_xdp_umem_config`, sdk/rocketnet_xdp_socket_config.h), libbpf/libxdp and the XDP program's maps, the io_uring
submission, completion and provided buffer rings, thread guard pages
and TLS, kernel page tables, loader memory, libc bookkeeping outside the SDK
allocator, and application memory. Stacks count reserved address space, not
resident pages. Statistics run on the ack path and add no thread. The current
default Linux worker topology is:

| transport | sockets | I/O workers | total stacks |
| --- | --- | --- | --- |
| portable server | 1 | 2 | 2 |
| gso | 4 | 5 | 5 |
| io_uring | 4 | 0 | 0 |
| xdp | 7 | 7 | 7 |

The default Windows server topology is one socket and two I/O workers for IOCP
(one completion worker and one transmit worker), or one socket and one I/O
worker for RIO. A portable server has the same two I/O workers as the Linux
row. On macOS, msgx and the portable server also have one receive worker and
one transmit worker. These server counts do not include client objects; each
normal client adds its own one background network thread.

Multiply the total stacks by the host's default stack reserve. Use the probe
below for byte totals from the exact build and configured geometry; allocator
sizes also depend on platform layout and compile-time limits.

Build tools normally, then use the resource probe:

```
./bin/transport_memory 1000 1416 16 portable 0 --plan-only
# A live portable server probe works in either build mode.
# Linux live GSO probe:
./bin/transport_memory 1000 1416 16 gso 0
# Host requests, including stack reserves, without creating the server:
./bin/transport_memory 1000 1416 16 gso 0 --session-plan-only
# Linux only; this prints geometry without attaching XDP or allocating UMEM:
./bin/transport_memory 1000 1416 1 xdp 7 --plan-only
python3 tools/check_memory.py
```

Arguments are clients, a legacy payload-size placeholder, reassembly slots,
concrete transport and queue count (zero selects that transport's default).
The payload argument is retained for command compatibility; the plan uses
`ROCKETNET_TRANSPORT_MAX_PAYLOAD_BYTES`. IOCP and RIO ignore the queue count.
`--plan-only` emits
a component breakdown, worker/socket counts and an explicit unknown OS/application
cost, then exits before initialization. `--session-plan-only` reports the public
server block plus its active transport's SDK allocations and host requests,
including socket buffers and thread stack reserves, without creating a server.
Run it without server configuration overrides in the environment, including development simulator overrides.
Without either option, the probe creates a loopback server, compares independent allocator live/peak observations against the plan,
reports process peak RSS separately and checks that destruction frees every SDK
allocation. Clear deployment `ROCKETNET_*` overrides when profiling a specific
argument set: real create still obeys those overrides and a mismatch fails loudly.
RSS and requested allocator bytes measure different things; untouched allocations
may not yet be resident. The probe never commits an XDP attachment.

Record the exact build, geometry and output when reporting byte totals. A live
measurement may be below the constructor plan because worker startup shims are
freed as their threads begin; the plan includes all of them. This does not relax
the requirement that destruction frees every SDK allocation. Large legal numeric
reassembly geometries can be planned without allocating them, while component
budgets can still refuse creation.

`test_transport_control` exercises one complete 20-slot stats modulo cycle plus
a publication update over 1000 mixed ACK/keepalive clients: 51 updates and 50
manual worker steps at that client count. The buffered transport accepts only
256 controls per send batch, and the test retains per-update control assertions.
Its elapsed time is a diagnostic for that synthetic workload, not a packet rate
or a frame/send/ACK cadence requirement.
