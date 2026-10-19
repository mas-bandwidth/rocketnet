# RocketNet on macOS

This is the page a Mac engine programmer reads first. It covers what the
checkout supports on macOS, how to build it, which transport a client or server
gets and how to pin one, how the client's threads interact with App Nap and
QoS, what a socket error means, the file-descriptor limit, the application
firewall prompt, debugging with lldb, and an ordered way to verify a Mac.

Each paragraph names the file and line range that backs it. Where a card named
by this guide has not landed at the commit this page was written at, the
section says **pending** and makes no claim. The source of truth for the
support tiers is [PLATFORMS.md](PLATFORMS.md); this page does not widen it.

## What macOS support is

The supported row is macOS 15 on arm64 with Apple Clang, drivers `portable` and
`msgx`, proved by `.github/workflows/ci.yml` on the runner labelled
`rocketnet-macos` (`docs/PLATFORMS.md`, the table). macOS on x86_64 is an
unsupported row: no workflow builds or runs it (`docs/PLATFORMS.md`, the
`macOS | x86_64 | none` row). A nightly soak row on `macos-latest` exists
(`docs/PLATFORMS.md`; `.github/workflows/nightly-soak.yml`, the `soak-macos`
job); it is nightly, not the per-commit claim.

CI builds both Debug and Release on that runner. Release goes through `make`,
because the Makefile is the Release front door; Debug configures CMake directly
(`.github/workflows/ci.yml`, the macOS matrix rows and the Build steps).

## Build

The tools are Apple clang from the Command Line Tools, `cmake` (3.20 or later),
`make`, `xxd` and Python 3; libsodium is vendored under `sodium/` and needs no
install (`BUILDING.md`, "Building"; `CMakeLists.txt:1`, which requires CMake
3.20). CI's dependency step checks exactly `cmake clang c++ xxd python3` on
macOS (`.github/workflows/ci.yml`, "Check the fleet's build dependencies").

Release, through the front door:

```sh
make
```

That configures `build/` with `CMAKE_BUILD_TYPE=Release` and builds with a
bounded job count (`Makefile`, the `build` target). Binaries and libraries land
in `bin/` (`CMakeLists.txt:37-45`; `BUILDING.md`).

