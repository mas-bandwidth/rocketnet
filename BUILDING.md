# Building RocketNet

Build a development library to run the introductory examples. Build a
production library when connecting your own backend. `Release` selects
compiler optimization; `ROCKETNET_DEV` separately selects the SDK's development
features. A Release build with `ROCKETNET_DEV=ON` still uses development features.

All commands run from the repository root. Output goes to `bin/`; Visual
Studio and other multi-config generators add a configuration directory such
as `bin/Release/`. RocketNet vendors libsodium, so the default build needs no
external libsodium package.

## Prerequisites

| Platform | Build tools and dependencies |
| --- | --- |
| Linux | C/C++ compiler, CMake 3.20+, and make or another CMake generator. Native server builds use liburing and an AF_XDP provider; see below. |
| macOS | Xcode Command Line Tools and CMake 3.20+. Full Xcode is needed only for the Xcode generator. |
| Windows | Visual Studio 18 / 2026 with Desktop development with C++, the Windows SDK, and CMake 4.2+ for that generator. |

Python 3 is needed for repository check scripts. On Windows, `py -3` selects
the installed interpreter instead of a Microsoft Store `python` stub.

### Linux

On Ubuntu 24.04, install the native server build dependencies:

```sh
sudo apt update
sudo apt install -y cmake build-essential clang libbpf-dev libxdp-dev liburing-dev xxd
```

