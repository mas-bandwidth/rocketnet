# RocketNet on Windows

The page a Windows server operator or engine programmer reads first: how to
build with MSVC, which transport a server and a client get and how to pin one,
what `iocp` and `rio` need from the host, the firewall, what a dead client's
ICMP answers do, the limits for 1000 clients, and the commands that verify a
host, in order. The transport table for every platform is in
[TRANSPORTS.md](TRANSPORTS.md); implementation details are in
[TRANSPORT-INTERNALS.md](TRANSPORT-INTERNALS.md). The support tiers are in [PLATFORMS.md](PLATFORMS.md); this page
does not widen them.

## What is supported

The supported row is Windows 11 on x86_64 with MSVC, transports `portable`,
`iocp` and `rio` ([PLATFORMS.md](PLATFORMS.md), the table). CI builds the whole
tree in Release and runs the happy path on every commit (`happy path /
windows`). Each night it runs three Release legs: a development library
(`ROCKETNET_DEV=ON`) running the unit suite and the lifecycle binary, a
production library (`ROCKETNET_DEV=OFF`) checked as a consumer sees it, with
the transport matrix below, and the functional suite
(`.github/workflows/ci.yml`, `build-and-test-windows`, run by
`nightly-ci.yml`). clang-cl
on x86_64 is a nightly row; Windows on arm64 is build-checked only and
unsupported. The Windows rows in [PERFORMANCE.md](PERFORMANCE.md) ran on
Windows Server 2025, which is not a row of the support table in PLATFORMS.md:
they record what those machines reached, and do not add Windows Server to the
supported platforms.

## Build

Install once: Visual Studio 2026 (Visual Studio 18) with the **Desktop
development with C++** workload, CMake 4.2 or later, and Python 3 from
python.org for the `py` launcher. libsodium is vendored; nothing else is
needed. [BUILDING.md](BUILDING.md) says why each is required. From the
checkout, in cmd.exe or PowerShell, one command per line:

```bat
cmake -B build -A x64
cmake --build build --config Release --parallel 4
```

