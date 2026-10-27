# Building rocketnet

Production libraries use an explicit source list. The SDK test suite is linked
only into `test` through `rocketnet_test_support`. Shared builds hide unannotated
implementation symbols; `tools/exports.txt` lists the supported C exports. CI
compares the built shared library with that list and checks that the static
archive contains no test suite. Review intentional ABI changes in the list as
part of the same change. The major version names SDK calling/layout
compatibility, separately from the wire version; see the ABI rules below.

Deploying a server on Linux: [docs/LINUX.md](docs/LINUX.md) covers the build, the production `xdp -> gso -> io_uring -> portable` transport ladder and pinning, what `gso`, `io_uring` and `xdp` need from the host, socket errors and ICMP, the fd and memlock limits for 1000 clients, containers, and how to verify a host. Both builds offer portable as the final Linux fallback.

rocketnet builds on Linux and macOS with CMake 3.20 or later, and on Windows with
CMake 4.2 or later (the Windows block below says why). On Linux the build is
tested with gcc 13.3.0 and 15.2.0 and with clang 18.1.3 and 21.1.8, in Debug and
Release: with each compiler the static and shared libraries, `./bin/test` and
`./bin/functional` build, and the unit suite passes. Functional results are
recorded per compiler and configuration: the matrix also exposes outstanding
receive-weather counter failures and an intermittent io_uring lifecycle
failure; an isolated rerun is not a full-suite pass. The `linux / clang` job in
`.github/workflows/nightly.yml` builds and runs both suites with clang; the
Linux build-and-test rows in `.github/workflows/ci.yml` build the library and
both suites with the default compiler (gcc), and ci.yml uses clang only for the
fuzz-smoke legs and the embedded XDP program. On Linux and macOS the Makefile is
the front door:

    make

The CMake build treats warnings as errors by default. GCC and clang use
`-Wall -Wextra -Wpedantic` plus the default-on strict set (`-Wshadow`,
`-Wconversion`, `-Wsign-conversion`, `-Wcast-qual`, `-Wformat=2`, `-Wundef` and
`-Wdouble-promotion`); clang-cl uses that strict set beside `/W4`, and MSVC uses
`/W4`. `ROCKETNET_WARNINGS_AS_ERRORS` defaults to `ON` (`-Werror` or `/WX`). If
a newer compiler introduces a warning the current checked toolchains do not
emit, configure with `-DROCKETNET_WARNINGS_AS_ERRORS=OFF` to keep every warning
enabled while temporarily dropping only the error policy. Set
`-DROCKETNET_STRICT_WARNINGS=OFF` only when the embedding build cannot accept
the strict set itself. These directory-scoped options apply to RocketNet's own
targets and do not propagate through the installed package or namespaced
targets into a consuming project.

Binaries and libraries land in `bin/`. On macOS, [docs/MACOS.md](docs/MACOS.md)
is the guide: the build, the two transports and how to pin them, the client's
threads and App Nap, socket errors, the file-descriptor limit, the application
firewall prompt, lldb, fuzz corpus replay, and the commands that verify a Mac.
Run the unit suite with:

    ./bin/test

The repository text audits that used to be unit cases run in the static
source-audit binary `./bin/source_audit` instead: the client/server
"says messages, not packets" scan
(`test_client_server_say_messages_not_packets`), the environment-name
documentation check (`test_config_env_every_name_read_is_documented`), the
counter-named-by-a-test check (`test_counters_every_counter_is_named_by_a_test`),
the one-family-per-outcome check
(`test_counters_every_outcome_in_at_most_one_family`) and the XDP counter-row
check (`test_xdp_transport_host_every_counter_has_a_case`). It reads
source, header and doc files through `ROCKETNET_SOURCE_DIR`, opens no socket
and starts no thread; CI runs it on a Linux leg beside
`tools/check_client_server_words.py`.

`./bin/test` does not open a socket and does not start a transport. It drives the
state machines and the in-memory network simulator, and reads and writes no
file: the known-answer fixtures it checks (`tests/connect_token_vectors.json`
and `tests/wycheproof/*_test.json`) are compiled into it at build time by
`cmake/EmbedTestFixtures.cmake`, so editing one rebuilds it. The vectors file is
regenerated only by the tool `./bin/connect_token_vectors <path>`
(tools/connect_token_vectors/main.cpp), which writes the path it is given. Tests run in parallel
(at most eight, or `ROCKETNET_TEST_JOBS` when that is smaller). In a Release
build a test over 120 seconds is killed and the suite fails. Debug and
sanitizer builds scale the limit by 5. A passing run prints each test's
name and then `*** ALL TESTS PASSED ***`; `ROCKETNET_TEST_REPORT=1` adds the
ten slowest cases and the ten largest peak resident sets after the results
(tests/README.md). Peak resident set is telemetry in this unit run: an
over-bound line names the measured peak and bound without changing the unit
verdict.

A test that calls `socket`, `bind`, `sendto`, `recvfrom`, `select`, `epoll`,
IOCP, or AF_XDP, or that starts a real transport (`rocketnet_server_create`,
`rocketnet_client_create`, `rocketnet_platform_socket_create`, or a
sibling binary that does), belongs in the functional suite:

    ./bin/functional

Each functional case targets completion within **60 seconds**, with a hard
**120-second maximum**, including Debug, sanitizer and emulated builds. Cases
should stop as soon as their correctness checks finish; they do not wait out
that budget. The live message matrix reports elapsed time against both bounds,
while preserving delivery, ordering, receipt and cleanup checks. This budget
is separate from the library's main-call latency/performance qualification.
`ROCKETNET_TEST_REPORT=1` reports the suite's slowest cases. Every functional
case, including sibling binaries, runs alone on every platform. Setting
`ROCKETNET_TEST_JOBS` cannot increase functional concurrency; deterministic
unit cases may still run in parallel. Run separate functional invocations and
other network workloads separately on the same test host as well.

The functional suite enforces peak-resident-set bounds. Its
`test_unit_case_memory` sibling row runs the complete unit binary in enforcement
mode, so the bound declarations beside unit cases and the suite-wide fallback
are reused directly. The row and every unit case inside it retain the
existing time limits.

Functional assertions check delivery, ordering, state transitions and eventual
recovery. Wall-clock latency and receive gaps are diagnostics, not pass/fail
performance thresholds. Generous deadlines catch missing progress or hangs.
Exact protocol timeout boundaries belong in deterministic tests with simulated
time. Real pacing primitives may check that they wait at all, without imposing
an upper latency threshold. Performance qualification is a separate workload.

The functional suite is its own target and prints results in the same format.
Put a new test in `./bin/test` when the assertion can be reached through a
state machine or the simulator. Put it in `./bin/functional`
when the assertion is about a real socket or a real transport. A test that
cannot run without a socket is functional, and it keeps its assertions.
The functional-only sibling binaries are `test_serialize` (a real clock and
`rocketnet_sleep`), `test_xdp_filter` (a kernel, root or CAP_BPF, and
`usleep`), `test_transport`, `test_transport_control`, `test_lifecycle`,
`test_production_client_lifecycle` (DEV=0 only), `test_msgx` and `test_iocp`;
they are functional tests, never `./bin/test`.
Where the same rule is already covered by a simulator or state-machine test,
that unit test stays beside it. The connect path's unit twin is
`test_client_server_connect` (and the simulator loss walks). Fragment and
ACK behavior already has protocol tests in the unit suite. The keyless-config
create is functional; `test_server_keyless_config_has_no_keys` checks the
config the gate reads, without creating a transport.

The functional suite takes its loopback ports from the OS. Every loopback
server and socket it opens binds port 0 and reads back the port the OS gave,
except the restart case, which reserves a port below the kernel's ephemeral
range so a sibling's port-0 bind cannot take it between rebinds. An address
that must carry a real port takes a free one from the OS through
`rocketnet_test_free_port`, which binds a scratch socket to
`127.0.0.1:0`, reads the port, and closes it; its callers hand that port to
an xdp create the commit barrier refuses before any bind. Two
`./bin/functional` runs may share a machine at the same time. What holds then:

- The lifecycle cases read their after counts with `posix_settled_counts`,
  which polls until the counts equal the before counts or 10 seconds pass, so
  a joined thread the kernel has not yet reaped is not counted as a leak, and
  a thread, descriptor or umem really left still fails the case.
- `test_posix_server_create_destroy_1000` runs 250 cycles on the gso and io_uring
  cells, 100 on the xdp cell and 1000 on the others, so two suites at once stay under the 120
  second case limit; the counts are compared after the first cycle and the
  last, so a leak per cycle still fails.
- `test_posix_server_restart_same_port_100` reserves a port below the kernel's
  ephemeral range in a slice its cell alone scans and binds it again 100
  times, so a sibling suite's port-0 binds cannot take it between rebinds.
- A server create on a free loopback port is retried up to three times on
  address-in-use, so a sibling process that takes the granted port in the gap
  between the free-port proof and the bind does not fail the create.
- The root-only XDP veth case (`ROCKETNET_TEST_XDP_VETH`) makes a fixed
  namespace and veth pair and binds port 40411 on it; it skips without root.

Two `tools/run_examples.py` runs may also share a machine: each example
pair runs in its own temporary directory on its own free port, the client
starts only after the server has written its keys, and a server that cannot
bind before then is retried on a new port, at most three tries
(tools/run_examples.py).

Many-client load runs use the dedicated load harness:

    ./bin/load <clients> <seconds> [drop-one]

The load tool runs the public API (`rocketnet_server_t` and `rocketnet_client_t`
from `rocketnet.h`) under the light profile (10 Hz updates, one message per client
per update in each direction: the client-to-server message rotates through all
eight reliability modes, and the server-to-client message is reliable-ordered),
verifying loopback scale up to 1000 concurrent clients across five pass criteria
(`connect_within_10s`, `no_unexpected_disconnect`, `reliable_delivery`,
`unreliable_integrity`, and `counters_agree`).

The soak test runs the public API under load until stopped, or for a bound:

    ./bin/soak --seconds 60
    ./bin/soak 10000

The examples build into `bin/` with everything else. Check and run them from
the repository root ([examples/README.md](examples/README.md) lists them):

    python3 tools/check_examples.py
    python3 tools/run_examples.py --build build

### Test binaries and their tiers

Every test binary is in exactly one tier. A unit binary opens no socket,
starts no thread, never sleeps and reads time only as an argument (the test
clock starts at 1000.0). A binary that opens a real socket, starts a thread,
sleeps or reads the wall clock is functional, and CI runs it only in a step
named functional, never as a unit step.

Unit cases feed SDK environment parsing through the in-memory source compiled
only into `rocketnet_test_hooks`; they never read or mutate the process
environment. `functional/environment_wiring.cpp` sets and restores real
variables and pins the SDK wiring, precedence, and refusal paths.

| Binary | Tier | Why | Where it runs |
| --- | --- | --- | --- |
| `test` | unit | state machines and the in-memory simulator | CI `Run unit tests` |
| `test_wide` | unit | `test.cpp` built with 512 channels (the `test_wide` target in CMakeLists.txt) | by hand |
| `test_sodium` | unit | known-answer vectors | CI `Sodium known answers` |
| `fuzz_replay_*` | unit | corpus replay, no init (the `fuzz_replay_${harness}` targets in CMakeLists.txt) | CI `Replay the fuzz corpora without libFuzzer` |
| `functional` | functional | real sockets, transports and shared-library exports | CI `Run functional suite` |
| `test_serialize` | functional | sleep-wait loops (test_serialize.cpp) | `./bin/functional` (the `execs` tables in `main`, functional/main.cpp) |
| `test_transport_control` | functional | creates a server transport on a loopback socket and reads `rocketnet_time()` (test_transport_control.cpp) | `./bin/functional` |
| `test_xdp` | functional | the AF_XDP transport | `./bin/functional` on Linux |
| `test_xdp_filter` | functional | loads the XDP program with BPF_PROG_TEST_RUN; root | `nightly-soak.yml` (`tools/run_xdp_filter_guest.sh`) and by hand |
| `test_msgx` | functional | a loopback socket and sleep-wait loops (test_msgx.cpp) | `./bin/functional` on macOS |
| `test_iocp` | functional | loopback sockets, threads and sleep-wait loops (test_iocp.cpp) | `functional.exe` on Windows |
| `test_lifecycle` | functional | threads, `time(NULL)` tokens, and real server and client transports (test_lifecycle.cpp) | CI `Functional: lifecycle binary` |
| `test_production_client_lifecycle` | functional | DEV=0 public client/server create, connect, update, disconnect and destroy over portable worker threads | production (`ROCKETNET_DEV=OFF`) gates |
| `test_transport` | functional | netlink route reads, and the portable transport on loopback sockets with sleep-wait loops (`test_driver.cpp`) | CI `Functional: native driver contract and external backend` |
| `test_server_stats_creates_no_thread` | functional | reads the process thread count from `/proc/self/task` and checks a loopback server create/update adds none (process_thread_counts.cpp) | `./bin/functional` |
| `test_public_step_driven_client_connects_without_a_thread` | functional | reads the process thread count from `/proc/self/task` and checks a step-driven loopback client create adds none (process_thread_counts.cpp) | `./bin/functional` |

The shared-library export cases are functional because they load the built
libraries through the OS loader: `test_api_polish_dev_token_string`,
`test_dev_connect_token_timeout_is_the_named_constant`, and
`test_dev_token_minting_absent_in_release`. They run from `./bin/functional`,
not `./bin/test`.

The channel cases in `./bin/functional` (`functional/channel_*.cpp`) run on
`rocketnet_channel_harness_t` (functional/channel_harness.h): the in-memory
simulator and the test clock, with no socket and no sleep.

The corpus-consistency cases in `./bin/functional`
(functional/corpus_consistency.cpp: `test_pcapng_corpus_replay`,
`test_pcapng_corpus_witness_unread_packet_goes_red`,
`test_pcapng_corpus_covers_control_packet_types` and
`test_fuzz_prefilter_seed_names_match_outcomes`) read `tests/pcapng/`,
`tests/pcapng/regress/` and `fuzz/corpus/` at run time, directories that grow
as findings land, so they are functional cases, never unit ones.

The two process thread-count cases live in `functional/process_thread_counts.cpp`
and count the process's threads from `/proc/self/task`, which Linux has and
the other platforms do not: off Linux the count reads -1 and the case pins
nothing, so neither case is unit logic.

Dependencies, per platform, as CI installs them:

    # Linux -- the build links the XDP dataplane, liburing is the bench
    # io_uring server transport (without it that transport is absent); clang with the BPF target compiles the XDP
    # program at build time (the server host needs no compiler)
    sudo apt install cmake build-essential clang libbpf-dev libxdp-dev liburing-dev xxd

    # Ubuntu 22.04: AF_XDP is in stock libbpf 0.5; libxdp-dev is not available
    sudo apt install cmake build-essential clang libbpf-dev liburing-dev xxd

    # macOS
    brew install cmake

The macOS default build is the host's native slice. `CMakeLists.txt` sets
`CMAKE_OSX_DEPLOYMENT_TARGET` to 12.0 when the caller does not: the Xcode 27
build system refuses 11.0 (its supported range starts at 12.0) while the
Command Line Tools accept it, so 12.0 is the lowest target both toolchains
build for both slices of a universal library. Override it with `-D` for
another minimum.
To build a universal (arm64 and x86_64) library and the suites:

    cmake -B build-universal -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" -DCMAKE_BUILD_TYPE=Release
    cmake --build build-universal --clean-first --parallel 4
    lipo -info build-universal/librocketnet.a bin/librocketnet.dylib