Ubuntu 22.04 supplies AF_XDP through libbpf 0.5; omit `libxdp-dev` there.
Build compatibility does not qualify a kernel or NIC for live XDP.
For a client build without native server dependencies, see
[Building a Linux client](#building-a-linux-client-without-the-server-libraries).

### macOS

Install Command Line Tools if needed (`xcode-select --install`), then install
CMake with your package manager (`brew install cmake` with Homebrew).
See [macOS](docs/MACOS.md) for universal builds and host behavior.

## Build and run a first example

On Linux or macOS:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DROCKETNET_DEV=ON
cmake --build build --target example_hello --parallel 4
./bin/example_hello
```

`PASS` means connection, message delivery, reconnect, and cleanup completed.
Continue with [Getting started](docs/GETTING-STARTED.md) for separate UDP processes.
To build everything, use `cmake --build build --parallel 4`; the repository
also provides `make` as a Linux/macOS convenience entry point.

## Windows

In PowerShell or cmd.exe:

```powershell
cmake -S . -B build -A x64 -DROCKETNET_DEV=ON
cmake --build build --config Release --target example_hello --parallel 4
.\bin\Release\example_hello.exe
```

Pass `--config Release` to both build and install. Visual Studio ignores
`CMAKE_BUILD_TYPE`; omitting the configuration builds Debug. Write each command
on one line; bash's `\` continuation is not Windows shell syntax.
See [Windows](docs/WINDOWS.md) for native transports and diagnostics.

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
game compiles matches its library. `ROCKETNET_DEV` is the one switch for every test seam; in a
production build (`-DROCKETNET_DEV=OFF`) `./bin/test` links `rocketnet`
directly to run the production-compatible unit suites and no seam is compiled
([docs/TEST-SCAFFOLDING.md](docs/TEST-SCAFFOLDING.md)).
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
without CMake, compile with `-DROCKETNET_DEV=0` for a production archive (and
`-DROCKETNET_MAX_CLIENTS=<n>` when the library was built with a non-default
value) so `rocketnet.h` matches the archive, and link against the C++ standard
library runtime (`-lstdc++` on Linux, `-lc++` on macOS) and the required
platform libraries (on Linux: `-lbpf -lxdp -luring -lstdc++ -lpthread -lm`,
where `-lbpf -lxdp` apply only when the library was built with XDP and
`-luring` applies only when the library was built with `io_uring`; on macOS:
`-lc++ -lpthread -lm -framework CoreFoundation`).

Supply the install prefix through `CMAKE_PREFIX_PATH`. The package resolves
Threads and the Linux bpf/xdp dependencies on the consuming machine. Sodium
is compiled into each SDK library with private symbol names; no sodium header,
DLL or package is required by consumers. Dependency metadata contains no
producer paths. Shared consumers inherit the Windows DLL import definition.
The package accepts a request for any earlier or equal version of the same major
version (`COMPATIBILITY SameMajorVersion`), so `find_package(RocketNet 1.0)`
finds every later 1.x patch. The imported targets
hand a consumer no floating-point option and no memory-budget definition: the
only compile definitions they carry are `ROCKETNET_DEV` and
`ROCKETNET_MAX_CLIENTS`, the values the library was built with, and
`ROCKETNET_SHARED` on `RocketNet::rocketnet_shared`. `tools/check_package.py`
checks all three.
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
primary static (`rocketnet` / `RocketNet::rocketnet`), primary shared
(`rocketnet_shared` / `RocketNet::rocketnet_shared`), build-tree development
static (`rocketnet_dev`), and build-tree development shared
(`rocketnet_shared_dev`). The primary pair follows `ROCKETNET_DEV`; only
`ROCKETNET_DEV=OFF` makes it production. SDK consumers that only link a subset of these variants
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

### Building Linux without XDP

XDP remains enabled by default. Set `-DROCKETNET_XDP=OFF` to build without
BPF/libxdp development headers or libraries, BPF clang, or `xxd`:

    cmake -S . -B build-no-xdp -DROCKETNET_XDP=OFF -DCMAKE_BUILD_TYPE=Release
    cmake --build build-no-xdp --parallel 4

The `xdp` rung reports that it was disabled at build time. A server start
with only that candidate is refused with `ROCKETNET_STATUS_TRANSPORT_NOT_BUILT`. A pin of `xdp` fails; automatic selection continues with `gso`, `io_uring`
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
is refused at server start with `ROCKETNET_STATUS_TRANSPORT_NOT_BUILT` and
a pin of an unavailable `io_uring` cannot start, and the installed CMake package requires none of the
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

## Test binaries and their tiers

After a full build, `./bin/test` runs deterministic unit tests and
`./bin/functional` runs tests of live sockets and transports. Windows uses
`bin\Release\test.exe` and `bin\Release\functional.exe`. A successful build
alone is not a test result. [Testing](TESTING.md) separates the test tiers and
indexes qualification; [contributor checks](docs/BUILD-REFERENCE.md) retain the
detailed runner, sanitizer, and toolchain instructions.

## Maximum clients

See [Memory and capacity](docs/MEMORY.md#maximum-clients) for compile-time
capacity, sizing, and the qualification limits above 1000 clients.

## History memory budget

See [History memory budget](docs/MEMORY.md#history-memory-budget).

## Pre-allocation server resource plans

See [Resource plans](docs/MEMORY.md#pre-allocation-server-resource-plans) for
allocator bytes, host resources, and transport geometry before startup.

## Runtime environment inventory

[Environment](docs/ENVIRONMENT.md) inventories deployment variables;
[Configuration](USAGE.md#configuration) explains precedence and field rules.

<details>
<summary>Complete SDK variable names checked by the source audit</summary>

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
* Environment: ROCKETNET_SERVER_HANDSHAKES_PER_UPDATE
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

</details>

## Server transports

Use [Transports](TRANSPORTS.md) to select a transport and
[Deployment](docs/DEPLOYING.md#server-transports) for native-server setup.

### Windows IOCP server

See [Windows IOCP deployment](docs/DEPLOYING.md#windows-iocp-server).

## Deploying the XDP transport: provision, then verify-and-bind

Follow [XDP provisioning](docs/DEPLOYING.md#deploying-the-xdp-transport-provision-then-verify-and-bind).

### Ubuntu 22.04 build compatibility

See [Ubuntu 22.04](docs/DEPLOYING.md#ubuntu-2204-build-compatibility).

## Floating point: rocketnet builds with -ffp-contract=off

See [Floating-point build policy](docs/BUILD-REFERENCE.md#floating-point-rocketnet-builds-with--ffp-contractoff).

### XDP transmit staging pressure

See [XDP staging pressure](docs/BUILD-REFERENCE.md#xdp-transmit-staging-pressure).

## Fuzzing on macOS

See [macOS fuzzing instructions](docs/BUILD-REFERENCE.md#fuzzing-on-macos).
