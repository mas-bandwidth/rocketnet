# RocketNet on macOS

This is the page a Mac engine programmer reads first: how to build, which
transport a client or server gets and how to pin one, the client's threads and
App Nap, what socket errors and ICMP do, the file-descriptor limit, the
application firewall prompt, debugging, and the commands that verify a Mac, in
order.

Each claim names the file that backs it. A section that says **pending** makes no claim. Every command on this page is run
on an Apple-silicon Mac with the Command Line Tools and no Xcode; a command
that needs more than that says what it needs. The support tiers themselves are
in [PLATFORMS.md](PLATFORMS.md); this page does not widen them.

## What is supported

The supported row is macOS 27 on arm64 with Apple Clang and the `portable` and
`msgx` transports. Continuous integration builds and tests that configuration;
nightly coverage also includes x86_64 under Rosetta 2. The support tiers are
listed in [PLATFORMS.md](PLATFORMS.md).

## Build

The tools are Apple clang from the Command Line Tools, `cmake` 3.20 or later,
`make`, `xxd` and Python 3; libsodium is vendored (`BUILDING.md`;
`CMakeLists.txt`, `cmake_minimum_required`). CI's macOS dependency step checks
for `cmake clang c++ xxd python3` (`.github/workflows/ci.yml`, "Check the
build dependencies are present").

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
`CMAKE_LIBRARY_OUTPUT_DIRECTORY`). Before switching configurations in one
checkout, run `make clean` to remove those shared outputs; otherwise a newer
binary from another configuration can be mistaken for an up-to-date target.
Then build the selected configuration and run its tests before switching again.

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
platform; server rank 30 in development builds and 50 in production, client
rank 30; the `PORTABLE` registry entry in `sdk/rocketnet_drivers.inc`)
and `msgx` (macOS, server and client rank 10; the `MSGX` registry entry);
lower ranks win. Server and client `auto` ask for `msgx` first and fall to `portable`
if `msgx` refuses. The `loopback` row is pin-only, an
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
(`rocketnet_platform_socket_connect`, `STANDARD.md` t11: the client trusts only
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
with Apple clang (`/usr/bin/clang`, the system
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
no changed bound; a finding is a defect.

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

The live message matrix is one functional case per message mode so each keeps
the fixed 120-second case budget under ThreadSanitizer. The case suffixes are
`unreliable`, `unreliable_sequenced`, `reliable`, `reliable_ordered`,
`reliable_sequenced`, `unreliable_with_ack_receipt`,
`reliable_with_ack_receipt`, and `reliable_ordered_with_ack_receipt`, in mode
order. Together they are the former single matrix without omitted cells. Each
case runs both directions, every channel its mode permits, the complete size
schedule and edge probes, and the over-maximum refusals for its mode.

One case under `leaks`, in process:

```sh
MallocStackLogging=1 leaks --atExit -- ./bin/test test_endian
MallocStackLogging=1 ROCKETNET_TEST_SUITE_CAPTURE=1 leaks --atExit -- ./bin/functional --run test_loopback_transport_udp
```

A case that runs past 180 s under the leaks leg is killed and named red.
`test_channels_allocator_trap_witness` (`tests/test_channels.cpp`) forks a
child whose allocator trap ends in `_Exit`; `leaks --atExit` interposes that
exit in the child it is not watching, so under `leaks` that one case can hang
and is named red by the case limit. Every other case is checked for leaks.

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

Each prints `replayed <n> inputs` and exits 0.

## The functional suite

`./bin/functional` is the per-commit functional gate on the macOS rows of
`.github/workflows/ci.yml`. `ROCKETNET_TEST_NIGHTLY=1 ./bin/functional` adds
the nightly tier, selected by `rocketnet_functional_test_group` in
`functional/main.cpp` and the `SUITE_GROUP_NIGHTLY` flag in
`tests/suite_runner.h`. The nightly tier adds the portable hard-killed-client
cases, three portable/msgx 1000-client connection combinations, and the
remaining transport combinations for `test_posix_destroy_under_traffic_100`.
The ordinary suite keeps the msgx hard-killed-client cases and the msgx/msgx
1000-client connection case.

The suite runner enforces a memory bound for each case process
(`tests/suite_runner.h`). Cases using the counting allocator are limited to
their planned peak plus 64 MiB; other cases use the suite-wide 698 MiB bound
or a case-specific `SUITE_CASE_RSS_BOUND`. Set
`ROCKETNET_TEST_MAX_CASE_RSS_MB` to override the bounds, and
`ROCKETNET_TEST_REPORT=1` to print each case's peak and bound. These variables
configure the test executable, not the library.

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

make clean
cmake -B build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug --clean-first --parallel 4
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

## Nightly coverage

The macOS nightly checks add soak behavior, frame-budget measurement, fuzz
corpus replay, and example builds to the regular build and test coverage.

### The resolver

A Mac that cannot resolve `github.com` fails every checkout of a job. Check
the resolver with `scutil --dns` and `networksetup -getdnsservers Ethernet`
(the service name is the network service in use), and test it with
`dscacheutil -q host -a name github.com`. mDNSResponder takes a REFUSED answer
from the first server as an answer and does not try the second, so a router
that refuses lookups should not be in the server list.