Both `lipo -info` lines name `x86_64 arm64`. The libraries and executables of
every build tree land in the checkout's `bin/` (`CMakeLists.txt`), so `--clean-first` relinks
them against the universal objects when a native build ran first; without it
CMake sees the newer native files and skips the link. To read the deployment
target on
each slice:

    otool -l -arch arm64 bin/librocketnet.dylib | grep -A4 LC_BUILD_VERSION
    otool -l -arch x86_64 bin/librocketnet.dylib | grep -A4 LC_BUILD_VERSION

Both print `minos 12.0`; the static archive carries the same load command in
each member, read after `lipo -thin <arch>` and `ar -x`. CMake compiles each
architecture in its own clang pass, so the vendored sodium builds its x86 asm
into the x86_64 slice and its ARM path into the arm64 slice; the XDP archive
is embedded once with the host `xxd` and the generated header is
architecture-neutral. The `-fcf-protection` hardening option is x86-only, so
the compiler-flag check (`CMakeLists.txt`) drops it for a fat compile while the stack protector
and the other Release flags stay on both slices.

Against a universal build, `tools/check_package.py build-universal` passes the
build's `CMAKE_OSX_ARCHITECTURES` to its consumers (`tools/check_package.py`)
and `tools/check_dll_consumer.py --build build-universal` adds one `-arch` per
library slice (`tools/check_dll_consumer.py`); each refuses a consumer
executable whose `lipo -archs` lacks a slice (`tools/check_package.py`,
`tools/check_dll_consumer.py`).

The Xcode generator needs Xcode installed, and its license accepted once by an
administrator (`sudo xcodebuild -license`); until then `xcodebuild` refuses and
the configure step stops with "No CMAKE_C_COMPILER could be found". The
Command Line Tools alone have no `xcodebuild`. `DEVELOPER_DIR` selects Xcode
for one command without changing `xcode-select`:

    export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
    cmake -B build-xcode -G Xcode
    cmake --build build-xcode --config Release --parallel 4
    ./bin/Release/test

`CMakeLists.txt` sets the output directories to `bin/` with no
configuration in the path, and a multi-config generator such as Xcode appends
the configuration to them, so its executables and libraries land in
`bin/Release/`. The unit suite opens the development library by its absolute
build path (`CMakeLists.txt`), and the release library by
`bin/librocketnet.dylib`, then by the bare name
(`sdk/rocketnet_tests.cpp`), which the executable's run path
resolves in `bin/Release/` (`CMakeLists.txt`).

