# RocketNet on macOS

This is the page a Mac engine programmer reads first: how to build, which
transport a client or server gets and how to pin one, the client's threads and
App Nap, what socket errors and ICMP do, the file-descriptor limit, the
application firewall prompt, debugging, and the commands that verify a Mac, in
order.

Each claim names the file that backs it. A section whose hardening work has not
landed says **pending** and makes no claim. Every command on this page is run
on an Apple-silicon Mac with the Command Line Tools and no Xcode; a command
that needs more than that says what it needs. The support tiers themselves are
in [PLATFORMS.md](PLATFORMS.md); this page does not widen them.

## What is supported

The supported row is macOS 27 on arm64 with Apple Clang, transports `portable`
and `msgx` (`docs/PLATFORMS.md`, the table). CI runs it on a self-hosted
Apple-silicon runner labelled `rocketnet-macos`, as a Debug row and a Release
row (`.github/workflows/ci.yml`, the `macos-15` entries of the
`build-and-test` matrix, whose `runner` is
`self-hosted, macOS, ARM64, rocketnet-macos`). The same runner carries the
macOS nightly rows: soak, frame budget and examples
(`docs/PLATFORMS.md`; `.github/workflows/nightly-soak.yml`,
`nightly-frame-budget.yml`, `nightly-examples.yml`). macOS on x86_64 is a
nightly row: the examples nightly builds the library and the suites universal
(arm64 and x86_64) and runs the x86_64 slice under Rosetta 2
(`docs/PLATFORMS.md`; `nightly-examples.yml`, the step "Build universal and
run the x86_64 slice under Rosetta 2"; the recipe is in `BUILDING.md`).

## Build

The tools are Apple clang from the Command Line Tools, `cmake` 3.20 or later,
`make`, `xxd` and Python 3; libsodium is vendored (`BUILDING.md`;
`CMakeLists.txt`, `cmake_minimum_required`). CI's macOS dependency step checks
for `cmake clang c++ xxd python3` (`.github/workflows/ci.yml`, "Check the
fleet's build dependencies are present").

Release, through the Makefile:

```sh
make
```

`make` configures `build/` as Release and builds with at most eight jobs
(`Makefile`, the `build` target; `make JOBS=4` lowers it). Debug configures
CMake directly, as CI's Debug row does (`.github/workflows/ci.yml`, "Build
(Debug)"):

```sh
cmake -B build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug --parallel 4
```

Both configurations write their executables and dylibs into the same `bin/`
(`CMakeLists.txt`, `CMAKE_RUNTIME_OUTPUT_DIRECTORY` and
`CMAKE_LIBRARY_OUTPUT_DIRECTORY`), so `bin/` holds whichever build ran last.

The targets a Mac programmer uses: `bin/test` (unit suite), `bin/functional`
(functional suite; it also runs `test_msgx`, `test_serialize` and
`test_transport_control` as sibling executables, `functional/main.cpp`),
`bin/test_msgx` (built only on Apple, `CMakeLists.txt`, the `if(APPLE)` block
that adds it), `bin/test_lifecycle`, `bin/test_transport`, the
`bin/example_*` programs, `bin/soak`, `bin/soak_sim`, `bin/load` and
`bin/transport_memory`.

The CMake cache sets no `CMAKE_OSX_ARCHITECTURES`, so a build is the host's
native slice. CMake sets `CMAKE_OSX_DEPLOYMENT_TARGET` to 12.0 unless the
caller sets it. Check what a build produced:

```sh
grep -E 'CMAKE_OSX_(ARCHITECTURES|DEPLOYMENT_TARGET)' build-debug/CMakeCache.txt
lipo -archs bin/librocketnet.dylib
vtool -show-build bin/test
```

On an Apple-silicon Mac `lipo` prints `arm64` and `vtool` prints `minos 12.0`.

### Universal binaries, the Xcode generator, the deployment target

See [BUILDING.md](../BUILDING.md) for the universal build, the Xcode generator
and the deployment target.

## The macOS transports

The transport registry has two rows that build on macOS: `portable` (every
platform; server rank 30 only in development builds and unavailable in
production, client rank 30 in either; the `PORTABLE` registry entry in
`sdk/rocketnet_drivers.inc`)
and `msgx` (macOS, server and client rank 10; the `MSGX` registry entry);
lower ranks win. Server `auto` asks for `msgx` first and falls to `portable`
only in a development build. In production, `msgx` is the macOS server
transport; portable remains
available to clients if `msgx` refuses. The `loopback` row is pin-only, an
in-memory transport for tests (the `LOOPBACK` registry entry). The backends
share `portable_client_operations`; their server operation tables use the same
bounded RX/TX bank implementation and select the socket pump by transport name
at create. On macOS, msgx uses the Darwin `sendmsg_x` and `recvmsg_x` pump and
portable uses the per-packet fallback. These definitions are in
`sdk/rocketnet_driver_builtin.cpp` and
`sdk/rocketnet_platform.cpp`.

**msgx** is the darwin batch pump over `sendmsg_x` and `recvmsg_x`
(`sdk/rocketnet_platform.cpp`, the block that opens "The darwin batch pump").
The two symbols are resolved with `dlsym` at platform init, with no link-time
reference, so their absence is never a load failure. At socket create a
non-blocking socket with both symbols becomes an `msgx` candidate; create reads
nothing. The macOS `recvmsg_x` rejects `MSG_PEEK` with `EINVAL`, so create
cannot peek at queued traffic. The first real batch call is the functional
probe: a `recvmsg_x` there delivers what it reads, so a fixed-port bind that
already has a datagram queued keeps it. A result of zero or more, or
`EAGAIN`/`EWOULDBLOCK`, keeps the pump;
any other errno pauses it and that same call finishes on the portable
per-packet loop, losing nothing (`rocketnet_msgx_pause`, the receive block in
`rocketnet_platform_socket_receive_packets`). The functional case
`test_posix_platform_send_seam` queues one datagram on the bound socket between
bind and that first batch call, checks that create left it queued, and requires
the batch call to deliver it with the pump still `msgx`.

At runtime a send errno that is ordinary UDP weather (`EAGAIN`, `EWOULDBLOCK`,
`ENOBUFS`, `ECONNREFUSED`, `EHOSTUNREACH`, `ENETUNREACH`, `EMSGSIZE`,
`EADDRNOTAVAIL`, `EPERM`, `EACCES`, `EHOSTDOWN`, `ENETDOWN`) finishes that
chunk per packet and does not pause the pump; any other refusal pauses the
batch pump on that socket for 60 seconds, and the first batch call after the
pause is the re-probe (`rocketnet_msgx_send_weather`, `rocketnet_msgx_pause`,
`ROCKETNET_MSGX_RETRY_SECONDS`). The pump is per socket, so one socket can be
paused while another batches. `rocketnet_platform_socket_pump` reports the live
pump, so a paused socket reads as `portable`.

**portable** uses per-packet `sendto` / `recvmsg` calls
(`rocketnet_platform_socket_send_packet` and
`rocketnet_platform_socket_receive_packet` in the macOS section of
`sdk/rocketnet_platform.cpp`). Both macOS server transports run one transmit
worker and one receive worker. The caller copies borrowed, sealed send frames
into one of three owned TX banks outside the lock; a try-lock publishes bank
indices to the transmit worker. The receive worker reads datagrams into three
owned RX banks and publishes them to the caller, which copies their packets and
performs shared protocol work. The workers use the selected msgx or portable
socket pump; the client path is separate.

**The msgx client** is the portable client table under the `msgx` name: the
`MSGX` registry row's client operations are `portable_client_operations`
(`rocketnet_msgx_transport` in `sdk/rocketnet_driver_builtin.cpp`), so the two
client pins differ only in the name the client create hands the socket create
(`portable_io_client_create` -> `portable_socket_create` ->
`rocketnet_platform_socket_create_with_pump`). A `msgx` pin requires the
`sendmsg_x`/`recvmsg_x` pump and refuses the create without it; a `portable`
pin forces the per-packet loop. The client I/O thread (docs/CLIENT-IO.md)
sends and receives through the table's `send_batch` and `receive_batch`, which
on macOS call `rocketnet_platform_socket_send_packets` and
`rocketnet_platform_socket_receive_packets`, the pump seam above, so a `msgx`
client batches on its I/O thread and never on the game thread. Client `auto`
on macOS is `msgx` (client rank 10, above portable's 30). A runtime refusal
pauses that client's pump exactly as on a server socket: the batch in flight
finishes per packet and nothing is lost. The unit cases in
`tests/test_unit_client_msgx.cpp` pin the shared table, its I/O callbacks and
the client ranking without a socket; the functional cases
`test_msgx_client_pin_selects_batch_pump`,
`test_portable_client_pin_selects_portable_pump`,
`test_msgx_client_degrades_without_loss` and
`test_msgx_client_transport_wire_ipv4`, `_ipv6` and `_dual_stack` (against the
`msgx` and the `portable` server) read the pump off the client's live socket
and carry payloads both ways.

### Pinning a transport

The client and server configs each carry a `transport` string, with an
environment overlay: `ROCKETNET_CLIENT_TRANSPORT` and
`ROCKETNET_SERVER_TRANSPORT` (`sdk/rocketnet.h`, `rocketnet_client_config_t`
and `rocketnet_server_config_t`). `NULL` is `auto`; one name is a pin (that
transport or a refused create, never a silent fall); a comma list is an
ordered candidate list (`BUILDING.md`, "Server transports"). An unknown name
refuses the create and the log lists the valid names. A name the registry
knows but macOS does not build (`gso`, `xdp`, `iocp`, `io_uring`, `rio`) is
skipped with `transport <name> is not available here. skipped`, and a list
left with no transport for the endpoint falls to `auto` with a warning
(`sdk/rocketnet_config.cpp`, `rocketnet_transport_ladder_parse`):

```sh
ROCKETNET_SERVER_TRANSPORT=gso ./bin/example_transports
```

logs `transport list "gso" names no transport for this endpoint on this platform. using auto (msgx,portable)` and `auto chose msgx`. The variable
displaces whatever `transport` the program set (each create logs `server config override ROCKETNET_SERVER_TRANSPORT=gso (displaces <old>)`), so the
example's own impossible pin is displaced too, its last check fails, and it
exits 1. The functional
case `test_transport_env_pin_is_honoured` fails under such a pin for the same reason.

Pinning `msgx` requires the batch symbols on the host: without them the create
refuses instead of falling to `portable`. The first real batch call then
decides the live pump; if the kernel refuses it there, the pinned transport
pauses `msgx` and runs `portable` rather than failing after create. Pinning
`portable` skips the batch decision entirely, so a fixed-port bind never has a
queued datagram read before the caller does
(`rocketnet_platform_socket_create_with_pump`).

`./bin/example_transports` prints the transport chosen by the automatic ladder
and verifies an explicit portable pin.

Every other config field has an overlay named
`ROCKETNET_<ENDPOINT>_<FIELD>` with `<ENDPOINT>` `CLIENT` or `SERVER`, built by
`rocketnet_config_env_var_name_for` (`sdk/rocketnet_config.cpp`); the comment
beside each field in `sdk/rocketnet.h` names its variable.

The test suites clear every config overlay before they
run, and keep only `ROCKETNET_SERVER_TRANSPORT` and
`ROCKETNET_CLIENT_TRANSPORT` (`sdk/rocketnet_tests.cpp`,
`rocketnet_test_environment_t`).

CI pins every macOS pair on IPv4, IPv6 and dual stack: `portable/portable`,
`portable/msgx`, `msgx/portable`, `msgx/msgx` and `loopback/loopback`, each
running `test_loopback_transport_udp`, `test_loopback_transport_ipv6`,
`test_transport_dual_stack_ipv4_client` and `test_transport_env_pin_is_honoured`
(`.github/workflows/ci.yml`, "Transport family matrix",
`TRANSPORT_MATRIX_MACOS`). The commands are in "Verify a Mac" below.

## The client's threads, QoS and App Nap

On macOS the platform layer builds the kqueue wake (`sdk/rocketnet_platform.h`,
`ROCKETNET_PLATFORM_HAS_WAKE`), so a socket client runs a client I/O thread
(`sdk/rocketnet_client_transport.cpp`): a kqueue holds the socket's `EVFILT_READ` and
an `EVFILT_USER` trigger (`sdk/rocketnet_platform.cpp`, the kqueue wake). The
thread is a plain `pthread_create` with default attributes
(`rocketnet_platform_thread_create`). Right after the create, the creating
thread makes one call on it: `pthread_setschedparam( thread, SCHED_RR, { sched_get_priority_min( SCHED_RR ) } )`
(`rocketnet_platform_client_thread_priority` in `sdk/rocketnet_platform.cpp`,
through `rocketnet_platform_network_thread_priority`).
That is the only thing the library sets on the macOS client network thread: no QoS
class (`thread_function_shim` in `sdk/rocketnet_platform.cpp` calls the thread
function and nothing else), no `THREAD_TIME_CONSTRAINT_POLICY`, no pinning, and
no App Nap call (a search of `sdk/` for `qos_class`, `NSProcessInfo` and
`LSAppNapIsDisabled` finds nothing). `rocketnet_thread_priority( false )`
turns the call off (`docs/CLIENT-IO.md`, Thread priority), and with it off the
library makes no scheduling call on any thread. Apart from it, the only
scheduling policy the SDK sets is the Mach time-constraint policy on the thread
that waits in frame sync, put back to the standard policy afterwards
(`rocketnet_frame_sync_elevate_priority` and
`rocketnet_frame_sync_restore_priority` in `sdk/rocketnet_frame_sync.cpp`);
`rocketnet_frame_sync_elevate_priority` returns before that call when
`rocketnet_platform_thread_priority_enabled` is false.

Each msgx or portable server has two socket workers, one for
receive and one for transmit. They use default `pthread_create` attributes and
do not receive the client thread's `SCHED_RR` priority request.

The deterministic cases in `test_unit_thread_priority.cpp` verify the client
and server priority requests, the off switch, and refusal counting and logging
through a fake platform call table. The functional scheduler readback and
App Nap timing cases have been retired: OS scheduling and process throttling
are outside RocketNet's control.

A game that wants the client's I/O on a thread it owns sets
`ROCKETNET_CLIENT_STEP_DRIVEN=1`: the
client then runs the I/O step inside `rocketnet_client_update` and
`rocketnet_client_send_packets` on the caller's thread and creates no I/O
thread (`sdk/rocketnet_client_transport.cpp`, where the thread is created only when
neither step mode is set). That thread's QoS is then the game's.

```sh
ROCKETNET_CLIENT_STEP_DRIVEN=1 ./bin/example_transports
```

It logs `client config override ROCKETNET_CLIENT_STEP_DRIVEN=1 (displaces 0)`
and exits 0.

App Nap is a property of the game process, not of the library: nothing in
`sdk/` holds a power or activity assertion. List the assertions a running game
holds with:

```sh
pmset -g assertions
```

How the client's timing behaves under App Nap and under a lowered QoS, and what
the library does about it: **pending.**

## Socket errors and ICMP

A client's socket is connected to its server
(`rocketnet_platform_socket_connect`, TRANSPORT.md t11: the client trusts only
the server), so the kernel drops datagrams from any other source, its sends
name no destination, and an ICMP error for the server reaches its receive,
which skips it as weather. A server socket is never connected, and every send
names its destination. A failed `sendto`
is logged at debug level and the packet is dropped
(`rocketnet_platform_socket_send_packet`). A failed `recvmsg` returns 0, which
ends that drain; an error other than `EAGAIN` or `EINTR` is logged at debug
level, and the next update reads again. A zero-length datagram is skipped
rather than read as "drained", and a datagram longer than the caller's slot
keeps its true length so the transport drops it as oversize
(`rocketnet_platform_socket_receive_packet`). The msgx pump applies the
weather and pause rules above to the same errnos.

`test_platform_batch_pump_refused_destination` is the run: a batch with a
refused destination in the middle delivers every other packet
(`functional/rocketnet_functional_tests.cpp`).

```sh
./bin/functional --run test_platform_batch_pump_refused_destination
```

What a receive does after a client is hard-killed and the kernel answers with
ICMP: **pending.**

## The file-descriptor limit

A socket transport owns one UDP socket: the memory plan for `portable` and for
`msgx` is one socket (`sdk/rocketnet_driver_builtin.cpp`,
`rocketnet_builtin_server_memory_plan`). A server keeps that one socket for all
of its clients (up to `ROCKETNET_MAX_CLIENTS`, 1000, `sdk/rocketnet.h`), so
the descriptor limit is not a limit on how many clients one server accepts. It
does bound how many client transports one process holds open: each live client
is one descriptor. A tool or test that runs many clients in one process checks
the limits first:

```sh
ulimit -n
launchctl limit maxfiles
sysctl kern.maxfilesperproc
```

`ulimit -n <n>` raises the soft limit for that shell and its children, up to
the hard limit and `kern.maxfilesperproc`. An app launched from Finder or the
Dock gets the launchd limit that `launchctl limit maxfiles` prints. Many
clients in one process on macOS, measured: **pending.**

## The application firewall prompt

A server binds the wildcard address on its port, whatever host its public
address names: the bind address is the public address with its host part
zeroed (`sdk/rocketnet_server_transport.cpp`, `bind_address`). With public
address `127.0.0.1:0` the log reads `server transport started on 0.0.0.0:<port>` (`./bin/example_transports`). A client binds the wildcard of
its family. The log reads `client bound to <address> [<transport>]`.

When the macOS application firewall is on, it asks "Do you want the
application to accept incoming network connections?" the first time an app it
has no rule for opens a socket that can receive from other machines; a
wildcard bind is such a socket. So a dev build of a game server, the examples
and the functional suite can each raise the prompt on a Mac with the firewall
on, even when every client is on loopback. The trigger is the bind, not the
transport: `msgx` and `portable` bind the same way.

- **Loopback-only tests need no exception.** The firewall filters traffic
  arriving from other machines; traffic to `127.0.0.1` and `::1` does not
  arrive that way. A run whose clients and server are all on loopback needs no rule, so the
  prompt can be denied. Do not add a rule for a test binary.
- **A server for players on other machines needs the exception**, for the
  game binary that hosts it.
- **A signed build avoids repeated prompts.** The firewall keys its rule to the
  app's code signature. The linker gives every build an ad-hoc signature that
  changes with the binary, so each rebuild is a new app to the firewall. A
  build signed with a stable identity (an Apple Development or Developer ID
  certificate, through `codesign` or the project's signing settings) keeps
  one rule across rebuilds.

```sh
/usr/libexec/ApplicationFirewall/socketfilterfw --getglobalstate
codesign -dv bin/functional
```

The first prints whether the firewall is on. The second prints
`Signature=adhoc` and `flags=0x20002(adhoc,linker-signed)` for a build from
this checkout. These commands were run on a Mac with the firewall off, so the
prompt and the loopback behaviour above are described from the firewall's
documented behaviour, not observed in a run.

## Debugging with lldb

`lldb` ships with the Command Line Tools. Build Debug (above), then load a
binary and set breakpoints; this works with no further setup:

```sh
lldb --batch -o "target create ./bin/example_transports" \
  -o "breakpoint set -n rocketnet_server_create" \
  -o "breakpoint set -n rocketnet_client_create"
lldb --batch -o "target create ./bin/functional" \
  -o "breakpoint set -n rocketnet_platform_socket_create_with_pump" \
  -o "image lookup -n rocketnet_platform_socket_pump"
```

`lldb --batch` reads standard input after its commands, so from a script or an
ssh session give it `< /dev/null`. Each `breakpoint set` resolves to a source
line. A breakpoint on `rocketnet_platform_socket_create_with_pump` stops at
every socket create, where the pump request is visible, and
`rocketnet_platform_socket_pump` reports the live pump of a socket.

Running or attaching under `lldb` needs developer tools access: Developer Mode
on (`DevToolsSecurity -status` prints its state; enabling it needs an
administrator), or a logged-in desktop session that answers the authorization
prompt. Without it, `run` waits on that prompt; over ssh nobody can answer it.
With access, the unit and functional suites each run one case in-process under
the debugger, through `--run <case>` (`tests/suite_runner.h`, `suite_main`):

```sh
lldb -- ./bin/functional --run test_loopback_transport_udp
lldb -- ./bin/test --run test_protocol_id_zero_refused
```

The suites otherwise run each case in a child process, so a breakpoint in a
whole-suite run is not hit. Inside `lldb`, `settings set target.env-vars ROCKETNET_CLIENT_TRANSPORT=msgx` pins a transport for that run.

## Sanitizers and leaks

`.github/workflows/nightly-macos-sanitizers.yml` runs nightly and on dispatch
on the `rocketnet-macos` runner with Apple clang (`/usr/bin/clang`, the system
toolchain), in three legs, each in its own job and its own build directory:

- **asan+ubsan**: Debug with `-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer`, the flags of ci.yml's
  Linux sanitizers job. Apple silicon AddressSanitizer has no `detect_leaks`,
  so `ASAN_OPTIONS=detect_leaks=0` and the leaks leg is the leak check.
- **tsan**: Debug with `-fsanitize=thread`,
  `TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1`.
- **leaks**: an unsanitized Debug build. The suites run each case in a child
  process (`tests/suite_runner.h`), and `leaks --atExit` inspects only the
  process it launched and does not pass its exit status through. So the suite
  first runs plainly with `MallocStackLogging=1` (its exit status is the suite
  verdict), then every case it named runs alone, in process, under
  `leaks --atExit --`, and each exec binary of the functional suite runs under
  it too. A case past 180 s is killed and named red. Red on any leak.

Each leg runs `./bin/test` and `./bin/functional` with
`ROCKETNET_SERVER_TRANSPORT` and `ROCKETNET_CLIENT_TRANSPORT` both `msgx`, then
both `portable`: twelve runs a night. No suppression file, no skipped case and
no changed bound; a finding is a defect with its own card.

One leg by hand, the tsan leg here (`asan+ubsan` takes the flags above,
`leaks` none):

```sh
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_FLAGS="-fsanitize=thread -fno-omit-frame-pointer" \
  -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer"
cmake --build build-tsan --target test functional test_serialize test_transport_control test_msgx --parallel 4
TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1 \
  ROCKETNET_SERVER_TRANSPORT=msgx ROCKETNET_CLIENT_TRANSPORT=msgx ./bin/functional
```

One case under `leaks`, in process:

```sh
MallocStackLogging=1 leaks --atExit -- ./bin/test test_endian
MallocStackLogging=1 ROCKETNET_TEST_SUITE_CAPTURE=1 leaks --atExit -- ./bin/functional --run test_loopback_transport_udp
```

The twelve runs by hand, Apple clang 21.0.0, macOS
27.2, two runs at a time (a unit run beside a functional run) next to the
CI runner. The step timeouts are about twice these times.

| leg | suite | pin | exit | wall |
|---|---|---|---|---|
| asan+ubsan | unit | msgx | 1 (finding 1) | 6 s |
| asan+ubsan | unit | portable | 1 (finding 1) | 5 s |
| asan+ubsan | functional | msgx | 0 | 661 s |
| asan+ubsan | functional | portable | 0 | 657 s |
| tsan | unit | msgx | 0 | 109 s |
| tsan | unit | portable | 0 | 114 s |
| tsan | functional | msgx | 1 (finding 2) | 33 s |
| tsan | functional | portable | 0 | 793 s |
| leaks | unit | msgx | 1 (finding 3), 954 of 955 cases 0 leaks | 1972 s (the hung case held one of four slots about ten minutes) |
| leaks | unit | portable | 1 (finding 3), 954 of 955 cases 0 leaks | 1472 s |
| leaks | functional | msgx | 0, 163 cases and the three exec binaries 0 leaks | 2103 s |
| leaks | functional | portable | 1 (findings 4 and 2, in the plain suite run, before the per-case pass) | 556 s; rerun alone 25 s |

Findings at 010c816:

1. **asan+ubsan, unit suite, both pins** (the unit suite clears the pins, so
   the transport does not matter). UndefinedBehaviorSanitizer, `load of value -2147483648, which is not a valid value for type 'enum rocketnet_status_t'`;
   stack top `rocketnet_status_name` at `sdk/rocketnet.cpp:210`, called from
   `test_status_name_out_of_range_is_unknown` at `tests/test_unit_status.cpp:94`
   (the `INT_MIN` probe). Apple clang's `-fsanitize=undefined` includes
   `enum`; gcc's, which the Linux jobs use, does not. The suite stops at its
   first red case, so every unit case was also run alone under this leg: the
   other 954 are clean.
2. **tsan, functional suite, msgx**. No ThreadSanitizer report:
   `check failed: ( rocketnet_counters_refusals_moved == 0 )` with
   `SEND_RING_FULL = 1556`, in
   `rocketnet_test_transport_acked_unfragmented_was_in_exactly_one_batch` at
   `functional/rocketnet_functional_tests.cpp:6503`. The case paces its sends
   with a 0.5 ms sleep and expects the msgx send ring never to fill; under
   TSan's slowdown it fills. The suite stops there, so the msgx cases after it
   have not run under TSan yet. The same leg on portable is green, but the
   leaks leg's plain run (unsanitized Debug, `MallocStackLogging=1`) on
   portable, rerun alone on the member, fails the same check in the same case:
   the case is load-sensitive on macOS, not a data race.
3. **leaks, unit suite, both pins**. `test_channels_allocator_trap_witness`
   (`tests/test_channels.cpp:1613`) hangs under `leaks --atExit`: its forked
   child's allocator trap ends in `_Exit` (`tests/test_channels.cpp:807`),
   which `libLeaksAtExit.dylib` interposes (`my__exit` then `__kill`), stopping
   a child that `leaks` is not watching, so the parent's `waitpid` never
   returns. This lies in the system tool, outside the product; the case keeps
   running and the leg's 180 s case limit names it red. Every other unit case
   reports 0 leaks.
4. **leaks, functional suite, portable**, the plain run, once:
   `check failed: ( unplanned disconnect )` in `live_matrix_window` at
   `functional/live_matrix.cpp:971` (`client_reason=-3`, the server still
   connected), in the case after `test_loopback_transport_live_message_matrix`.
   Two Release builds and a unit run were going on the member beside it; the
   rerun alone stopped earlier, at finding 2, so it is not yet reproduced.

## Fuzzing

The fuzz corpora replay on macOS without libFuzzer, as CI does on its macOS
rows (`.github/workflows/ci.yml`, "Replay the fuzz corpora without
libFuzzer"); `make` builds the four replay binaries:

```sh
./bin/fuzz_replay_rocketnet_fuzz fuzz/corpus/*
./bin/fuzz_replay_rocketnet_fuzz_message fuzz/corpus_message/*
./bin/fuzz_replay_rocketnet_fuzz_connect_token fuzz/corpus_connect_token/*
./bin/fuzz_replay_rocketnet_fuzz_receive_structured fuzz/corpus_receive_structured/*
```

Each prints `replayed <n> inputs` and exits 0. Running libFuzzer on macOS:
**pending.**

## The functional suite: per-commit and nightly tiers, the memory bound

`./bin/functional` (Release) is the per-commit gate on the macOS rows of
`.github/workflows/ci.yml`. `ROCKETNET_TEST_NIGHTLY=1 ./bin/functional` adds
the nightly tier, which `.github/workflows/nightly.yml` runs on the same
darwin runner (job "macos / Release functional nightly tier"). The tier is
chosen in `functional/main.cpp` (`rocketnet_functional_test_group`, the
`SUITE_GROUP_NIGHTLY` flag of `tests/suite_runner.h`); no case checks less in
either tier:

- `test_posix_hard_killed_client_portable_*` (four cells): each waits out
  the real 10 s token timeout. The four `msgx` server cells stay per commit.
- `test_posix_connect_disconnect_1000_{portable_portable,portable_msgx,msgx_portable}`:
  `msgx_msgx` stays per commit.
- `test_posix_destroy_under_traffic_100` runs its `msgx`/`msgx` pair per
  commit and all four pairs in the nightly tier.

Two causes made the suite slow, and both are fixed rather than moved: the
bundled sodium wiped memory with its volatile byte loop on Darwin until
`CMakeLists.txt` defined `HAVE_MEMSET_S` (server create/destroy x1000 went
from 73.7 s to 23.7 s), and the 8-slot server of
`test_posix_connect_disconnect_1000` kept 64 used tokens, so every 64th
connect waited about 4.5 s for one to expire; its server has 128 slots.

Measured on a Mac (Apple M5 Pro, Darwin 27.2, Release, host load 3 to 7
as found), one run each:

| run | main 830c5437 | this tree |
|---|---|---|
| `./bin/functional` | 202.7 s, 204 s | 49 s |
| `ROCKETNET_TEST_NIGHTLY=1 ./bin/functional` | (no tier) | 71 s |
| `test_posix_connect_disconnect_1000_msgx_msgx` alone | 76.5 s, 15 connects of about 4.5 s | 5.6 s to 10.2 s, none over 1 s |
| `test_posix_server_create_destroy_1000` | 73.7 s | 23.7 s |
| `test_posix_destroy_under_traffic_100` | 35.7 s | 6.3 s (msgx/msgx), 24.7 s (nightly, four pairs) |

At d7095705 (this tree merged with main 7a025861, which since #538 runs every
functional case serially), five `./bin/functional` runs, one
after another, each started once the host's 1-minute load was under 4 (other
jobs still share the host, so load rose during runs):

| run | exit | wall time | load before (1, 5, 15 min) |
|---|---|---|---|
| 1 | 0 | 167.6 s | 2.30 5.89 8.07 |
| 2 | 0 | 170.0 s | 3.80 5.98 7.40 |
| 3 | 0 | 169.0 s | 3.69 5.87 6.82 |
| 4 | 0 | 172.0 s | 3.61 5.95 6.82 |
| 5 | 0 | 171.3 s | 3.92 5.78 6.59 |

5 of 5 pass; the slowest is 172.0 s, not under 60 s: the 49 s above was the
parallel suite, before main made it serial. An earlier run started at load 9
(not counted) failed `test_posix_connect_disconnect_1000_msgx_msgx` at
`functional/posix_lifecycle_stress.cpp:1108`, the 5 s
`posix_wait_server_empty` after a disconnect.

Linux at d7095705 (x86_64, gcc, Release, load 3.8 as found):
`./bin/test` exit 0 (16 s), `./bin/functional` exit 0 (193 s). gcc and
clang, `ROCKETNET_DEV=ON` and `OFF`, build with 0 warnings on Linux; Apple
clang `ON` and `OFF` with 0 warnings on macOS.

Each case process is held to its own memory plan (`tests/plan_rss.h`): the
functional worker gives `rocketnet_init` a counting allocator, and a case
that passes with it still installed reports its plan's peak plus a 64 MiB
slack, which the runner holds its peak resident set to. A case that installs
its own allocator (the channel harness cases, `test_lifecycle_transport`)
and the sibling executables keep the suite-wide 698 MiB bound or the bound
they declare with `SUITE_CASE_RSS_BOUND`; `ROCKETNET_TEST_MAX_CASE_RSS_MB`
still lifts every bound. `ROCKETNET_TEST_REPORT=1` prints each case's peak
and bound. The largest peak over a plan's peak was 7 MiB on macOS and 5 MiB
on Linux x86_64.

## Verify a Mac

Run these in order from the checkout, one at a time; each exits 0. Builds and
suites never run two at once.

```sh
uname -s                  # Darwin
uname -m                  # arm64
sw_vers -productVersion
clang --version           # Apple clang
cmake --version
git log --oneline -1

make JOBS=4
./bin/test
./bin/functional
ROCKETNET_SERVER_TRANSPORT=portable ROCKETNET_CLIENT_TRANSPORT=portable ./bin/functional
./bin/test_msgx
./bin/test_lifecycle
./bin/test_transport
./bin/example_transports

for pair in portable:portable portable:msgx msgx:portable msgx:msgx loopback:loopback; do
  for name in test_loopback_transport_udp test_loopback_transport_ipv6 \
              test_transport_dual_stack_ipv4_client test_transport_env_pin_is_honoured; do
    ROCKETNET_SERVER_TRANSPORT=${pair%%:*} ROCKETNET_CLIENT_TRANSPORT=${pair##*:} \
      ./bin/functional --run $name > /dev/null || echo "FAILED $pair $name"
  done
done

cmake -B build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug --parallel 4
./bin/test
```

How to read them:

- `./bin/test` prints each unit case's name, then `*** ALL TESTS PASSED ***`;
  it opens no socket. A case over 120 seconds is killed and the suite fails
  (`tests/suite_runner.h`; Debug scales the limit).
- `./bin/functional` opens real sockets on loopback. With no pin set
  `test_transport_env_pin_is_honoured` is not in the suite; under the portable pin
  it prints `test_transport_env_pin_is_honoured`, which is what CI greps for
  (`.github/workflows/ci.yml`, "Run functional suite (portable rung pinned by
  env)").
- The loop is CI's macOS transport matrix, case by case; it prints nothing
  when every cell passes.
- `./bin/functional` under a pin other than `portable` runs cases CI does not
  run under that pin; the four matrix cases are the per-pin claim.
- The functional suite has timing bounds. On a heavily loaded Mac a timed case
  such as `test_loopback_transport_live_message_matrix` (10 second bound) can fail on time;
  rerun that one case with `./bin/functional --run <case>` before reading it
  as a defect, and report it either way.

## Nightly jobs on the macOS member

Every macOS nightly runs on the fleet's one darwin runner, one job at a time. Run a job by hand from a checkout on that
machine, one command at a time, and record the wall time of each step.

| Job | Workflow and line | By hand | Last green, or the card that owns its red |
|---|---|---|---|
| `soak-macos` | `.github/workflows/nightly-soak.yml:216` | `make && ./bin/test && ROCKETNET_SOAK_SIM_AUDIT=1 ./bin/soak_sim 100000 && ./bin/soak_sim 10000000 && ./bin/soak 200000` | run 39928961016 (main at 19eaa61d, 2026-10-23). Red in run 40080339910 (221e3bee) at `./bin/test`: `check failed: test_server_stats_current_every_update peak_rss_mb=1164 bound_mb=698`; no card owns it yet (below) |
| `macos-frame-budget` | `.github/workflows/nightly-frame-budget.yml:56` | `cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --target client_main_thread_cost --parallel 4 && python3 tools/check_frame_budget.py --bin ./bin/client_main_thread_cost --transport msgx --report-only` | run 40066869774 (84c3c50b, 2026-10-24) |
| `fuzz-macos` | `.github/workflows/fuzz-nightly.yml:185` | the job's steps: LLVM clang with libFuzzer, `cmake -B build-fuzz` instrumented, one target per leg | run 40074127124 (221e3bee, 2026-10-24), every target |
| `coverage-macos` | `.github/workflows/fuzz-nightly.yml:544` | `CC=clang CXX=clang++ tools/fuzz_coverage.sh` | run 40074127124 (221e3bee, 2026-10-24) |
| `examples-macos` | `.github/workflows/nightly-examples.yml:71` | `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --config Release --parallel 4 && python3 tools/check_examples.py && python3 tools/run_examples.py --build build`, then the universal build under Rosetta 2 | no green run recorded; red in run 40070921798 at "Check the examples against the house rules" |
| `macos-transport-cells-repeat` | `.github/workflows/nightly.yml:237` | `make -j4 && scripts/transport-cells-repeat --runs 50` | run 40079431071 (221e3bee, 2026-10-24) |
| `macos-functional-nightly` | `.github/workflows/nightly.yml` | `ROCKETNET_TEST_NIGHTLY=1 ./bin/functional` | not landed |
| `sanitizers-macos` | `.github/workflows/nightly-macos-sanitizers.yml` | that workflow's ASan+UBSan, TSan and leaks legs | not landed |

By hand at 5c2aa60f (main 1a2f2c43 plus this section), Release, 2026-10-24:

- frame budget: configure 1 s, build 2 s, report 4 s:
  `frame_budget transport=msgx P=32 total_us_p99_median=8.79 runs=8.67,9.58,8.79 gate=100 verdict=PASS`.
  The first two tries printed `VOID load=6.68 cores=18` and `VOID load=4.56 cores=18`: the
  script voids any run whose 1-minute load is above cores / 4, and other jobs shared the machine.
- soak: `make -j4` 26 s; `./bin/test` 8 s, exit 1 on
  `check failed: test_server_stats_current_every_update peak_rss_mb=1164 bound_mb=698`
  (the same case alone, `./bin/test --run test_server_stats_current_every_update`, exits 0);
  wire audit 100000 14 s, 0 failures, 0 hangs; simulator soak 10000000 554 s, 0 failures,
  0 hangs; UDP soak 200000 312 s, exit 0. The `SEND_RING_FULL` refusal seen at d6dbd0a8
  did not recur.

The `./bin/test` red is macOS only: the same head passes `./bin/test` on Linux. The bound is
the suite-wide one, and the case passes when run alone, so the measured peak is the suite's
(how `wait4` reports `ru_maxrss` for a case run inside the full suite on macOS), not the case's
plan. The bound is not loosened here.

### The runner's resolver

The frame budget red of run 39906750528 (2026-10-23 05:37 UTC, `Could not resolve host: github.com` on every checkout try) was the LAN resolver, not the
workflow. The Ethernet service had its DNS servers set by hand to
`192.168.1.1` (the router) then `1.1.1.1`. From 03:33 UTC the router answered
`github.com IN A` with REFUSED (rcode 5). mDNSResponder takes REFUSED as an
answer and does not try the second server, and it held the negative answer
in its cache: between 05:03:09 and 06:15:24 UTC it sent no query upstream and
answered 2825 lookups of `github.com` with no address and none with one. The
first good answer came at 06:15:24 UTC. The machine did not sleep: `pmset -g`
shows `sleep 0` and `standby 0`, and `pmset -g log` has no Sleep or Wake entry since
the boot at 2026-10-22 05:24 UTC (01:24 EDT); around 01:37 EDT on 2026-10-23 it has only
assertion entries.

The fix, made on 2026-10-24 at 12:35 UTC: the router is out of the Ethernet
service's server list, `networksetup -setdnsservers Ethernet 1.1.1.1 1.0.0.1`.
`scutil --dns` resolver #1 now lists `1.1.1.1` then `1.0.0.1` (before:
`192.168.1.1` then `1.1.1.1`), and `dscacheutil -q host -a name github.com`
answers an address. Check a Mac's resolver the same way, with `scutil --dns`
and `networksetup -getdnsservers Ethernet`.
