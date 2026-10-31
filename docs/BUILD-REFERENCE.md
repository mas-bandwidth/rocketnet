# Build reference and contributor checks

For a first build, install, or embedded SDK, use [Building](BUILDING.md).
This page retains toolchain details, contributor test policy, sanitizer setup,
and build-system design. Commands run from the repository root. Historical
toolchain observations describe the recorded run, not qualification of a newer
checkout; current evidence is indexed in [Testing](TESTING.md).

## Build-system boundaries and test policy

Production libraries use an explicit source list. The SDK test suite is linked
only into `test` through `rocketnet_test_support`. Shared builds hide unannotated
implementation symbols; `tools/exports.txt` lists the supported C exports. CI
compares the built shared library with that list and checks that the static
archive contains no test suite. Review intentional ABI changes in the list as
part of the same change. The major version names SDK calling/layout
compatibility, separately from the wire version; see [ABI compatibility](BUILDING.md#abi-compatibility).

Deploying a server on Linux: [docs/LINUX.md](LINUX.md) covers the build, the production `xdp -> gso -> io_uring -> portable` transport ladder and pinning, what `gso`, `io_uring` and `xdp` need from the host, socket errors and ICMP, the fd and memlock limits for 1000 clients, containers, and how to verify a host. Both builds offer portable as the final Linux fallback.

RocketNet requires CMake 3.20 or later. The Windows instructions below use
Visual Studio 18, whose generator requires CMake 4.2 or later. Recorded Linux
checks used gcc 13.3.0 and 15.2.0 and clang 18.1.3 and 21.1.8, in Debug and
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

Binaries and libraries land in `bin/`. On macOS, [docs/MACOS.md](MACOS.md)
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
file: the known-answer fixtures it checks (`standard/vectors/connect_token_v1.json`
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
`ROCKETNET_TEST_REPORT=1` reports the suite's slowest cases, and
`ROCKETNET_TEST_REPORT=all` every case. Every functional case, including
sibling binaries, runs in parallel, one job per core (`ROCKETNET_TEST_JOBS`
to change it), so the per-commit run fits its two minutes. A case that cannot
share the host is fixed, or moved to the nightly tier
(`ROCKETNET_TEST_NIGHTLY=1`, tests/functional/main.cpp), never run alone per commit.

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
the repository root ([examples/README.md](../examples/README.md) lists them):

    python3 tools/check_examples.py
    python3 tools/run_examples.py --build build

### Test binaries and their tiers

Every test binary is in exactly one tier. A unit binary opens no socket,
starts no thread, never sleeps and reads time only as an argument (the test
clock starts at 1000.0). A binary that opens a real socket, starts a thread,
sleeps or reads the wall clock is functional, and CI runs it only in a step
named functional, never as a unit step.

Unit cases feed SDK environment parsing through the in-memory source compiled
only under `ROCKETNET_DEV`; they never read or mutate the process
environment. `tests/functional/environment_wiring.cpp` sets and restores real
variables and pins the SDK wiring, precedence, and refusal paths.

| Binary | Tier | Why | Where it runs |
| --- | --- | --- | --- |
| `test` | unit | state machines and the in-memory simulator | CI `Run unit tests` |
| `test_wide` | unit | `test.cpp` built with 512 channels (the `test_wide` target in CMakeLists.txt; `EXCLUDE_FROM_ALL`, built by name with `cmake --build build --target test_wide`) | by hand |
| `test_sodium` | unit | known-answer vectors | CI `Sodium known answers` |
| `fuzz_replay_*` | unit | corpus replay, no init (the `fuzz_replay_${harness}` targets in CMakeLists.txt) | CI `Replay the fuzz corpora without libFuzzer` |
| `functional` | functional | real sockets, transports and shared-library exports | CI `Run functional suite` |
| `test_serialize` | functional | sleep-wait loops (test_serialize.cpp) | `./bin/functional` (the `execs` tables in `main`, tests/functional/main.cpp) |
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

The channel cases in `./bin/functional` (`tests/functional/channel_*.cpp`) run on
`rocketnet_channel_harness_t` (tests/functional/channel_harness.h): the in-memory
simulator and the test clock, with no socket and no sleep.

The corpus-consistency cases in `./bin/functional`
(tests/functional/corpus_consistency.cpp: `test_pcapng_corpus_replay`,
`test_pcapng_corpus_witness_unread_packet_goes_red`,
`test_pcapng_corpus_covers_control_packet_types` and
`test_fuzz_prefilter_seed_names_match_outcomes`) read `tests/pcapng/`,
`tests/pcapng/regress/` and `tests/fuzz/corpus/` at run time, directories that grow
as findings land, so they are functional cases, never unit ones.

The two process thread-count cases live in `tests/functional/process_thread_counts.cpp`
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
`bin/Release/`. The functional shared-library token cases
(`tests/functional/shared_library_tokens.cpp`) open the development and
production libraries through their generated absolute build paths. These
OS-loader checks are functional tests, not unit tests.

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
see [Installing and embedding the SDK](BUILDING.md#installing-and-embedding-the-sdk).

Every rocketnet server binds the wildcard address (`0.0.0.0:40000` for the
example server), so on an interactive desktop the first run of `server.exe`,
`functional.exe` or a game server raises the Windows Defender Firewall dialog.
Loopback traffic works whatever is chosen there; allow access only on the
networks remote clients connect from.

The build compiles `sdk/xdp/server_xdp.c` to `server_xdp.o` with a clang that
has the BPF target. It embeds the bytes in the library. The server host needs no
compiler. AF_XDP comes from libxdp on newer systems or from libbpf 0.5
(`bpf/xsk.h`) on Ubuntu 22.04. Without such a clang or an AF_XDP provider, or with
`-DROCKETNET_XDP=OFF`, the build still succeeds with the XDP transport compiled
out: the library carries no XDP code and links neither library, and a
server pinned to `xdp` refuses start with `ROCKETNET_STATUS_TRANSPORT_NOT_BUILT`. The generated header lives in each build tree, so concurrent
configurations do not rewrite the checkout. macOS and Windows do not build the
object.

## Building the API reference

`make docs` generates the public API reference at
`build/docs/html/index.html`. It needs CMake, a C/C++ toolchain, Python 3 with
PyYAML, and the MrDocs version pinned in `tools/docs/mrdocs.sh`. On Linux and
macOS that script can download the pinned MrDocs archive with `curl`, verify
its SHA-256 digest, and unpack it with `tar`. Set `MRDOCS` to a matching local
binary to use an existing installation.

Install Python dependencies in a local virtual environment when PyYAML is not
already available to `python3`:

```sh
python3 -m venv build/docs-venv
. build/docs-venv/bin/activate
python3 -m pip install PyYAML
make docs
make docs-undocumented
```

The script uses `python3` from `PATH`, so keep the virtual environment active
for both commands. `ModuleNotFoundError: No module named 'yaml'` means that
interpreter lacks PyYAML. The HTML build and the undocumented-symbol check
are separate from the compiled documentation examples and link checks in
[the documentation workflow](../.github/workflows/docs.yml).

## Floating point: rocketnet builds with -ffp-contract=off

Estate policy for mas-bandwidth network libraries: builds are strict about floating point
contraction. This build sets the right flag on every target, so nothing is required of you to
build rocketnet itself:

  - GCC/Clang: `-ffp-contract=off`. Not merely the absence of `-ffast-math` — GCC's default
    is `-ffp-contract=fast`, which contracts across statement boundaries, and clang's default
    `=on` still fuses within a single expression.
  - MSVC: `/fp:precise`, which does not contract.
  - clang-cl: `/fp:precise -Xclang -ffp-contract=off`. clang-cl maps `/fp:precise` to
    `-ffp-contract=on`, which fuses `a*b+c` wherever the target has FMA (ARM64, or x64 with
    `/arch:AVX2`). `/clang:-ffp-contract=off` sets the same mode, but the driver warns that it
    overrides `/fp:precise`, and `/WX` makes that warning an error.

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
configuring the tree fails when any target it compiles lacks its compiler family's flag above
or carries a contracting one (`rocketnet_check_fp_floor` in `CMakeLists.txt`), and
`tools/check_fp_contract.sh` (run in CI) fails any compiler invocation outside CMake that
omits the flag for its compiler.

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
`ROCKETNET_FUZZ_CXX` and both the library build and `tests/fuzz/Makefile` pick it
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

make -C tests/fuzz ROCKETNET_FUZZ_CXX="$ROCKETNET_FUZZ_CXX"
```

That builds `rocketnet_fuzz`, `rocketnet_fuzz_message`,
`rocketnet_fuzz_connect_token` and `rocketnet_fuzz_receive_structured` in
`tests/fuzz/`, each linked with `-fsanitize=fuzzer,address,undefined` and the same
`-ffp-contract=off` floor as the library. On macOS the link adds
`-framework CoreFoundation -framework SystemConfiguration`, which the platform
layer's interface lookup uses, as `CMakeLists.txt` does. Run one for fifteen
minutes from its seed corpus and the persisted corpus, with the wire
dictionary, and keep leak detection off: libFuzzer starts its own RSS-polling
thread and never joins it, so LeakSanitizer reports that frame on macOS
whatever the product does.

```
mkdir -p corpus/rocketnet_fuzz
ASAN_OPTIONS=detect_leaks=0 ./tests/fuzz/rocketnet_fuzz corpus/rocketnet_fuzz tests/fuzz/corpus \
  -dict=tests/fuzz/rocketnet.dict -max_total_time=900 -print_final_stats=1
```

The other three take `corpus/rocketnet_fuzz_message` with
`tests/fuzz/corpus_message`, `corpus/rocketnet_fuzz_connect_token` with
`tests/fuzz/corpus_connect_token`, and `corpus/rocketnet_fuzz_receive_structured`
with `tests/fuzz/corpus_receive_structured`. A reproducer libFuzzer writes
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
`tests/fuzz/corpus*` inputs, which executes every committed file exactly once. The
`ROCKETNET_FUZZ_CC`/`ROCKETNET_FUZZ_CXX` variable mechanism is not Apple
silicon specific: building the instrumented library with LLVM clang and
running `make -C tests/fuzz ROCKETNET_FUZZ_CXX=...` produces the four harnesses with
`-fsanitize=fuzzer,address,undefined` on any host that carries a libFuzzer
runtime.