The executables land in `bin\Release\`. A production library is configured
with `-DROCKETNET_DEV=OFF`; the Debug configuration, AddressSanitizer,
Application Verifier and clang-cl builds are in BUILDING.md, as is installing
the SDK into a game's own build.

## The Windows transports

**`rio` is the server transport to use on Windows.** It is the one measured
to hold the 1000-player peak, about 900,000 packets a second, with no refused
sends: the five- and six-lane rows in [PERFORMANCE.md](PERFORMANCE.md) have
zero message-start refusals. `iocp` remains available and holds the average
load with none, but at the peak it refuses a small share of sends: its two
peak rows there held about 900,000 packets a second with 738 and 7405
message-start refusals over the run. A server's `auto` tries `rio` first, and
falls to `iocp` where `rio` cannot start.

| Transport | Endpoint | `auto` rank | What it is |
| --- | --- | --- | --- |
| `portable` | server and client | server: development 30, production 50; client 30 | one UDP socket; two workers on the server, one I/O worker on the client |
| `iocp` | server and client | server 40; client 10 | server: `num_iocp_completion_workers` completion workers (2 on a bind address, 1 on the wildcard) and `num_iocp_send_threads` send threads (2); client: one I/O worker on its own completion port |
| `rio` | server | 35 | Winsock Registered I/O: per lane (`num_rio_lanes`: 5 on a bind address, 1 on the wildcard) one socket, one registered memory region and one I/O worker polling its completion queues |
| `loopback` | server and client | pin only | in-process, no socket; for tests and tools |

A production server's `auto` is **rio, then iocp, then portable**; a
development server's is **portable, rio, iocp**. The client's `auto` order is
**iocp, then portable**, on the client I/O thread described in
[CLIENT-IO.md](CLIENT-IO.md). The ranks are the rows of
`sdk/rocketnet_drivers.inc`.

`iocp` probes UDP send offload (`UDP_SEND_MSG_SIZE`) and receive offload
(`UDP_RECV_MAX_COALESCED_SIZE`) on its bound socket. A stack without either is
a slower tier, not a refusal: the line it logs when the server starts says
which it got,

```text
iocp transport: 1 completion workers, 2 send threads and 8 send lanes of 4096 on port 40000, USO yes, URO yes
```

with `no (WSASendTo per datagram)` or `no (WSARecvFrom per datagram)` in
place of `yes`. A send offload the stack accepted at start but refuses later
logs a warning and falls back to one send per datagram. `iocp` runs
`num_iocp_completion_workers` completion workers
(`ROCKETNET_SERVER_NUM_IOCP_COMPLETION_WORKERS`, 1 to 32; default 2 on a
specific bind address, 1 on the wildcard) and `num_iocp_send_threads` send
threads (`ROCKETNET_SERVER_NUM_IOCP_SEND_THREADS`, default 2, at most the
send lanes: 8 at 1000 clients, fewer for a smaller server; more is refused
with `CONFIG_OUT_OF_RANGE`). With more than one worker, worker `w` takes the
first logical processor of core `w` (cores in Windows' processor order: the
order the default RSS layout hands receive queues to), its socket bound to
that processor with `SIO_CPU_AFFINITY`; the send threads take one logical
processor each on the cores after the workers'.

`rio` loads the Registered I/O function table from Winsock, allocates and
registers one memory region for every send and receive buffer of each lane,
and starts one I/O worker for each lane. The lanes are `num_rio_lanes`
(`ROCKETNET_SERVER_NUM_RIO_LANES`, 1 to 32; default 5 on a specific bind
address, 1 on the wildcard). With more than one lane, lane `l` takes the
first logical processor of core `l` from core 0 up, in the same order as
`iocp`, and its worker the other logical processors of that core; more lanes
than cores share processors, with a warning. All of this happens when the
server starts, and the line it logs then names each lane's region size:

```text
rio transport: port 40000, 1 lane (one socket and I/O worker each), 4096 receive slices and 16384 send frames per phase per lane, <n> registered bytes per lane
```

Because `rio` ranks ahead of `iocp`, `auto` runs it whenever it starts, and
falls to `iocp` only when `rio` refuses: when Winsock offers no Registered I/O
function table, when `num_rio_lanes` is outside 1 to 32, when more than one
lane is asked for on the wildcard or an IPv4-mapped address, when another
socket holds the lanes' address and port, when its registered region cannot
be allocated or registered, or when its memory plan exceeds the operator's
memory budget. Each refusal is logged with its reason. With no bind address
a server runs one `rio` lane, as `iocp` runs one completion worker (below).
How its send phases and receive banks change hands is in
TRANSPORTS.md, "The RIO server transport".

### To reach 10 Gb/s on Windows set a bind address

The defaults are 5 RIO lanes, or 2 IOCP completion workers with 2 send
threads: the counts that sent the peak load to 1000 players at 60 Hz, about
900,000 packets a second (an estimated 9.39 Gb/s, from the packet rate), on
the machines in [PERFORMANCE.md](PERFORMANCE.md). Measure yours. More than
one lane or worker shares the port by processor, and Windows allows that only
on a specific local address, so those defaults need a bind address. Set the network card's address
in the config, `bind_ipv4_address` (or `bind_ipv6_address`), or in the
environment:

```bat
set ROCKETNET_SERVER_BIND_IPV4_ADDRESS=10.0.0.5
```

Without one, the server binds the wildcard and `rio` and `iocp` run one lane
or one completion worker, and say so once at warning level:

```text
rio transport: running 1 lane because the bind address is the wildcard or IPv4-mapped. The default of 5 lanes needs a specific local address: to reach 10 Gb/s set bind_ipv4_address or bind_ipv6_address (ROCKETNET_SERVER_BIND_IPV4_ADDRESS, ROCKETNET_SERVER_BIND_IPV6_ADDRESS) to the network card's address. To run 1 lane without this warning, set num_rio_lanes (ROCKETNET_SERVER_NUM_RIO_LANES) to 1
```

A count above 1 asked for on the wildcard (`num_rio_lanes` or
`num_iocp_completion_workers`) is refused with `INVALID_ADDRESS`, never run on
one. The server starts whatever the NIC's receive queues are (a flow whose
processor has no lane reaches the first lane bound), but the `rio` rows that
measured the defaults set the NIC's receive-side scaling (RSS) as the worked
example below does; the `iocp` row used the setup in "IOCP at 10 Gb/s". Check
either as "Network adapter setup for rio and iocp" says.

### Worked example: RIO at 10 Gb/s

1. Choose the lane count, or take the default of 5. Lane `l` sits on the first
   logical processor of core `l` from core 0 and its I/O worker on the other
   processor of that core; on a machine with two logical processors a core,
   lane `l` is on processor `2l` and its worker on `2l + 1`, and the lanes use
   processors 0 to `2 x lanes - 1`. Keep the main thread and game threads
   above them (the measured rows ran the main thread on processor
   `2 x lanes`).
2. Set a specific bind address (above):

   ```bat
   set ROCKETNET_SERVER_BIND_IPV4_ADDRESS=10.0.0.5
   set ROCKETNET_SERVER_NUM_RIO_LANES=5
   ```

3. Steer receive to the lanes' processors, one RSS queue a lane, from an
   elevated PowerShell (the adapter name is the one `Get-NetAdapter` lists for
   that address). For 5 lanes on a machine with two logical processors a core:

   ```powershell
   Set-NetAdapterRss -Name "Ethernet 2" -BaseProcessorNumber 0 -MaxProcessorNumber 8 -MaxProcessors 5 -NumberOfReceiveQueues 5
   Get-NetAdapterRss -Name "Ethernet 2"
   ```

   For `L` lanes: `-BaseProcessorNumber 0 -MaxProcessorNumber (2L - 2)
   -MaxProcessors L -NumberOfReceiveQueues L`. The 6-lane rows used
   `-MaxProcessorNumber 10 -MaxProcessors 6 -NumberOfReceiveQueues 6`.
4. Start the server. Its log names each lane's processors (socket on `2l`,
   worker on `2l + 1`); check them against the RSS processors read back in
   step 3, then run the per-processor check in "Network adapter setup for rio
   and iocp" at load.

### Network adapter setup for rio and iocp

Set the adapter up for `rio`, the server transport to use on Windows (see
"The Windows transports" above); the `iocp` part below is for a server that
falls back to it.

The network card's work, its interrupts and the deferred procedure calls
(DPCs) that follow them, runs ahead of every thread on the processor it lands
on. The `rio` lane workers and `iocp` completion workers run at
`THREAD_PRIORITY_TIME_CRITICAL` by default (unless `rocketnet_thread_priority(
false )`, below), so a thread at normal priority cannot take their processors:
a normal-priority busy loop on every `rio` worker processor for 3 s changed
nothing in the measured rows. A realtime-class thread, interrupts and DPCs can.
For `rio`, receive work belongs on each lane's socket processor (`2l`) and
never on its worker's (`2l + 1`); step 3 above places RSS that way. For `iocp`,
see "IOCP at 10 Gb/s" below.

On the measured machines (two logical processors a core), RSS used one logical
processor a core, the even-numbered ones: put the base and maximum on even
processors. Every `Set-NetAdapterRss` in these rows reset the adapter: the ssh
session to the host through it dropped for about 15 s each time, so run it
from the machine's console or from a session that does not go through that
adapter. `Get-NetAdapterRss` reads back the RSS
settings, not where the adapter's interrupts land: in the 6-lane rows below it
read back exactly what was set while interrupts landed on worker processors.
So after every change, check where the work lands, on the running server at or
near its peak load, from PowerShell:

```powershell
(Get-Counter -Counter '\Processor Information(0,*)\% DPC Time','\Processor Information(0,*)\% Interrupt Time' -SampleInterval 2 -MaxSamples 5).CounterSamples | Where-Object { $_.CookedValue -gt 1 -and $_.InstanceName -notlike '*_Total' } | Sort-Object Path | Format-Table Path, CookedValue
```

It lists the processors (instance `0,<processor>`) that spent more than 1% of
a sample in DPCs or interrupts, once for each such sample and counter. The server's log names each worker's processor
(`rio transport: lane <l> socket on processor <p> ..., worker pinned to mask
...`, `iocp transport: worker <w> socket on processor <p> ..., worker pinned to
mask ...`); for `rio` no worker's processor should be in the list. In the
measured 5-lane `rio` rows at the peak load (about 900,000 packets a second),
the socket processors 0, 2, 4, 6 and 8 each spent 38 to 47% of their time in
DPCs (about 40,000 to 47,000 a second each, the `DPCs Queued/sec` counter), and
the worker processors 1, 3, 5, 7 and 9 spent 0.0% in DPCs and at most 0.7% in
interrupts. If a worker's processor is in the list, do not trust the other
numbers until it is not. These rows did not measure a way back from that
state. On the host below, the falling-behind 5-lane rows (v1.1.53 and v1.1.47)
followed a reset to the layout of step 3; after two more resets (to 6 lanes,
then back to step 3's layout) rows of the same builds passed, and the rows
with per-processor counters, taken after later resets to the same layout,
read clean.

What was measured in 6-lane `rio` rows of this version on one host, after
`-BaseProcessorNumber 0 -MaxProcessorNumber 10 -MaxProcessors 6
-NumberOfReceiveQueues 6` read back as set (per-processor counters in two of
the three runs): the worker processors 7 and 9 spent 21.6 to 23.1% of their
time in interrupts (about 48,000 to 50,000 a second each) and 11 about 15%. The
runs held about 685,000 to 740,000 packets a second instead of 900,000, with
about 165,000 to 225,000 message starts refused a second, from the load's ramp
to its end, three runs out of three. 5-lane rows of the same build on the same
host just before, with no interrupt time on any processor, held 900,000 a
second with 0 refused, three out of three, and 6-lane v1.1.53 rows on that host
about an hour earlier, after another reset to the same 6-lane settings, held 900,000
with 0 refused, two out of two (interrupt placement not captured). These rows
show the interrupts on the workers and the shortfall together, not that one
caused the other.

The falling-behind rows (those 6-lane rows, and 5-lane v1.1.53 rows on the same
host whose interrupt placement was not captured) showed, against passing runs:

- the counter `RIO_SEND_WAITS` (flushes that kept a lane's fill phase open
  because its next phase was not yet idle): about 9,900 by 80 s in the 6-lane
  rows and about 8,100 in the v1.1.53 rows, against 10 or fewer in passing
  v1.1.53 runs;
- the counter `MESSAGE_UNRELIABLE_WRITER_NO_CAPACITY`: about 1.6 million by
  80 s in the v1.1.53 rows (not read in the 6-lane rows), against 0;
- the lane workers' thread time (`\Thread(<server process>/*)\% Processor
  Time`): 59
  to 79% in the 6-lane rows, where interrupts took 22% of two of the workers'
  processors and that time is not the thread's, and 55 to 78% in the v1.1.53
  rows, against 85 to 96%;
- the players' input delay that the load harness measured, at 60 Hz: about 23
  to 30 ms mean and 125 to 145 ms p99, against 9 to 13 ms mean and 29 to 35 ms
  p99 for passing v1.1.53 runs and 5 to 9 ms mean and 18 to 20 ms p99 for
  passing runs of this version.

A 3 s stall of every worker, forced with a realtime-class busy loop on each
`rio` worker processor, cost those 3 s and about one more, and the server was
then back at 900,000 a second with 0 refused. A short stall recovered on its
own, so a run that stays behind for the whole load points to a cause that
persists; the per-processor check above is the first thing to read.

### IOCP at 10 Gb/s

`iocp` is the fallback where `rio` cannot start; for the peak load use `rio`.
The rule is the same as for `rio`: keep the RSS processors and the completion
workers' processors apart, and check with the per-processor list above that no
worker processor the server logs is in it.

At this version (v1.1.x), IOCP completion worker `w` sits on the first logical
processor of core `w` (processor `2w` with two a core), and its send threads
take the cores after the workers. Each worker's socket is bound to its worker's
own processor, so RSS queue `w` on that processor brings worker `w` its flows,
and the worker shares its processor with the network card's DPCs. In a release
build at this version nothing moves the worker off it, and there is no
measured RSS layout that keeps the DPCs off the completion workers. The measured `iocp` row (2 completion
workers, 2 send threads, peak load) did keep them apart, with a
development-only layout (`ROCKETNET_TEST_IOCP_LAYOUT`, a `ROCKETNET_DEV`
build): RSS on processors 2 and 4, the completion workers on 3 and 5, the send
threads on 6 and 8 and the main thread on 10, set from an elevated PowerShell
with

```powershell
Set-NetAdapterRss -Name "Ethernet 2" -BaseProcessorNumber 2 -MaxProcessorNumber 4 -MaxProcessors 2 -NumberOfReceiveQueues 2
```

It held 900,000 packets a second with 0 refused, with 54 to 57% DPC time on
processors 2 and 4. At this version, for the built-in placement, the RSS form
that follows from it, `-BaseProcessorNumber 0 -MaxProcessorNumber 2 -MaxProcessors 2
-NumberOfReceiveQueues 2`, puts the DPCs on the workers' processors and is not
a measured row.

Without a bind address, every Windows server transport binds the wildcard
address on the server's port with `SO_EXCLUSIVEADDRUSE`, so a second process
cannot share the port. An
IPv6 socket clears `IPV6_V6ONLY`, so a server created with both an IPv4 and an
IPv6 address on one port serves both families from one socket. Each transport
asks the OS for the port it was given, so a public address with port 0 works.

## Pinning a transport

Set `transport` in `rocketnet_server_config_t` or `rocketnet_client_config_t`,
or set `ROCKETNET_SERVER_TRANSPORT` or `ROCKETNET_CLIENT_TRANSPORT`. The
variable is read once, at create, and displaces the config's value. In
cmd.exe:

```bat
set ROCKETNET_SERVER_TRANSPORT=rio
```

and in PowerShell, `$env:ROCKETNET_SERVER_TRANSPORT = "rio"`. Clear it with
`set ROCKETNET_SERVER_TRANSPORT=` or `Remove-Item Env:ROCKETNET_SERVER_TRANSPORT`.

One name the endpoint has is a pin: that transport or a failed start, never
a quiet fall to another. One name it does not have is not a pin: it falls to
`auto` with a warning. A comma list such as `rio,iocp` is tried in that order. A name
Windows does not have (`gso`, `io_uring`, `xdp`, `msgx`), or `rio`
named for a client, is skipped with an info line; a list left empty by
skipping uses `auto` with a warning. The table of what each kind of unmet pin
returns is in TRANSPORTS.md, "Choosing a transport".
`bin\Release\example_transports.exe` creates a server under `auto`, under a
`portable` pin and under the list `xdp,gso,portable` (on Windows the first
two names are skipped), prints the transport each one got, and shows the
refusal of a name no build knows.

## What the host needs

- **portable:** UDP sockets. Nothing else.
- **iocp:** Winsock overlapped I/O on a UDP socket. Send and receive offload
  are used when the network stack offers them and skipped when it does not.
- **rio:** the Registered I/O extension functions, which Winsock hands out per
  socket. A host whose stack refuses them, or refuses to register a lane's
  region, refuses `rio` when the server starts with
  `rio transport create failed (Winsock <n> <name>, Win32 <n>)` (a
  development library adds `, test stage <n>`), and the next candidate in the
  list is tried. A region the allocator cannot supply, or a registration
  Windows refuses for want of memory, logs the same line but ends the start
  with `OUT_OF_MEMORY`: a failure for want of memory never falls to the next
  candidate.
- **Every transport:** the process's own memory budget, if one is set with
  `ROCKETNET_SERVER_MEMORY_BUDGET_BYTES`; a transport whose memory plan does
  not fit is refused before anything is allocated.

No transport needs an elevated process. By default `rocketnet_init` asks
Windows for a 1 ms timer period (`timeBeginPeriod( 1 )`) and `rocketnet_term`
releases it (`timeEndPeriod( 1 )`), and each network thread `iocp` and `rio`
create is set to `THREAD_PRIORITY_TIME_CRITICAL`. So is every client's own
network thread, whatever its transport, `portable` included: a game that
creates a client has one time-critical thread in its process. Call
`rocketnet_thread_priority( false )` before init to skip both if the game sets
the timer period and priorities itself (`sdk/rocketnet.h`).

## The firewall

A server binds the wildcard address by default, so the first run of a server,
of `functional.exe` or of the example server on an interactive desktop raises
the Windows Defender Firewall prompt. Loopback traffic works whatever is chosen
there. A headless server host needs an inbound rule that allows UDP on the
server's port from the networks clients connect from; the client needs no
inbound rule. Creating that rule is the host's provisioning, not the
library's: rocketnet never edits the firewall.

## Dead clients, ICMP and connection reset

Winsock reports an ICMP port unreachable, sent back after the server sends to
a client that has gone, to the server socket's next receive as
`WSAECONNRESET`. Every socket rocketnet creates on Windows turns those reports
off at create, and every receive path skips the reset status and keeps
reading, so one dead client never stalls the others; it times out like any
silent client and its slot is freed. TRANSPORTS.md, "Windows connection
reset", names the calls and the functional cases that prove it for each
transport and family.

What is cleared when a connection ends, on either side, so that a slot or a
client object can be reused, is the separate inventory in
[CONNECTION-RESET.md](CONNECTION-RESET.md).

## Limits for 1000 clients

A server's socket count depends on its transport: one for `portable`, one for
each completion worker for `iocp` (2 on a bind address by default, 1 on the
wildcard; its send threads add none) and one for each lane for `rio` (5 on a
bind address by default, 1 on the wildcard). That is a few sockets, not one a
client, so there is no descriptor limit to raise. The limit that matters is
memory: per-client state grows with `max_clients`, the send staging and receive buffers of each
transport are sized from the same configured `max_clients` (1000 by default),
and `rio` registers (and so locks) each lane's registered region when the
server starts.

Every block the library uses comes from the caller's allocator, in two steps.
`rocketnet_server_create` allocates the server's own block.
`rocketnet_server_start` then allocates the transport, before any traffic:
its sockets, workers, send and receive buffers and each `rio` lane's
registered region, and the per-client tables. Windows commits memory when it
is allocated, touched or not, so once the server has started all of it is
charged against the machine's commit limit (RAM plus the page file), though
far less of it is ever resident. `rocketnet_server_required_memory` returns
the most the allocator holds for a config over create, start, stop, restart
and destroy, plus 1 MiB of slack. This page gives no figure for it: query it
for the config you deploy. With a system-managed page file Windows may grow
the page file first; a create or start the host still cannot commit is
refused with `OUT_OF_MEMORY`, and a start refused for memory does not fall to
another transport. Size the page file, or `max_clients` and the profile, so
the `rocketnet_server_required_memory` of every server, and the
`rocketnet_client_required_memory` of every client, in the process fits.
Threads are two for
`portable`; for `iocp` the completion workers plus the send threads (4 on a
bind address by default, 3 on the wildcard); for `rio` one a lane (5 on a bind
address by default, 1 on the wildcard); each with the executable's default
stack reserve (1 MiB unless the game links with another). Each RIO lane has
its own registered region, so more lanes raise the plan: ask for the figure
of the config you deploy, bind address included. Ask for the numbers
for a given config before deploying:

- `rocketnet_server_required_memory( &config )` in code, and
- `bin\Release\transport_memory.exe 1000 1416 iocp 0 --plan-only` (or
  `rio`) for the per-transport breakdown, including the registered region,
  workers and sockets. The arguments are explained in
  [Memory and capacity](MEMORY.md#pre-allocation-server-resource-plans).

## Verify a host

Run these in order from the checkout in cmd.exe, one at a time; each exits 0.

```bat
ver
cmake --version
git log --oneline -1
cmake -B build -A x64
cmake --build build --config Release --parallel 4
bin\Release\test.exe
bin\Release\functional.exe
bin\Release\test_iocp.exe
bin\Release\test_transport.exe
bin\Release\example_transports.exe
bin\Release\transport_memory.exe 1000 1416 iocp 0 --plan-only
bin\Release\transport_memory.exe 1000 1416 rio 0 --plan-only
set ROCKETNET_CLIENT_TRANSPORT=portable
set ROCKETNET_SERVER_TRANSPORT=iocp
bin\Release\functional.exe --run test_transport_env_pin_is_honoured
set ROCKETNET_SERVER_TRANSPORT=rio
bin\Release\functional.exe --run test_transport_env_pin_is_honoured
set ROCKETNET_SERVER_TRANSPORT=
set ROCKETNET_CLIENT_TRANSPORT=
```

How to read them:

- `test.exe` is the unit suite and opens no socket; it ends with
  `*** ALL TESTS PASSED ***`.
- `functional.exe` opens real sockets on loopback and includes the Windows
  connection reset cases; with no pin set the server takes the build's `auto`.
- `test_iocp.exe` drives the `iocp` transport directly: completion lifetime,
  allocation failure and shutdown.
- `example_transports.exe` ends with `PASS`.
- Each pinned `functional.exe --run` prints
  `test_transport_env_pin_is_honoured` when the pinned transport is the one the
  server got. CI's Windows matrix runs four cases under each of
  `portable/portable`, `portable/auto`, `iocp/portable`, `portable/iocp`,
  `rio/portable` and `loopback/loopback` (`.github/workflows/ci.yml`, "Transport family matrix").
- The functional suite has timing bounds. On a loaded host rerun a case that
  failed on time on its own, with `functional.exe --run <case>`, before
  reading it as a defect.
- The development Debug functional suite (`cmake --build build --config Debug
  --parallel 4`, then `bin\Debug\functional.exe`) runs with
  `set ROCKETNET_TEST_CASE_TIME_SCALE=2`, as the nightly Linux Debug functional
  row does, and with `set ROCKETNET_TEST_CASE_TIME_SCALE=4` under
  `ROCKETNET_TEST_NIGHTLY=1`. Its `test_unit_case_memory` row runs the whole
  Debug unit suite in one case, over the 120 s case limit at scale 1 on the
  fleet Windows host; in the nightly tier it takes about 225 s, 155 s of it the
  1000-client per-client isolation case. Clear it afterwards with
  `set ROCKETNET_TEST_CASE_TIME_SCALE=`.