Windows has no Makefile path; CMake is driven directly, and the binaries land
under `bin\<config>\`. It needs, installed once:

- Visual Studio 18 (Visual Studio 2026) with the **Desktop development with C++**
  workload, which carries the MSVC x64 toolchain and the Windows SDK.
- CMake 4.2 or later. 4.2 is the first CMake with the `Visual Studio 18 2026`
  generator (CMake 4.2 release notes,
  https://cmake.org/cmake/help/latest/release/4.2.html: "The Visual Studio 18
  2026 generator was added"), and an older CMake does not generate for Visual
  Studio 18.
- Python 3 for `tools/*.py`, from python.org, which also installs the `py`
  launcher. On a fresh Windows a bare `python` or `python3` is the Microsoft
  Store stub: it prints `Python was not found; run without arguments to install from the Microsoft Store, ...` and exits 9009. `py -3` runs the installed Python.

libsodium is vendored under `sodium/`; nothing else is installed. From the
checkout, in PowerShell or cmd.exe:

    cmake -B build -A x64
    cmake --build build --config Release --parallel 4
    bin\Release\test.exe
    bin\Release\functional.exe
    py -3 tools\check_xdp_archive.py --committed-only

`bin\Release\test.exe` is the unit suite and `bin\Release\functional.exe` the
loopback suite; `bin\Release\test.exe <name>` runs one case. PowerShell also
takes `./bin/Release/test.exe`; cmd.exe does not (`'.' is not recognized as an internal or external command`). Neither PowerShell nor cmd.exe takes bash's `\`
line continuation, so on Windows write each command on one line.

Configure with `-DROCKETNET_SANITIZE_ADDRESS=ON` (default `OFF`) to build every
target with AddressSanitizer: `/fsanitize=address` and `/Zi` with
`/INCREMENTAL:NO` at link under MSVC, and `-fsanitize=address` elsewhere. On
Windows (`cmake -B build -A x64 -DROCKETNET_SANITIZE_ADDRESS=ON`, then
`cmake --build build --config Release --parallel 4`), the build copies the MSVC
AddressSanitizer runtime (beside `cl.exe`) into `bin\<config>\` so
`bin\Release\test.exe`, `bin\Release\test_lifecycle.exe`,
`bin\Release\test_iocp.exe` and `bin\Release\functional.exe` run directly.
The Debug configuration is built with the same command and a different
configuration name (`cmake --build build --config Debug --parallel 4`, then
`bin\Debug\test.exe` and `bin\Debug\functional.exe`). Under Visual Studio 18
(cl 19.51.36260) both configurations load the same
`clang_rt.asan_dynamic-x86_64.dll`: a Debug target is compiled against the debug
CRT (`/MDd`; `dumpbin /directives build\Debug\rocketnet.lib` names `MSVCRTD`
and `VCASAND`) and `dumpbin /dependents bin\Debug\test.exe` names
`ucrtbased.dll` beside the one AddressSanitizer runtime. Every target must be
compiled against the same CRT, or the executables refuse to start; those two
commands are where to look when one does. With clang-cl
(`cmake -B build -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_BUILD_TYPE=MinSizeRel -DROCKETNET_SANITIZE_ADDRESS=ON`)
the build links clang's own dynamic runtime and copies its DLL beside the executables in `bin\`.

`py -3 tools\windows\appverif_run.py -- bin\Debug\test_iocp.exe` runs one
executable under the Application Verifier Basics layers (Handles, Heaps, Locks,
Memory, TLS, Threadpool and Leak) with every stop logged and no debugger
attached (a stop ends the run once it is logged), exports the verifier log of
every process it starts with that image name (the suite runners start one child
per case), and exits non-zero naming each stop's layer, stop code and top frames; it disables
verification for the executable again on every exit path. Verifier settings and logs are keyed by
image name across the whole machine, so it runs a copy of the executable beside it under a name of
its own (`test_iocp_appverif_<pid>.exe`), which keeps another `test_iocp.exe` on the host (a CI
leg, another build) out of the run's logs, and removes the copy afterwards. It needs Application Verifier for x64 from
the Windows SDK and an elevated prompt, and runs a Debug build so the frames
carry symbols; arguments after the executable and the environment
(`ROCKETNET_SERVER_TRANSPORT` and the rest) pass through to it, and `--xml <file>` keeps the exported logs.

The examples are an echo server and a client that connects to it with a
development token. Run each in its own window:

    bin\Release\server.exe
    bin\Release\client.exe

The server's public address, the one the client dials, is `127.0.0.1:40000`;
either takes another address as its argument
(`bin\Release\server.exe 192.0.2.10:40000`). The client logs its change
to the `connected` state once the handshake completes, then counts the echoed
payloads. Ctrl+C stops either. For a first loopback try from your own project,
see the development-token note under "Installing and embedding the SDK" below.

Every rocketnet server binds the wildcard address (`0.0.0.0:40000` for the
example server), so on an interactive desktop the first run of `server.exe`,
`functional.exe` or a game server raises the Windows Defender Firewall dialog.
Loopback traffic works whatever is chosen there; allow access only on the
networks remote clients connect from.

The build compiles `server_xdp/server_xdp.c` to `server_xdp.o` with a clang that
has the BPF target. It embeds the bytes in the library. The server host needs no
compiler. AF_XDP comes from libxdp on newer systems or from libbpf 0.5
(`bpf/xsk.h`) on Ubuntu 22.04. Without such a clang or an AF_XDP provider, or with
`-DROCKETNET_XDP=OFF`, the build still succeeds with the XDP transport compiled
out: the library carries no XDP code and links neither library, and a
transport list naming `xdp` refuses with `ROCKETNET_STATUS_TRANSPORT_NOT_BUILT`. The generated header lives in each build tree, so concurrent
configurations do not rewrite the checkout. macOS and Windows do not build the
object.

### Building Linux without XDP

XDP remains enabled by default. Set `-DROCKETNET_XDP=OFF` to build without
BPF/libxdp development headers or libraries, BPF clang, or `xxd`:

    cmake -S . -B build-no-xdp -DROCKETNET_XDP=OFF -DCMAKE_BUILD_TYPE=Release
    cmake --build build-no-xdp --parallel 4

The `xdp` rung reports that it was disabled at build time and refuses create
with `ROCKETNET_STATUS_TRANSPORT_NOT_BUILT`. A pin of `xdp` fails; automatic selection continues with `gso`, `io_uring`
(when built), then `portable` in either build. The installed CMake package also omits the
BPF/libxdp dependencies. Native `test_xdp` and `test_xdp_filter` targets are
not built; the host-C filter and portable frame-codec unit tests still run.
Configure with `-DROCKETNET_XDP=ON` (the default) to restore the native
backend and its dependencies. With it on, a configure that finds no clang with
the BPF target, or no libbpf and AF_XDP development files, is not an error: it
says why and builds exactly as `-DROCKETNET_XDP=OFF` does
(`tools/check_xdp_compiled_out.py --absent` checks the built library).

### Building a Linux client without the server libraries

liburing, libbpf and libxdp are server-only: the io_uring and XDP server
transports link them, and no client code does. A Linux client build therefore
needs none of their headers or libraries. Configure with both
`-DROCKETNET_XDP=OFF` and `-DROCKETNET_IO_URING=OFF` and build the two
libraries:

    cmake -S . -B build-client -DCMAKE_BUILD_TYPE=Release -DROCKETNET_XDP=OFF -DROCKETNET_IO_URING=OFF
    cmake --build build-client --target rocketnet rocketnet_shared --parallel 4
    cmake --install build-client --prefix /your/sdk/prefix

The shared library then carries no `liburing`, `libbpf` or `libxdp` link
(`readelf -d` shows no such `NEEDED` entry), a transport list naming `xdp`
refuses with `ROCKETNET_STATUS_TRANSPORT_NOT_BUILT` and one naming `io_uring`
refuses at create, and the installed CMake package requires none of the
three: `find_package(RocketNet)`
succeeds on a machine without their development headers, and a game links
`RocketNet::rocketnet` or `RocketNet::rocketnet_shared` as usual. CI builds
exactly this configuration on Linux and runs a C client against both
installed libraries through it (`tools/check_package.py --client`).

## Pinned sodium

The default build uses the committed `sodium/sodium.c` and `sodium/sodium.h`
pair. It needs no crypto download, code generator or system libsodium.
`sodium/README.md` records provenance, licenses, exact regeneration and parity
checks. The fixed-vector test is `bin/test_sodium`.

For integration testing against an independently supplied libsodium, configure
with `-DROCKETNET_USE_SYSTEM_SODIUM=ON`. That opt-in uses pkg-config on POSIX,
or `ROCKETNET_SODIUM_INCLUDE_DIR` and `ROCKETNET_SODIUM_LIBRARY` on Windows,
and configure fails unless that libsodium is 1.0.20 or later
(`cmake/RocketNetDependencies.cmake`).
A Windows caller must select a library for its configuration and CRT. Packages
built in this mode retain that external dependency. The default vendor is built
with the same compiler and Debug/Release runtime as RocketNet.

## Production builds

`ROCKETNET_DEV` (in `sdk/rocketnet.h`) is 1 by default: every build is a
development build unless it says otherwise. A development library accepts the
published development keys and test packet filter, mints development connect
tokens, and carries the network simulator and the unit test seams. A
production build sets `ROCKETNET_DEV` to 0, one setting:

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DROCKETNET_DEV=OFF
    make ROCKETNET_DEV=OFF                      # the Makefile front door

or `#define ROCKETNET_DEV 0` (`-DROCKETNET_DEV=0`) when compiling the sources
into your own build. The CMake option sets the value on `rocketnet` and
`rocketnet_shared`, and the targets hand it to every consumer, so the header a
game compiles matches its library. `rocketnet_test_hooks` is built only when
`ROCKETNET_DEV=ON`; in a production build (`-DROCKETNET_DEV=OFF`) no hook
library is built, `./bin/test` links `rocketnet` directly to run the
production-compatible unit suites, and enabling `ROCKETNET_TEST_SEAMS` or any
`*_TEST_HOOKS` / `*_TESTS` macro is refused at both CMake configure time and
C/C++ compile time ([docs/TEST-SCAFFOLDING.md](docs/TEST-SCAFFOLDING.md)).
`rocketnet_dev` and `rocketnet_shared_dev` are `ROCKETNET_DEV=1` in every
build when `ROCKETNET_BUILD_DEV_LIBRARIES=ON`, for the examples, the functional
suite and the tools.

A production library contains no network simulator, no test seam, no
deterministic connect-token fixture writer, no reset-inventory walker, no
`rocketnet_server_transport_dev_config`, no development token minting and no
development server secret key (`tools/check_exports.py` and
`tools/check_test_scaffolding_boundary.py` check the headers, build tree,
static archive, shared library and Windows import library). It refuses the development values: the published test
packet filter at client and server create
(`ROCKETNET_STATUS_TEST_PACKET_FILTER_IN_PRODUCTION`), the published
development keys at server start (`ROCKETNET_STATUS_DEV_KEYS_IN_PRODUCTION`),
and a connect token sealed to the development server key at client connect
(`ROCKETNET_STATUS_DEV_CONNECT_TOKEN_IN_PRODUCTION`). Keys, the packet filter
and every other security setting still come from the config and its
environment variables (`ROCKETNET_SERVER_PACKET_FILTER`,
`ROCKETNET_CLIENT_PACKET_FILTER` and the rest), set at deploy time.

## Installing and embedding the SDK

Build and install both production libraries without examples, tests or tools:

    cmake -S . -B build-sdk -DCMAKE_BUILD_TYPE=Release -DROCKETNET_DEV=OFF \
      -DROCKETNET_BUILD_TESTS=OFF -DROCKETNET_BUILD_EXAMPLES=OFF -DROCKETNET_BUILD_TOOLS=OFF
    cmake --build build-sdk --parallel
    cmake --install build-sdk --prefix /your/sdk/prefix

On Windows, write each command on one line, and pass `--config Release` to the
build and the install. The Visual Studio generator is multi-config: it ignores
`CMAKE_BUILD_TYPE`, a build without `--config` makes Debug, and the install then
fails with `file INSTALL cannot find ".../build-sdk/Release/rocketnet.lib"`. In
PowerShell or cmd.exe:

    cmake -S . -B build-sdk -A x64 -DROCKETNET_DEV=OFF -DROCKETNET_BUILD_TESTS=OFF -DROCKETNET_BUILD_EXAMPLES=OFF -DROCKETNET_BUILD_TOOLS=OFF
    cmake --build build-sdk --config Release --parallel 4
    cmake --install build-sdk --config Release --prefix C:\your\sdk\prefix

An independent CMake project (enable CXX for the static library's C++ runtime)
can then use:

```cmake
find_package(RocketNet 1.0 CONFIG REQUIRED)
target_link_libraries(game PRIVATE RocketNet::rocketnet)        # static
# Or: target_link_libraries(game PRIVATE RocketNet::rocketnet_shared)
```

When linking the static archive (`librocketnet.a`) directly in a C project
without CMake, link against the C++ standard library runtime (`-lstdc++` on
Linux, `-lc++` on macOS) and the required platform libraries (on Linux:
`-lbpf -lxdp -luring -lstdc++ -lpthread -lm`; on macOS: `-lc++ -lpthread -lm`).

Supply the install prefix through `CMAKE_PREFIX_PATH`. The package resolves
Threads and the Linux bpf/xdp dependencies on the consuming machine. Sodium
is compiled into each SDK library with private symbol names; no sodium header,
DLL or package is required by consumers. Dependency metadata contains no
producer paths. Shared consumers inherit the Windows DLL import definition.
The package accepts a request for any earlier or equal version of the same major
version (`COMPATIBILITY SameMajorVersion`), so `find_package(RocketNet 1.0)`
finds every later 1.x patch. The imported targets
hand a consumer no floating-point option and no memory-budget definition: the
only compile definitions they carry are `ROCKETNET_DEV`, the value the library
was built with, and `ROCKETNET_SHARED` on `RocketNet::rocketnet_shared`. `tools/check_package.py` checks both.
The Unix shared-library soname follows the major version.
Use a separate install prefix for each build configuration. With Visual Studio
or another multi-config generator, pass `--config Debug` or `--config Release`
to build/install and select the same configuration in the consumer. A
Debug-only prefix should be consumed with `-DCMAKE_CONFIGURATION_TYPES=Debug`,
so toolchains do not try to generate other configurations requiring uninstalled
Release artifacts. Installing another configuration into the same prefix
overwrites the libraries; side-by-side Debug/Release installation is not supported.

`add_subdirectory(path/to/rocketnet)` exposes the same two namespaced targets when
shared libraries are enabled. Examples, tests and tools default off when embedded,
and the parent's `BUILD_SHARED_LIBS` does not change the static target. Explicit
`ROCKETNET_BUILD_*` options override those defaults.

By default, a top-level or embedded configure builds all four library variants:
production static (`rocketnet` / `RocketNet::rocketnet`), production shared
(`rocketnet_shared` / `RocketNet::rocketnet_shared`), build-tree development
static (`rocketnet_dev`), and build-tree development shared
(`rocketnet_shared_dev`). SDK consumers that only link a subset of these variants
can omit the unused shared and/or build-tree development libraries at configure
time:

| `-DROCKETNET_BUILD_SHARED` | `-DROCKETNET_BUILD_DEV_LIBRARIES` | Build-tree targets | Installed / exported targets |
|---|---|---|---|
| `ON` (default) | `ON` (default) | `rocketnet`, `rocketnet_shared`, `rocketnet_dev`, `rocketnet_shared_dev` | `RocketNet::rocketnet`, `RocketNet::rocketnet_shared` |
| `ON` | `OFF` | `rocketnet`, `rocketnet_shared` | `RocketNet::rocketnet`, `RocketNet::rocketnet_shared` |
| `OFF` | `ON` | `rocketnet`, `rocketnet_dev` | `RocketNet::rocketnet` |
| `OFF` | `OFF` | `rocketnet` | `RocketNet::rocketnet` |

When `-DROCKETNET_BUILD_SHARED=OFF` is set, only `RocketNet::rocketnet` is
exported and installed (`find_package(RocketNet CONFIG REQUIRED COMPONENTS static)`
succeeds, while requesting component `shared` or `rocketnet_shared` fails at
configure time). Because tests link both shared and development targets, and
examples and tools link `rocketnet_dev`, CMake rejects incompatible combinations
at configure time: `-DROCKETNET_BUILD_TESTS=ON` requires both
`-DROCKETNET_BUILD_SHARED=ON` and `-DROCKETNET_BUILD_DEV_LIBRARIES=ON`, while
`-DROCKETNET_BUILD_EXAMPLES=ON` and `-DROCKETNET_BUILD_TOOLS=ON` require
`-DROCKETNET_BUILD_DEV_LIBRARIES=ON`.

An SDK installed from a production build (`-DROCKETNET_DEV=OFF`) has no
development token minting by design. `rocketnet_create_dev_connect_token` and
`ROCKETNET_DEV_BACKEND_PUBLIC_KEY` are declared only with
`ROCKETNET_DEV=1`; defining that macro while linking a production
library does not provide the development function. For a first loopback try, embed the
checkout and link both the server and the client to the development library,
`rocketnet_dev`, which supplies `ROCKETNET_DEV=1`, creates development
token strings, and accepts the published development backend public key.
Initialize a server config with `rocketnet_server_default_config`, set a
nonzero protocol ID and the key from `ROCKETNET_DEV_BACKEND_PUBLIC_KEY`, and
start the server. Its own keypair is generated at create. Read its public key
and served addresses to create a development token; pass the resulting
opaque string directly to `rocketnet_client_connect`. It is a build-tree aid, never installed and never shipped:

```cmake
add_subdirectory(path/to/rocketnet rocketnet)
target_link_libraries(game_server PRIVATE rocketnet_dev)
target_link_libraries(game PRIVATE rocketnet_dev)
```

`python3 tools/check_package.py build --config Release --embedded` installs and
moves the package, then builds and runs separate C/C++ consumers against both
libraries. It also checks an embedded build with `BUILD_SHARED_LIBS=ON` and
verifies that the generated header embeds exactly the `server_xdp.o` the build
compiled. These checks are in CI.

## ABI compatibility

Each public config struct starts with `config_bytes`, the size of the struct
the caller compiled against, set by the inline default_config call. A library
reads a larger config as far as it knows and a smaller one as a prefix with
defaults after it, so a config struct can grow at its end without breaking a
program built against an older header (`docs/ABI-EVOLUTION.md`).

An incompatible layout or calling-contract change must increment the
major version. Additive functions alone do not require a revision
change. The other public structs carry no size, so a header and a library from
different ABI revisions must not be mixed; compare `rocketnet_version()` with
`ROCKETNET_VERSION` to catch a header and a shared library from different
releases.

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

A 10000-client build is qualified by
[docs/QUALIFICATION-max-clients-10000.md](docs/QUALIFICATION-max-clients-10000.md)
and re-proven nightly on the Linux fleet runner by
[`.github/workflows/nightly-max-clients.yml`](.github/workflows/nightly-max-clients.yml):
zero-warning builds with gcc and clang at DEV ON and OFF, the unit suite, and
a bounded loopback load of all 10000 clients for 30 seconds
(`LOAD_PROFILE=light ./bin/load 10000 30`). The load uses the light
per-client config, about 25 GB planned for the server and its 10000
in-process clients, because the default per-client config plans about
183 GB for the server alone (below); the runner checks host room against the
plan before the load runs.

The value raises the ceiling, not the default: `max_clients` still defaults
to 1000 (or the build's maximum when that is smaller), and a server asks for
more in its config or with `ROCKETNET_SERVER_MAX_CLIENTS`. Every limit that
serves all of a server's clients derives from it at compile time, with a
static assert holding each to its rule:

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

Two limits are fixed and asserted instead. The server packet lease table
(131072 entries, 65536 leases per update) holds two packets per client up to
the 32768 bound. The per-queue rings of the multi-queue transports (xdp, gso
and io_uring send staging of 4096, 4096 and 2048 per queue, and the XDP UMEM
geometry) do not grow with the client count: a destination hash spreads a
server's clients over its queues, so a build for more clients runs more
queues (`num_xdp_queues`, or `ROCKETNET_GSO_DEFAULT_QUEUES` and
`ROCKETNET_URING_DEFAULT_QUEUES` at build time) to keep two packets of
staging per client; the asserts check that the most queues can. The XDP
replay window (16 packets per client) is unchanged.

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
kernel buffers and your application. The default seven-queue XDP geometry alone
still requires roughly 4.2 GiB; select fewer queues or provision that explicitly.

## Pre-allocation server resource plans

`rocketnet_server_required_memory(config)` returns the peak bytes held by the
allocator over create, start, stop, restart and destroy for `config` at its
`max_clients`, including planned built-in transport allocations and the
development link simulator in development builds. The bound assumes unchanged
configuration and environment overrides; OS-owned memory and custom-transport
allocations are excluded. It works before
`rocketnet_init`, opens no sockets, starts no workers and allocates no memory.
Pass `NULL` config for defaults. The query reads the supplied configuration and
applies environment overrides (matching create), but does not check keys, probe
hardware or approve a configuration against the component budgets. `rocketnet_client_required_memory` is
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

Arguments are clients, maximum payload bytes, reassembly slots, concrete transport
and queue/worker count (zero selects that transport's default). `--plan-only` emits
a component breakdown, worker/socket counts and an explicit unknown OS/application
cost, then exits before initialization. `--session-plan-only` reports the public
server block plus its active transport's SDK allocations and host requests,
including socket buffers and thread stack reserves, without creating a server.
It requires no `ROCKETNET_SERVER_` or `ROCKETNET_DEV_SERVER_` environment overrides.
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

## Runtime environment inventory

These are the environment variables read by the SDK. Each setting has one name;
the client and server forms are separate settings. Entries marked development
only are absent from a production library. The unit inventory checks this list
in both directions against the SDK reads and the public header.

* Environment: ROCKETNET_CLIENT_PACKET_FILTER
* Environment: ROCKETNET_CLIENT_MAX_FRAGMENTED_MESSAGES
* Environment: ROCKETNET_CLIENT_STEP_DRIVEN (development only)
* Environment: ROCKETNET_CLIENT_TRANSPORT
* Environment: ROCKETNET_CLIENT_SEND_KBPS
* Environment: ROCKETNET_CLIENT_NUM_CHANNELS
* Environment: ROCKETNET_CLIENT_RELIABLE_MESSAGES_IN_FLIGHT
* Environment: ROCKETNET_CLIENT_RECEIVE_MESSAGE_BUFFER_BYTES
* Environment: ROCKETNET_CLIENT_MAX_MESSAGE_BYTES
* Environment: ROCKETNET_CLIENT_SEND_MESSAGE_BUFFER_BYTES
* Environment: ROCKETNET_CLIENT_SEND_QUEUE_MESSAGES
* Environment: ROCKETNET_CLIENT_PACKETS_IN_FLIGHT
* Environment: ROCKETNET_CLIENT_PACKETS_PER_UPDATE
* Environment: ROCKETNET_CLIENT_UNRELIABLE_MESSAGES_PER_UPDATE
* Environment: ROCKETNET_CLIENT_SEQUENCED_MESSAGES_HELD
* Environment: ROCKETNET_CLIENT_MAX_MESSAGE_RECEIPTS
* Environment: ROCKETNET_CLIENT_MESSAGE_RESEND_DELAY_INITIAL_MS
* Environment: ROCKETNET_CLIENT_MESSAGE_RESEND_DELAY_MIN_MS
* Environment: ROCKETNET_CLIENT_MESSAGE_RESEND_DELAY_MAX_MS
* Environment: ROCKETNET_CLIENT_RELIABLE_MESSAGE_TIMEOUT_MS
* Environment: ROCKETNET_DEV_CLIENT_PACKET_LOSS_PERCENT
* Environment: ROCKETNET_DEV_CLIENT_LATENCY_MS
* Environment: ROCKETNET_DEV_CLIENT_JITTER_MS
* Environment: ROCKETNET_DEV_CLIENT_DUPLICATE_PERCENT
* Environment: ROCKETNET_SERVER_PROTOCOL_ID
* Environment: ROCKETNET_SERVER_MAX_CLIENTS
* Environment: ROCKETNET_SERVER_PACKET_FILTER
* Environment: ROCKETNET_SERVER_BACKEND_PUBLIC_KEY
* Environment: ROCKETNET_SERVER_NUM_XDP_QUEUES
* Environment: ROCKETNET_SERVER_NUM_OS_CPUS
* Environment: ROCKETNET_SERVER_MAX_FRAGMENTED_MESSAGES
* Environment: ROCKETNET_SERVER_MAX_TIMEOUT_SECONDS
* Environment: ROCKETNET_SERVER_HANDSHAKE_DEADLINE_SECONDS
* Environment: ROCKETNET_SERVER_DISCONNECT_PACKETS_PER_UPDATE
* Environment: ROCKETNET_SERVER_TRANSPORT
* Environment: ROCKETNET_SERVER_IPV4_ADDRESS
* Environment: ROCKETNET_SERVER_IPV6_ADDRESS
* Environment: ROCKETNET_SERVER_PER_CLIENT_SEND_KBPS
* Environment: ROCKETNET_SERVER_NUM_CHANNELS
* Environment: ROCKETNET_SERVER_RELIABLE_MESSAGES_IN_FLIGHT
* Environment: ROCKETNET_SERVER_RECEIVE_MESSAGE_BUFFER_BYTES
* Environment: ROCKETNET_SERVER_MAX_MESSAGE_BYTES
* Environment: ROCKETNET_SERVER_SEND_MESSAGE_BUFFER_BYTES
* Environment: ROCKETNET_SERVER_SEND_QUEUE_MESSAGES
* Environment: ROCKETNET_SERVER_PACKETS_IN_FLIGHT
* Environment: ROCKETNET_SERVER_PACKETS_PER_UPDATE
* Environment: ROCKETNET_SERVER_UNRELIABLE_MESSAGES_PER_UPDATE
* Environment: ROCKETNET_SERVER_SEQUENCED_MESSAGES_HELD
* Environment: ROCKETNET_SERVER_MAX_MESSAGE_RECEIPTS
* Environment: ROCKETNET_SERVER_MESSAGE_RESEND_DELAY_INITIAL_MS
* Environment: ROCKETNET_SERVER_MESSAGE_RESEND_DELAY_MIN_MS
* Environment: ROCKETNET_SERVER_MESSAGE_RESEND_DELAY_MAX_MS
* Environment: ROCKETNET_SERVER_RELIABLE_MESSAGE_TIMEOUT_MS
* Environment: ROCKETNET_DEV_SERVER_PACKET_LOSS_PERCENT
* Environment: ROCKETNET_DEV_SERVER_LATENCY_MS
* Environment: ROCKETNET_DEV_SERVER_JITTER_MS
* Environment: ROCKETNET_DEV_SERVER_DUPLICATE_PERCENT
* Environment: ROCKETNET_LOG_LEVEL
* Environment: ROCKETNET_CLIENT_GAME_CLOCK_EPOCH
* Environment: ROCKETNET_SERVER_GAME_CLOCK_EPOCH
* Environment: ROCKETNET_CLIENT_SIMULATED_DROP_EVERY_NTH
* Environment: ROCKETNET_SERVER_SIMULATED_DROP_EVERY_NTH
* Environment: ROCKETNET_CLIENT_SIMULATED_REVERSE_WINDOW
* Environment: ROCKETNET_SERVER_SIMULATED_REVERSE_WINDOW
* Environment: ROCKETNET_CLIENT_MEMORY_BUDGET_BYTES
* Environment: ROCKETNET_SERVER_MEMORY_BUDGET_BYTES
* Environment: ROCKETNET_GATEWAY_ETHERNET_ADDRESS

## Server transports

Transport selection is runtime: one build carries every transport its
platform has. Linux production servers have xdp (privileged), gso (SO_REUSEPORT +
sendmmsg/recvmmsg + UDP GSO/GRO, non-root) and io_uring; macOS production
servers have msgx; Windows production servers have iocp and rio. Portable
servers and clients are also available on every platform. Production server
auto selection ranks portable after the native transports. Loopback is on every platform and is selected
only by name. The transport walks an ordered candidate
list at create. The list rides the config's transport field or the
per-endpoint `ROCKETNET_SERVER_TRANSPORT` and `ROCKETNET_CLIENT_TRANSPORT`
environment variables (precedence: default < struct < env):

    ./bin/functional                                # auto: the platform's recommended ladder
                                                #   (production linux: xdp,gso,io_uring,portable — falls on refusal)
    ROCKETNET_SERVER_TRANSPORT=portable ROCKETNET_CLIENT_TRANSPORT=portable ./bin/functional
                                                #   portable server is available in both build modes
                                                # a one-element list is a pin: that transport
                                                #   or loud failure, never a silent fall
    ROCKETNET_SERVER_TRANSPORT=gso,portable ./bin/functional  # exactly these, exactly this order

Names known but absent on a platform — or not applicable to an endpoint, like
`gso` for a client — are skipped with a log line, so one fleet-wide value deploys
everywhere; a list whose every name is skipped falls to that endpoint's own auto
order, loudly, rather than bricking an endpoint the value was not addressing.
An explicit server pin of `portable` selects only portable in either build.
Unknown names refuse loudly at create. The granted transport is reported by
the transport name query on each client and server, and the create log line. Queue-count bounds are per transport, refused loudly and never
clamped: the xdp transport takes any `num_xdp_queues` from 1, the gso transport refuses
past its 32-queue geometry. The GSO transport still probes UDP_SEGMENT and UDP_GRO at
create and degrades to plain sendmmsg/recvmmsg batching where a kernel lacks them —
that degradation is internal to the transport, below the ladder; likewise the Darwin
batch pump's live-refusal pause. Select the portable server in either
build with `ROCKETNET_SERVER_TRANSPORT=portable`; select the client's portable
side with `ROCKETNET_CLIENT_TRANSPORT=portable` in either build.
Run `./bin/functional`
against each rung. The unit
suite, `./bin/test`, does not start a transport, so a transport pin does not change
what it asserts.

The SDK test suite temporarily clears config overrides derived from both
config tables, then restores them on return. `ROCKETNET_SERVER_TRANSPORT` and
`ROCKETNET_CLIENT_TRANSPORT` remain intentional test pins;
`ROCKETNET_TEST_VERBOSE` keeps its diagnostic meaning. Passing runs print test names only; a
test a platform does not build is not registered there. Failures retain the
captured diagnostic log.

### Windows IOCP server

Native transport authors: [TRANSPORTS.md](TRANSPORTS.md) describes registration,
capabilities, ownership and the independently built extension example.

Select `transport = "iocp"` in the existing server config, or set
`ROCKETNET_SERVER_TRANSPORT=iocp`. The transport uses overlapped UDP with completion batches;
send buffers are recycled only after completion, and stop cancels and drains
outstanding operations. IPv6 binds accept both address families. The client
continues to use the portable path.

The existing `num_xdp_queues` field selects IOCP workers, following the GSO
transport's reuse of this field: zero selects one worker; 1 through 32 are valid.
Workers dequeue completions from the one port and each publishes its own receive
buffer, so more than one worker can reorder a client's datagrams, across workers
and across ticks, and the reliable layer answers the reordering with resends; one
worker is the default for that reason.
There are 128 posted receives per worker, 4096 receive staging slots per worker
buffer, and 2048 send frames across all workers (the staging and send frames
scale with `ROCKETNET_MAX_CLIENTS`; see "Maximum clients"). These are resource bounds,
not timing or packet-rate requirements. A send frame still owned by the OS is
unavailable to the next fill, even after an application flush.

Windows production server `auto` tries iocp, then rio; development builds
try portable first. Windows CI runs both configurations, plus
completion-lifetime, allocation-failure and shutdown tests with IOCP selected.
Compilation and loopback correctness do not establish NIC
line-rate performance; that requires measurements on suitable Windows hardware.
The current implementation shares one UDP socket between completion workers.
Its receive-queue contention must be measured before choosing whether Windows
needs RSS-sharded sockets or a different native backend for the line-rate tier.

## Deploying the XDP transport: provision, then verify-and-bind

The library never mutates machine state. `rocketnet_server_start( server )`
with the xdp transport is verify-and-bind only: a non-mutating pre-flight checks
that the machine is provisioned and refuses by name — each unmet condition logs
the knob, the wanted value, and the script that sets it — and the library's own
acts stay per-socket (its BPF program built, loaded and attached, AF_XDP binds,
the memlock rlimit). Provisioning uses `scripts/rocketnet-provision`, an operator
step; no application using XDP reconstructs it. The
script's header carries the authoritative
condition list; the pre-flight checks exactly those conditions.

Cold, on a fresh box, as root:

    # 1. BOOT TIER (once, then reboot): edit GRUB_CMDLINE_LINUX in /etc/default/grub
    #    to carry: nosmt irqaffinity=0 isolcpus=1-15 nohz_full=1-15
    #    then: update-grub && reboot
    #    (build a recent vendor NIC driver where the distro's is too old — for
    #    Intel 700 series, i40e from github.com/mas-bandwidth/ethernet-linux-i40e)

    # 2. RUNTIME TIER (idempotent, re-run any time):
    scripts/rocketnet-provision eth0 7

    # 3. Check without changing anything (what the library's pre-flight will say):
    scripts/rocketnet-provision eth0 7 verify

    # 4. Run the server: the xdp rung grants only on a verified machine, and the
    #    auto ladder falls to gso with the reason named where it is not.

    # 5. AFTER A CRASH (a server that died without destroy): each instance
    #    leaves a bpffs pin root named for the instance --
    #    /sys/fs/bpf/rocketnet_xdp_<token>_<pid>_<starttime>_<bootid> -- and
    #    the library never adopts or removes state it did not create this
    #    run, so crash leftovers are removed DELIBERATELY:
    scripts/rocketnet-provision sweep
    #    The sweep judges staleness by the metadata in each root's name
    #    (foreign boot id, dead pid, or recycled pid sweeps; a live matching
    #    instance is left; an unparseable name is left and named for manual
    #    judgment) and prints its container caveat before acting: with bpffs
    #    shared across pid namespaces, sweep from the namespace that owns
    #    the servers. Leftovers are inert -- a colliding create refuses
    #    loudly and names this sweep.

    # (scripts/rocketnet-provision selftest holds the script's IRQ-name
    #  matcher to the C test's case set AND drives the R3/R7 enforcement
    #  functions end to end, both modes, against stubs replaying captured
    #  xdp-tools output (R3) and real-shaped ethtool output (R7) -- every
    #  fail site in both functions has a driving
    #  case, and
    #  the WHOLE SCRIPT is driven door-to-fail by a full-flow verify case
    #  (there is no level above the whole script). it
    #  needs no root and touches only a private temp dir it creates and
    #  removes. CI runs it on every push, so neither the matcher halves nor
    #  the enforcement paths can drift silently.)

This configuration — isolated cores 1-15, seven queues, per-queue IRQ pinning,
the busy-poll pair — is the 10 Gbps point, as measured on a provisioned 10 GbE
Linux host; the magnitude belongs to that machine, and the claim attaches to a
verified-provisioned box, never to the library alone. The isolated-core mask, the
queue count and the IRQ pinning scale together with NIC bandwidth: one queue
wants one isolated core, and higher-bandwidth NICs need more cores (the script
takes the range as a parameter; 1-15 is the default, not a constant). `isolcpus`
is a boot parameter — nothing at runtime can perform it, which is why the script
only verifies the boot tier. The config's `num_os_cpus` names the first core the
xdp workers pin from: workers occupy `num_os_cpus + queue`, so it should point
at the isolated range.

Two honest limits of the IRQ-pinning check: drivers whose interrupt names do not
embed the interface name (mlx5-class) cannot be checked by name — both the
script and the pre-flight state that rather than passing it off as
nothing-to-pin — and drivers that add a misc interrupt carrying the bare
interface name (ixgbe-class) count one line beyond the queue count, so size the
isolated range for queues plus one there.

Honest labels: CI runs the committing xdp path (BPF program build/load/attach,
AF_XDP binds) only inside a virtme-ng guest, in the `xdp portable` cell of the
transport matrix, and `.github/workflows/nightly-soak.yml` runs
`bash tools/run_xdp_filter_guest.sh` inside a four-CPU virtme-ng guest on the
built `server_xdp.o` and `test_xdp_filter`, requiring all 18 `BPF_PROG_TEST_RUN`
and guest XDP cases to print `ok` with zero skips (`test_xdp_filter: 18 cases passed`;
see `ROOTGATE.md`). No CI leg attaches XDP to a real NIC, so that tier's claims
rest on a provisioned server. The pre-flight
supports both Ubuntu 22.04's stock libbpf 0.5 and newer libbpf/libxdp.
On 22.04 the build selects `bpf/xsk.h`, loads the embedded BPF object directly,
and uses the legacy XDP query/attach APIs. Attach refuses an occupied slot;
detach supplies the expected program fd so it cannot remove a replacement.
The modern libxdp path is unchanged. No process-global libbpf strict-mode
setting is changed. Installed CMake packages retain their selected provider.

### Ubuntu 22.04 build compatibility

Use the stock package command above and the ordinary CMake build. Configure
must report `XDP: using legacy AF_XDP APIs from libbpf (Ubuntu 22.04)`;
`XDP off` does not qualify an XDP build. The default build includes the embedded
BPF object, static/shared libraries, examples and XDP test executables. Run
`./bin/test_xdp_compat` for the legacy loader/attachment adapter contract checks
(no kernel operations), `./bin/test` for the unit suite, and
`python3 tools/check_package.py build` for relocated C/C++ consumers of both
libraries. Jammy's liburing 2.1 lacks multishot receive, so the existing
io_uring guard leaves that transport unavailable; XDP is independent of it.

Compilation and linking in an Ubuntu 22.04 container qualify its toolchain and
userspace libraries only. The host supplies the container's kernel. Loading,
attaching, zero-copy support and performance on the original 5.15 kernel and
the deployment NIC require separate runtime qualification.


## Floating point: rocketnet builds with -ffp-contract=off

Estate policy for mas-bandwidth network libraries: builds are strict about floating point
contraction. This build sets the right flag on every target, so nothing is required of you to
build rocketnet itself:

  - GCC/Clang: `-ffp-contract=off`. Not merely the absence of `-ffast-math` — GCC's default
    is `-ffp-contract=fast`, which contracts across statement boundaries, and clang's default
    `=on` still fuses within a single expression.
  - MSVC: `/fp:precise`.

rocketnet's float wire encoding is a bit copy (no arithmetic), so no flag is required of consumers
for correct wire bytes today. The strict build is the family floor: contraction is
architecture-dependent (FMA is in the aarch64 baseline and absent from the x86-64 one), so
float arithmetic near a wire diverges bit-wise between architectures without it. If you
compile rocketnet's sources into your own build rather than linking the library, carry the same
flag. The flag is private to rocketnet's own targets: neither the installed
package nor `add_subdirectory` passes it to your targets, whose floating-point
flags stay your own.
The canary `test_fp_contract_off` in `test.cpp` asserts the floor holds on every platform
(one rounding of `a*b+c` would change `0x00000000` to `0x33800000` / `0x3C90000000000000`),
and `tools/check_fp_contract.sh` (run in CI) fails any compiler invocation outside CMake that
omits `-ffp-contract=off` or `/fp:precise`.

### XDP transmit staging pressure

Each queue retains published staging generations until its worker has copied
all their packets into owned UMEM frames. Repeated application flushes cannot
reset an undrained generation. If both staging buffers remain owned by the
worker, new reservations return null and the transport's send-ring-full counter
records the refusal. Once the worker retires a buffer, a later reservation can
acquire it without requiring an extra update or flush. The ordinary fill path
adds an acquire load of the producer-buffer index; only recovery after both
buffers were queued takes the publication mutex.

## Fuzzing on macOS

Apple clang ships no libFuzzer runtime, so `-fsanitize=fuzzer` fails there with
`libclang_rt.fuzzer_osx.a not found`. Fuzz on a Mac with an LLVM toolchain that
carries it; the official LLVM macOS ARM64 release unpacked at
`$HOME/sdk/llvm` is one. Name that compiler once in `ROCKETNET_FUZZ_CC` and
`ROCKETNET_FUZZ_CXX` and both the library build and `fuzz/Makefile` pick it
up. Apple silicon (arm64) is the supported macOS host.

```
export ROCKETNET_FUZZ_CC="$HOME/sdk/llvm/bin/clang"
export ROCKETNET_FUZZ_CXX="$HOME/sdk/llvm/bin/clang++"
export CC="$ROCKETNET_FUZZ_CC" CXX="$ROCKETNET_FUZZ_CXX"

cmake -B build -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER="$ROCKETNET_FUZZ_CC" \
  -DCMAKE_CXX_COMPILER="$ROCKETNET_FUZZ_CXX" \
  -DCMAKE_C_FLAGS="-fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer" \
  -DCMAKE_CXX_FLAGS="-fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer"
cmake --build build --target rocketnet_dev --parallel 4

make -C fuzz ROCKETNET_FUZZ_CXX="$ROCKETNET_FUZZ_CXX"
```

That builds `rocketnet_fuzz`, `rocketnet_fuzz_message`,
`rocketnet_fuzz_connect_token` and `rocketnet_fuzz_receive_structured` in
`fuzz/`, each linked with `-fsanitize=fuzzer,address,undefined` and the same
`-ffp-contract=off` floor as the library. On macOS the link adds
`-framework CoreFoundation -framework SystemConfiguration`, which the platform
layer's interface lookup uses, as `CMakeLists.txt` does. Run one for fifteen
minutes from its seed corpus and the persisted corpus, with the wire
dictionary, and keep leak detection off: libFuzzer starts its own RSS-polling
thread and never joins it, so LeakSanitizer reports that frame on macOS
whatever the product does.

```
mkdir -p corpus/rocketnet_fuzz
ASAN_OPTIONS=detect_leaks=0 ./fuzz/rocketnet_fuzz corpus/rocketnet_fuzz fuzz/corpus \
  -dict=fuzz/rocketnet.dict -max_total_time=900 -print_final_stats=1
```

The other three take `corpus/rocketnet_fuzz_message` with
`fuzz/corpus_message`, `corpus/rocketnet_fuzz_connect_token` with
`fuzz/corpus_connect_token`, and `corpus/rocketnet_fuzz_receive_structured`
with `fuzz/corpus_receive_structured`. A reproducer libFuzzer writes
(`crash-*`, `oom-*`, `timeout-*`, `leak-*`) names the bug; a minimized input
belongs in the target's seed corpus as `regress_<name>` together with its
pcapng capture, which `tools/check_fuzz_regressions.py` enforces. The nightly
workflow's `fuzz-macos` job runs this same build for each target; it fails fast
with `TOOL-MISSING LLVM clang with libclang_rt.fuzzer_osx.a` when
`$HOME/sdk/llvm` carries no libFuzzer runtime, before any build runs.
`ci.yml`'s
`Replay the fuzz corpora without libFuzzer` step stays the gate for the four
committed corpora: build `fuzz_replay_rocketnet_fuzz`,
`fuzz_replay_rocketnet_fuzz_message`,
`fuzz_replay_rocketnet_fuzz_connect_token` and
`fuzz_replay_rocketnet_fuzz_receive_structured` and run each against its
`fuzz/corpus*` inputs, which executes every committed file exactly once. The
`ROCKETNET_FUZZ_CC`/`ROCKETNET_FUZZ_CXX` variable mechanism is not Apple
silicon specific: building the instrumented library with LLVM clang and
running `make -C fuzz ROCKETNET_FUZZ_CXX=...` produces the four harnesses with
`-fsanitize=fuzzer,address,undefined` on any host that carries a libFuzzer
runtime.
