# Building rocketnet

Production libraries use an explicit source list. The SDK test suite is linked
only into `test` through `rocketnet_test_support`. Shared builds hide unannotated
implementation symbols; `tools/exports.txt` lists the supported C exports. CI
compares the built shared library with that list and checks that the static
archive contains no test suite. Review intentional ABI changes in the list as
part of the same change. The major version names SDK calling/layout
compatibility, separately from the wire version; see the ABI rules below.

Deploying a server on Linux: [docs/LINUX.md](docs/LINUX.md) covers the build, the production `xdp -> gso -> io_uring` transport ladder and pinning, what `gso`, `io_uring` and `xdp` need from the host, socket errors and ICMP, the fd and memlock limits for 1000 clients, containers, and how to verify a host. Development builds add portable last.

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

Binaries and libraries land in `bin/`. On macOS, [docs/MACOS.md](docs/MACOS.md)
is the guide: the build, the two transports and how to pin them, the client's
threads and App Nap, socket errors, the file-descriptor limit, the application
firewall prompt, lldb, fuzz corpus replay, and the commands that verify a Mac.
Run the unit suite with:

    ./bin/test

`./bin/test` does not open a socket and does not start a transport. It drives the
state machines and the in-memory network simulator. Tests run in parallel
(at most eight, or `ROCKETNET_TEST_JOBS` when that is smaller). In a Release
build a test over 120 seconds is killed and the suite fails. Debug and
sanitizer builds scale the limit by 5. A passing run prints each test's
name and then `*** ALL TESTS PASSED ***`; `ROCKETNET_TEST_REPORT=1` adds the
ten slowest cases and the ten largest peak resident sets after the results
(test/README.md).

A test that calls `socket`, `bind`, `sendto`, `recvfrom`, `select`, `epoll`,
IOCP, or AF_XDP, or that starts a real transport (`rocketnet_server_create`,
`rocketnet_client_create`, `rocketnet_platform_socket_create`, or a
sibling binary that does), belongs in the functional suite:

    ./bin/functional

The functional suite is its own target. It runs in parallel under the same
120 second failure limit and prints the same way. Put a new test in `./bin/test` when the assertion can be
reached through a state machine or the simulator. Put it in `./bin/functional`
when the assertion is about a real socket or a real transport. A test that
cannot run without a socket is functional, and it keeps its assertions.
The functional-only sibling binaries are `test_serialize` (a real clock and
`rocketnet_sleep`), `test_xdp_filter` (a kernel, root or CAP_BPF, and
`usleep`), `test_transport`, `test_transport_control`, `test_lifecycle`,
`test_msgx` and `test_iocp`; they are functional tests, never `./bin/test`.
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

| Binary | Tier | Why | Where it runs |
| --- | --- | --- | --- |
| `test` | unit | state machines and the in-memory simulator | CI `Run unit tests` |
| `test_wide` | unit | `test.cpp` built with 512 channels (the `test_wide` target in CMakeLists.txt) | by hand |
| `test_sodium` | unit | known-answer vectors | CI `Sodium known answers` |
| `fuzz_replay_*` | unit | corpus replay, no init (the `fuzz_replay_${harness}` targets in CMakeLists.txt) | CI `Replay the fuzz corpora without libFuzzer` |
| `functional` | functional | real sockets and transports | CI `Run functional suite` |
| `test_serialize` | functional | sleep-wait loops (test_serialize.cpp) | `./bin/functional` (the `execs` tables in `main`, functional/main.cpp) |
| `test_transport_control` | functional | creates a server transport on a loopback socket and reads `rocketnet_time()` (test_transport_control.cpp) | `./bin/functional` |
| `test_xdp` | functional | the AF_XDP transport | `./bin/functional` on Linux |
| `test_xdp_filter` | functional | loads the XDP program with BPF_PROG_TEST_RUN; root | by hand |
| `test_msgx` | functional | a loopback socket and sleep-wait loops (test_msgx.cpp) | `./bin/functional` on macOS |
| `test_iocp` | functional | loopback sockets, threads and sleep-wait loops (test_iocp.cpp) | `functional.exe` on Windows |
| `test_lifecycle` | functional | threads, `time(NULL)` tokens, and real server and client transports (test_lifecycle.cpp) | CI `Functional: lifecycle binary` |
| `test_transport` | functional | netlink route reads, and the portable transport on loopback sockets with sleep-wait loops (`test_driver.cpp`) | CI `Functional: native driver contract and external backend` |