Debug, configuring CMake directly:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 4
```

Debug has no Makefile spelling; this is the command CI runs on the Debug row
(`.github/workflows/ci.yml`, "Build (Debug)"). The card brief asks for
`--parallel 4`; the CI Debug step leaves the count to CMake.

The native slice is what both commands build. Universal binaries, the Xcode
generator and a pinned deployment target are **pending**: no card at this base
builds, tests or fixes either, so this page makes no claim about them.

The macOS-only unit target is `bin/test_msgx` (`CMakeLists.txt:479-482`, built
when `APPLE`). The other targets named in the build are the unit suite
`bin/test`, the functional suite `bin/functional`, the per-driver checks
`bin/test_lifecycle` and `bin/test_driver`, and the tools `bin/soak`,
`bin/soak_sim`, `bin/load` and `bin/transport_memory` (`CMakeLists.txt:309-346`,
`425-430`, `526-528`, `542`).

## The macOS transports

The registry has two rows for macOS: `portable` on every platform and `msgx` on
macOS, with `msgx` ranked ahead of `portable` (rank 10 against 30) for both
server and client (`sdk/rocketnet_drivers.inc:5`, `:8`). So an `auto` server or
client on macOS asks for `msgx` first and falls to `portable` on refusal.

Both rows share the same server and client operations. The server path is the
buffered per-packet socket path; the client path is the eight-callback table
that runs the I/O thread (`sdk/rocketnet_driver_builtin.cpp:8-15`,
`:28-135`).

**msgx** is the Darwin batch pump over `sendmsg_x` and `recvmsg_x`
(`sdk/rocketnet_platform.cpp:1680-1690`). The two symbols are resolved by
`dlsym` once at platform init, with no link-time reference
(`sdk/rocketnet_platform.cpp:1739-1740`), so a kernel without them is not a
load-time failure. At socket create, a non-blocking socket with both symbols
present runs one real `recvmsg_x` as a probe. A result of zero or more, or
`EAGAIN`/`EWOULDBLOCK`, grants the pump; any other errno leaves that socket on
the portable per-packet loop (`sdk/rocketnet_platform.cpp:2116-2148`). The
probe is skipped for a blocking socket (`sdk/rocketnet_platform.cpp:2123`).

A refusal at runtime does not kill the socket. A weather error on the send path
(`EAGAIN`, `EWOULDBLOCK`, `ENOBUFS`, `ECONNREFUSED`, `EHOSTUNREACH`,
`ENETUNREACH`, `EMSGSIZE`, `EADDRNOTAVAIL`, `EPERM`, `EACCES`, `EHOSTDOWN`,
`ENETDOWN`) finishes the chunk per packet without pausing the pump; a refusal
that is not weather pauses the pump for 60 seconds and the next batch call
re-probes (`sdk/rocketnet_platform.cpp:2332`, `:2368-2387`, `:2490-2553`).
Because the pump is per socket, one socket can degrade while another keeps
batching.

**Portable** is the ordinary per-packet `sendto`/`recvfrom` loop; on macOS it is
what the probe falls back to (`sdk/rocketnet_platform.cpp:2160-2270`).

### Pinning a transport

Two names, one variable per endpoint:

```sh
ROCKETNET_SERVER_TRANSPORT=msgx ROCKETNET_CLIENT_TRANSPORT=msgx ./bin/functional
ROCKETNET_SERVER_TRANSPORT=portable ROCKETNET_CLIENT_TRANSPORT=portable ./bin/functional
```

The value is the config's transport field: `auto` by default, a one-element
list is a pin (that transport or loud failure, never a silent fall), and a
comma list is an ordered candidate list (`BUILDING.md`, "Server transports";
`sdk/rocketnet_config.cpp:820-940`). The legacy names `ROCKETNET_DRIVER`,
`ROCKETNET_TRANSPORT`, `ROCKETNET_SERVER_DRIVER` and
`ROCKETNET_CLIENT_DRIVER` are refused aloud and never applied
(`sdk/rocketnet_config.cpp:514-530`). A known transport that is absent, or not
applicable to that endpoint, is skipped with a log line; a list whose every
name is skipped falls back to that endpoint's auto order, loudly
(`sdk/rocketnet_config.cpp:900-935`). An unknown name refuses the create
(`sdk/rocketnet_config.cpp:850-860`).

Pinning `msgx` requires the probe to grant it. If the probe refuses, the create
fails; it does not fall to portable (`sdk/rocketnet_platform.cpp:4071-4130`).
Pinning `portable` suppresses the probe entirely, so a fixed-port bind does not
consume a queued datagram in the probe
(`sdk/rocketnet_platform.cpp:4098-4125`).

**`ROCKETNET_PUMP`** is the pre-#106 override, absorbed with its old meaning.
Only the value `portable` is meaningful: it forces the portable pump and
suppresses the probe. The successors are `ROCKETNET_SERVER_TRANSPORT=portable`
and `ROCKETNET_CLIENT_TRANSPORT=portable` (`BUILDING.md`, "Server transports";
`sdk/rocketnet_platform.cpp:2125-2130`, `:4102-4120`). A pinned `msgx` under
`ROCKETNET_PUMP=portable` refuses before a socket is bound
(`sdk/rocketnet_platform.cpp:4112-4120`).

The transport family matrix runs every pair on IPv4, IPv6 and a dual-stack
bind: `portable/portable`, `portable/msgx`, `msgx/portable`, `msgx/msgx` and
`loopback/loopback` on macOS, each cell asserting
`driver_env_pin_is_honoured` (`.github/workflows/ci.yml`, "Transport family
matrix", `TRANSPORT_MATRIX_MACOS`).

The per-endpoint config fields also have the overlay
`ROCKETNET_<TRANSPORT>_<FIELD>`, where `<TRANSPORT>` is `SERVER` or `CLIENT`;
the name is built by `rocketnet_config_env_var_name_for`
(`sdk/rocketnet_config.cpp:344-377`). For example
`ROCKETNET_CLIENT_SEND_KBPS`, `ROCKETNET_SERVER_MAX_CLIENTS`; the full field
list is in `USAGE.md`, "Environment overrides".

## Client threads, QoS and App Nap

On macOS the portable client uses the I/O-thread design: the socket and a
kqueue are created together, the kqueue holds the socket's `EVFILT_READ` and an
`EVFILT_USER` trigger, and `wait` enables or disables the read filter as the
receive buffer has room (`sdk/rocketnet_platform.cpp:2873-2986`;
`sdk/rocketnet_driver_builtin.cpp:32-112`; `ROCKETNET_PLATFORM_HAS_WAKE` is set
for Apple in `sdk/rocketnet_platform.h:471-479`). The thread is an ordinary
pthread created by the platform layer (`sdk/rocketnet_platform.cpp:2722-2750`).

The SDK sets no QoS class on that thread and no App Nap assertion. It makes no
`NSProcessInfo` or `pthread_set_qos_class_*` call; a search of `sdk/` for
`NSProcessInfo`, `App Nap`, `LSAppNapIsDisabled` and `qos_class` finds no call.
The only Mach scheduling policy the SDK sets is on the frame-sync waiting
thread, when frame sync is used: `THREAD_TIME_CONSTRAINT_POLICY` with a period,
computation and constraint derived from the frame period, restored to
`THREAD_STANDARD_POLICY` afterwards (`sdk/rocketnet_frame_sync.cpp:173-198`).
That policy is not applied to the client I/O thread.

What the game does about it:

- **App Nap.** macOS may nap a background app: timers coalesce and the app's
  threads are scheduled rarely, which shows up as late receives and resends.
  The game keeps an activity assertion while it is playing, e.g. an
  `NSProcessInfo` activity with `NSActivityUserInitiated` and
  `NSActivityLatencyCritical`, or ships `LSAppNapIsDisabled` in its
  `Info.plist`. This is application behaviour, not a RocketNet call: nothing in
  `sdk/` disables App Nap.
- **QoS.** Because the SDK leaves the I/O thread at the default class, a game
  that wants the receive thread above its utility threads sets that itself, on
  the thread, with the platform's QoS API. Do not raise the whole process: the
  library's other threads are deliberately ordinary. The frame-sync policy
  above is the only elevated policy the SDK owns.

Observe the assertions a run holds, and read App Nap state in Activity
Monitor's Energy tab:

```sh
pmset -g assertions
```

A run with no assertion of its own is a candidate for App Nap when another app
is frontmost; a run holding a user-initiated activity is not.

## Socket errors and ICMP

RocketNet's UDP sockets are unconnected and every send carries its destination
(`sdk/rocketnet_platform.cpp:500-560`). On an unconnected socket an ICMP port
unreachable or host unreachable does not surface on the next receive as
`ECONNREFUSED` or `EHOSTUNREACH`; the kernel has no connected peer to attach it
to, and the SDK never reads `SO_ERROR`. The result is ordinary UDP loss: the
packet is gone and the protocol's resend or timeout handles it. The portability
layer deliberately does not turn ICMP into a connection reset. The Windows
socket path disables `SIO_UDP_CONNRESET` for exactly that reason
(`sdk/rocketnet_platform.cpp:3431-3436`).

The send path logs a failed `sendto` at debug and continues
(`sdk/rocketnet_platform.cpp:527-533`, `:2198-2222`). The receive path treats
`EAGAIN` and `EINTR` as "drained" and returns zero; any other `recvfrom` error
is logged at debug and also returns zero, so one bad socket read never tears
down the transport (`sdk/rocketnet_platform.cpp:575-610`). A zero-length
datagram is legal UDP but carries nothing the protocol admits, so it is skipped
rather than ending the drain; `MSG_TRUNC` reports the true datagram size, and
an over-size packet is handed up for the prefilter to drop as too large
(`sdk/rocketnet_platform.cpp:596-640`).

The msgx pump applies the same rules in batch form. Weather errnos finish the
chunk per packet; a non-weather refusal pauses the pump for 60 seconds
(`sdk/rocketnet_platform.cpp:2368-2387`, `:2500-2560`, `:2590-2655`). The
client I/O thread and the server both keep running through these errors; a
dead destination costs packets, not the transport.

## File-descriptor limit

A transport owns one UDP socket. The portable and msgx drivers create exactly
one socket at create (`sdk/rocketnet_driver_builtin.cpp:8-15`), and the server
keeps one socket for the whole server regardless of `max_clients`: the
connection table is in-process memory indexed by client slot, not one socket or
one descriptor per client (`sdk/rocketnet_server_transport.cpp:1438-1452`;
`ROCKETNET_MAX_CLIENTS` is 1000 in `sdk/rocketnet.h:89`). The loopback driver
uses no socket at all (`DRIVERS.md`, "Native transport drivers"). So the
macOS file-descriptor limit is not a limit on how many clients one server
accepts.

It is a limit on how many sockets one process opens. A process that runs many
client transports (a load or soak harness, a listen server in the same
process, or a tools run with several endpoints) spends one descriptor per live
transport. Check and raise the soft limit in the shell that launches it:

```sh
ulimit -n
ulimit -n 4096
```

A launchd-launched app inherits the limit from its job; a game started from
Xcode inherits the scheme's environment. If a many-client run fails at create
with a socket error, compare `ulimit -n` with the number of live transports
before changing anything else. The server's receive backlog is a separate,
memory-backed bound, not a descriptor count
(`sdk/rocketnet_server_transport.cpp:1815`).

## The application firewall prompt

A server zeroes the host part of its public address before binding, so it binds
the wildcard on the advertised port (`sdk/rocketnet_server_transport.cpp:1086`,
`:1229-1240`; `BUILDING.md`, the Windows paragraph describes the same wildcard
bind). The example server's default public address is `127.0.0.1:40000`
(`server.c:909-910`), and its bind is still `0.0.0.0:40000`.

macOS's application firewall filters incoming connections to an app. The first
time an unsigned or ad-hoc development build binds a wildcard port and receives
a packet from another host, macOS raises the "Do you want the application to
accept incoming network connections?" prompt. What triggers it is the inbound
connection to the listening app, not the `bind` call and not the transport.
Clicking Allow adds that build to the firewall list.

- **Loopback-only tests need no exception.** Traffic to `127.0.0.1` or `::1`
  does not arrive on an external interface, so `./bin/functional` and the
  examples when dialled at their loopback address do not need the prompt
  answered. That is why CI can run the suite on an unattended runner: the
  functional cases dial loopback.
- **A server for remote players does need the exception.** Binding the
  wildcard and accepting traffic from another machine is the case the firewall
  exists for. Allow it only on the networks those players use.
- **A signed build avoids repeated prompts.** The firewall records an allowed
  app by its code identity. Rebuilding an unsigned dev binary changes that
  identity, so the next launch prompts again. A Developer ID-signed and
  notarized build with a stable bundle identifier is recorded once and does not
  re-prompt on each run.

A prompt raised during an automated run is a finding to report, not something
to click through. Keep automated runs on loopback.

## Debugging with lldb

Build Debug, then run a suite or one case under the debugger. The unit suite
takes a case name as its one positional argument (`test.cpp:164-178`); the
functional suite takes `--run <case>` (the card brief; `functional/main.cpp`).

```sh
lldb ./bin/test
(lldb) run test_client_io_handoff_sends_ring_once

lldb ./bin/functional
(lldb) run -- --run test_transport_udp_loopback
```

Set an environment pin for the debug session either in the shell before lldb,
or with lldb's own settings so a `run` keeps it:

```sh
(lldb) settings set target.env-vars ROCKETNET_CLIENT_TRANSPORT=msgx ROCKETNET_SERVER_TRANSPORT=msgx
```

Useful stops and views:

```sh
(lldb) breakpoint set -n rocketnet_client_create
(lldb) breakpoint set -n rocketnet_server_create
(lldb) breakpoint set -f rocketnet_platform.cpp -l 2141   # the msgx probe
(lldb) run
(lldb) bt
(lldb) thread list
(lldb) thread backtrace all
(lldb) frame variable
```

For a hang, attach and ask every thread, or sample the process without
stopping it:

```sh
(lldb) process attach --pid <pid>
(lldb) thread backtrace all
sample <pid> 1
```

A crash inside the message layer usually needs the transport and message
objects alive when the frame is inspected; build Debug so the frame variables
survive, and keep the failing command line in the report.

## Sanitizers and leaks

**pending.** No landed card at this base builds or runs a sanitizer or a leak
check on macOS, so this page makes no sanitizer claim and gives no command for
one. The Linux gates in `.github/workflows/ci.yml` and
`.github/workflows/nightly-leaks.yml` are Linux claims
(`docs/PLATFORMS.md`, "Workflows that do not add a platform row").

## Fuzzing

**pending.** No landed card at this base builds a libFuzzer target or runs a
macOS fuzz session, so this page makes no fuzzing claim and gives no command
for one.

## Nightlies on the fleet runner

**pending.** The nightly soak row on hosted `macos-latest` is in
`docs/PLATFORMS.md` and `.github/workflows/nightly-soak.yml`, but the card that
moves the macOS nightlies onto the fleet's own runner has not landed at this
base. Until it does, do not read a fleet-runner nightly claim into this page.

## Verify a Mac

Run these in order on the Mac, from the checkout, one at a time. Each must exit
zero; a non-empty `git status --short` means the tree is not the commit being
verified.

```sh
uname -s                  # Darwin
uname -m                  # arm64 on the supported row
sw_vers -productVersion
clang --version           # Apple clang from the Command Line Tools
cmake --version