The channel cases in `./bin/functional` (`functional/channel_*.cpp`) run on
`rocketnet_channel_harness_t` (functional/channel_harness.h): the in-memory
simulator and the test clock, with no socket and no sleep.

Dependencies, per platform, as CI installs them:

    # Linux -- the build links the XDP dataplane, liburing is the bench
    # io_uring server transport (without it that transport is absent); clang with the BPF target compiles the XDP
    # program at build time (the server host needs no compiler)
    sudo apt install cmake build-essential clang libbpf-dev libxdp-dev liburing-dev xxd

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
AddressSanitizer runtime (`clang_rt.asan_dynamic-x86_64.dll`, beside `cl.exe`)
into `bin\<config>\` so `bin\Release\test.exe`, `bin\Release\test_lifecycle.exe`,
`bin\Release\test_iocp.exe` and `bin\Release\functional.exe` run directly.

`py -3 tools\windows\appverif_run.py -- bin\Debug\test_iocp.exe` runs one
executable under the Application Verifier Basics layers (Handles, Heaps, Locks,
Memory, TLS, Threadpool and Leak) with every stop logged and no debugger
attached (a stop ends the run once it is logged), exports the verifier log of
every process it starts with that image name (the suite runners start one child
per case), and exits non-zero naming each stop's layer, stop code and top frames; it disables
verification for the executable again on every exit path. It needs Application Verifier for x64 from
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
compiler. The generated header lives in each build tree, so concurrent
configurations do not rewrite the checkout. macOS and Windows do not build the
object.

### Building Linux without XDP

XDP remains enabled by default. Set `-DROCKETNET_XDP=OFF` to build without
BPF/libxdp development headers or libraries, BPF clang, or `xxd`:

    cmake -S . -B build-no-xdp -DROCKETNET_XDP=OFF -DCMAKE_BUILD_TYPE=Release
    cmake --build build-no-xdp --parallel 4

The `xdp` rung reports that it was disabled at build time and refuses create.
A pin of `xdp` fails; automatic selection continues with `gso`, `io_uring`
(when built), then `portable` in development builds only. The installed CMake package also omits the
BPF/libxdp dependencies. Native `test_xdp` and `test_xdp_filter` targets are
not built; the host-C filter and portable frame-codec unit tests still run.
Configure with `-DROCKETNET_XDP=ON` to restore the native backend and its
dependencies. Missing BPF/libxdp development files remain a configure error
when XDP is enabled.

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

## Installing and embedding the SDK

Build and install both libraries without examples, tests or tools:

    cmake -S . -B build-sdk -DCMAKE_BUILD_TYPE=Release \
      -DROCKETNET_BUILD_TESTS=OFF -DROCKETNET_BUILD_EXAMPLES=OFF -DROCKETNET_BUILD_TOOLS=OFF
    cmake --build build-sdk --parallel
    cmake --install build-sdk --prefix /your/sdk/prefix

On Windows, write each command on one line, and pass `--config Release` to the
build and the install. The Visual Studio generator is multi-config: it ignores
`CMAKE_BUILD_TYPE`, a build without `--config` makes Debug, and the install then
fails with `file INSTALL cannot find ".../build-sdk/Release/rocketnet.lib"`. In
PowerShell or cmd.exe:

    cmake -S . -B build-sdk -A x64 -DROCKETNET_BUILD_TESTS=OFF -DROCKETNET_BUILD_EXAMPLES=OFF -DROCKETNET_BUILD_TOOLS=OFF
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
only compile definition they carry is `ROCKETNET_SHARED` on
`RocketNet::rocketnet_shared`. `tools/check_package.py` checks both.
The Unix shared-library soname follows the major version.
Use a separate install prefix for each build configuration. With Visual Studio
or another multi-config generator, pass `--config Debug` or `--config Release`
to build/install and select the same configuration in the consumer. A
Debug-only prefix should be consumed with `-DCMAKE_CONFIGURATION_TYPES=Debug`,
so toolchains do not try to generate other configurations requiring uninstalled
Release artifacts. Installing another configuration into the same prefix
overwrites the libraries; side-by-side Debug/Release installation is not supported.

`add_subdirectory(path/to/rocketnet)` exposes the same two namespaced targets.
Examples, tests and tools default off when embedded, and the parent's
`BUILD_SHARED_LIBS` does not change the static target. Explicit
`ROCKETNET_BUILD_*` options override those defaults.

The installed SDK is the production library, and it has no development token
minting by design. `rocketnet_create_dev_connect_token` and
`ROCKETNET_DEV_BACKEND_PUBLIC_KEY` are declared only with
`ROCKETNET_DEVELOPMENT=1`; defining that macro while linking a production
library does not provide the development function. For a first loopback try, embed the
checkout and link both the server and the client to the development library,
`rocketnet_dev`, which supplies `ROCKETNET_DEVELOPMENT=1`, creates development
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
`ROCKETNET_REASSEMBLY_MEMORY_MIB` defaults to 512 and `ROCKETNET_XDP_MEMORY_MIB`
to 4096. Both accept 1 through 1048576 MiB. Reassembly includes its slot records,
arena and delivered mirror; XDP includes userspace queue structs and UMEM.
Oversized geometry is refused with required and permitted byte counts before
allocation. These bounds are not an available-RAM check: reserve additional
memory for ACK history, other transport objects, allocator overhead, stacks,
kernel buffers and your application. The default seven-queue XDP geometry alone
still requires roughly 4.2 GiB; select fewer queues or provision that explicitly.

## Pre-allocation server resource plans

`rocketnet_server_required_memory(config)` returns the bytes of the one block
`rocketnet_server_create` allocates through the allocator for `config` at its
`max_clients`. It works before
`rocketnet_init`, opens no sockets, starts no workers and allocates no memory.
Pass `NULL` config for defaults. The query reads the supplied configuration and
applies environment overrides (matching create), but does not check keys, probe
hardware or approve a configuration against the component budgets. `rocketnet_client_required_memory` is
the client form. The detailed per-transport plan and the kernel-facing host memory
estimate are internal to the transport layer (`sdk/rocketnet_layers.h`), not part
of the public client and server API.

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
rings (the server asks for 2048 entries each, `XSK_RING_PROD__DEFAULT_NUM_DESCS`
and `XSK_RING_CONS__DEFAULT_NUM_DESCS`, in `rocketnet_xdp_umem_config`,
sdk/rocketnet_xdp_socket_config.h), libbpf/libxdp and the XDP program's maps, the io_uring
submission, completion and provided buffer rings, thread guard pages
and TLS, kernel page tables, loader memory, libc bookkeeping outside the SDK
allocator, and application memory. Stacks count reserved address space, not
resident pages. With default configuration and an 8 MiB default stack, on Linux:

| transport | required_memory | sockets | socket buffers | XDP rings | threads x stack | estimate total |
| --- | --- | --- | --- | --- | --- | --- |
| portable | 688,206,108 | 1 | 2,000,000 | 0 | 0 | 690,206,108 |
| gso | 830,413,923 | 4 | 8,000,000 | 0 | 4 x 8 MiB | 871,968,355 |
| io_uring | 799,247,707 | 4 | 8,000,000 | 0 | 0 | 807,247,707 |
| xdp | 4,473,629,517 (UMEM 1,879,048,192) | 7 | 0 | 3,670,016 | 7 x 8 MiB | 4,536,019,789 |

Build tools normally, then use the resource probe:

```
./bin/transport_memory 1000 1192 16 portable 0 --plan-only
# A live portable server probe requires a development build.
./bin/transport_memory 1000 1192 16 gso 0
# Linux only; this prints geometry without attaching XDP or allocating UMEM:
./bin/transport_memory 1000 1192 1 xdp 7 --plan-only
python3 tools/check_memory.py
```

Arguments are clients, maximum payload bytes, reassembly slots, concrete transport
and queue/worker count (zero selects that transport's default). `--plan-only` emits
a component breakdown, worker/socket counts and an explicit unknown OS/application
cost, then exits before initialization. Without it, the probe creates a loopback
server, compares independent allocator live/peak observations against the plan,
reports process peak RSS separately and checks that destruction frees every SDK
allocation. Clear deployment `ROCKETNET_*` overrides when profiling a specific
argument set: real create still obeys those overrides and a mismatch fails loudly.
RSS and requested allocator bytes measure different things; untouched allocations
may not yet be resident. The probe never commits an XDP attachment.

On the shared test machine, Release portable plans matched all 40 live allocations:

| Clients | Payload bytes | Reassembly slots | Planned/observed allocator bytes |
| ---: | ---: | ---: | ---: |
| 1 | 1192 | 1 | 32,609,307 |
| 2 | 1192 | 1 | 32,937,054 |
| 1000 | 1192 | 1 | 360,028,560 |
| 1000 | 1192 | 16 | 417,433,560 |
| 1000 | 4096 | 16 | 510,407,896 |

These are target-specific constructor measurements, not deployment limits. The
maximum legal numeric reassembly geometry plans roughly 146 GiB and can be queried
without allocating it; the default reassembly component budget refuses its create.
`test_transport_control` also exercises six maintenance passes over 1000 mixed
ACK/keepalive clients while the buffered transport accepts only 256 controls per send
batch. Its elapsed time is a diagnostic for that synthetic workload, not a packet
rate or a frame/send/ACK cadence requirement.

## Runtime environment inventory

These are the environment variables read by the SDK. Each setting has one name;
the client and server forms are separate settings. The unit inventory checks this
list in both directions against the SDK reads and the public header.

* Environment: ROCKETNET_CLIENT_PACKET_FILTER
* Environment: ROCKETNET_CLIENT_MAX_PAYLOAD_BYTES
* Environment: ROCKETNET_CLIENT_MAX_FRAGMENTED_MESSAGES
* Environment: ROCKETNET_CLIENT_STEP_DRIVEN
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
* Environment: ROCKETNET_SERVER_MAX_PAYLOAD_BYTES
* Environment: ROCKETNET_SERVER_MAX_FRAGMENTED_MESSAGES
* Environment: ROCKETNET_SERVER_MAX_TIMEOUT_SECONDS
* Environment: ROCKETNET_SERVER_HANDSHAKE_DEADLINE_SECONDS
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
servers have msgx; Windows production servers have iocp and rio. The portable
server is available only in development builds; its client side remains
available on every platform. Loopback is on every platform and is selected
only by name. The transport walks an ordered candidate
list at create. The list rides the config's transport field or the
per-endpoint `ROCKETNET_SERVER_TRANSPORT` and `ROCKETNET_CLIENT_TRANSPORT`
environment variables (precedence: default < struct < env):

    ./bin/functional                                # auto: the platform's recommended ladder
                                                #   (production linux: xdp,gso,io_uring — falls on refusal)
    ROCKETNET_SERVER_TRANSPORT=portable ROCKETNET_CLIENT_TRANSPORT=portable ./bin/functional
                                                #   portable server requires a development build
                                                # a one-element list is a pin: that transport
                                                #   or loud failure, never a silent fall
    ROCKETNET_SERVER_TRANSPORT=gso,portable ./bin/functional  # exactly these, exactly this order

Names known but absent on a platform — or not applicable to an endpoint, like
`gso` for a client — are skipped with a log line, so one fleet-wide value deploys
everywhere; a list whose every name is skipped falls to that endpoint's own auto
order, loudly, rather than bricking an endpoint the value was not addressing.
An explicit server pin of `portable` in a production build refuses with
`ROCKETNET_STATUS_INVALID_TRANSPORT_LIST` instead of falling to auto.
Unknown names refuse loudly at create. The granted transport is reported by
the transport name query on each client and server, and the create log line. Queue-count bounds are per transport, refused loudly and never
clamped: the xdp transport takes any `num_xdp_queues` from 1, the gso transport refuses
past its 32-queue geometry. The GSO transport still probes UDP_SEGMENT and UDP_GRO at
create and degrades to plain sendmmsg/recvmmsg batching where a kernel lacks them —
that degradation is internal to the transport, below the ladder; likewise the Darwin
batch pump's live-refusal pause. Select the portable server in a development
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
buffer, and 2048 send frames across all workers. These are resource bounds,
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
transport matrix. No CI leg attaches XDP to a real NIC, so that tier's claims
rest on a provisioned server. The pre-flight
also sets the tree's libbpf floor at 1.0 — `bpf_xdp_query_id` plus the uniform
negative-errno error convention the pre-flight's error reporting assumes
(pre-1.0 libbpf returned -1 with errno set and would misreport there); Ubuntu
22.04's libbpf 0.5 is below the floor, 24.04's 1.x is comfortably above.

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