git log --oneline -1
git status --short

make
cmake -B build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug --parallel 4

./bin/test
./bin/functional

ROCKETNET_SERVER_TRANSPORT=portable ROCKETNET_CLIENT_TRANSPORT=portable ./bin/functional
ROCKETNET_SERVER_TRANSPORT=msgx ROCKETNET_CLIENT_TRANSPORT=msgx ./bin/functional
ROCKETNET_SERVER_TRANSPORT=msgx ROCKETNET_CLIENT_TRANSPORT=portable ./bin/functional
ROCKETNET_SERVER_TRANSPORT=portable ROCKETNET_CLIENT_TRANSPORT=msgx ./bin/functional

./bin/test_msgx
./bin/test_lifecycle
./bin/test_driver

python3 tools/check_platform_policy.py
```

How to read the results:

- `./bin/test` prints each unit case and a summary; it opens no socket and
  starts no transport (`BUILDING.md`). A test over 60 seconds prints a warning;
  over 120 seconds it is killed and the suite fails.
- `./bin/functional` prints each functional case and its summary; it opens real
  sockets on loopback. Under a `msgx` pin, a red `driver_env_pin_is_honoured`
  means the pin was not the granted transport.
- The four pinned functional runs are the macOS transport matrix cells from
  `.github/workflows/ci.yml`; each cell must print
  `driver_env_pin_is_honoured` (not `[SKIPPED]`).
- `./bin/test_msgx` is the macOS-only batch-pump target
  (`CMakeLists.txt:479-482`).
- `python3 tools/check_platform_policy.py` rereads `docs/PLATFORMS.md` and
  refuses a support row whose workflow file or runner label is absent
  (`docs/PLATFORMS.md`, "Checking the table").

A command that needs a tool this Mac does not have is reported as not run and
the missing tool is named; do not mark it passed. In particular, building the
Debug tree and running `bin/test_msgx` both need the Command Line Tools, and
`make` needs `xxd`. The universal, Xcode, sanitizer, leak and fuzz checks have
no command on this page yet (see their pending sections).

Record for each run: the command, the exit status, and the suite's own summary
or the one case's result. That table, not this page, is the evidence that a
particular Mac was verified.
