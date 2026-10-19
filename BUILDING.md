# Building rocketnet

Production libraries use an explicit source list. The SDK test suite is linked
only into `test` through `rocketnet_test_support`. Shared builds hide unannotated
implementation symbols; `tools/exports.txt` lists the supported C exports. CI
compares the built shared library with that list and checks that the static
archive contains no test suite. Review intentional ABI changes in the list as
part of the same change. `ROCKETNET_ABI_VERSION` identifies SDK calling/layout
compatibility separately from the wire version; see the ABI rules below.

rocketnet builds on Linux and macOS with CMake 3.20 or later, and on Windows with
CMake 4.2 or later (the Windows block below says why). On Linux and macOS the
Makefile is the front door:

    make

Binaries and libraries land in `bin/`. Run the unit suite with:

    ./bin/test

`./bin/test` does not open a socket and does not start a transport. It drives the
state machines and the in-memory network simulator. Tests run in parallel
(at most eight, or `ROCKETNET_TEST_JOBS` when that is smaller). A test over
60 seconds prints a warning line. A test over 120 seconds is killed and the
suite fails. The ten slowest are printed at the end. Those timing lines use
the `test_timing_* [SKIPPED ...]` spelling because `tools/check_test_environment.py`
accepts only test-name lines on a passing `./bin/test` run, and that checker
is shared with the unit suite.

A test that calls `socket`, `bind`, `sendto`, `recvfrom`, `select`, `epoll`,
IOCP, or AF_XDP, or that starts a real transport (`rocketnet_server_create`,
`rocketnet_client_create`, `rocketnet_platform_socket_create`, or a
sibling binary that does), belongs in the functional suite:

    ./bin/functional

The functional suite is its own target. It runs in parallel under the same
60 second warning and 120 second failure limits, and it prints the ten
slowest as a table. Put a new test in `./bin/test` when the assertion can be
reached through a state machine or the simulator. Put it in `./bin/functional`
when the assertion is about a real socket or a real transport. A test that
cannot run without a socket is functional, and it keeps its assertions.
Where the same rule is already covered by a simulator or state-machine test,
that unit test stays beside it. The connect path's unit twin is
`test_client_server_connect` (and the simulator loss walks). Fragment and
ACK behavior already has protocol tests in the unit suite. The keyless-config
create is functional; `test_server_keyless_config_has_no_keys` checks the
config the gate reads, without creating a transport.

Dependencies, per platform, as CI installs them:

    # Linux -- the build links the XDP dataplane, and liburing is the bench
    # harness's io_uring rung
    sudo apt install cmake build-essential libbpf-dev libxdp-dev liburing-dev xxd

    # macOS
    brew install cmake

Windows has no Makefile path; CMake is driven directly, and the binaries land
under `bin\<config>\`. It needs, installed once:

- Visual Studio 18 (Visual Studio 2026) with the **Desktop development with C++**
  workload, which carries the MSVC x64 toolchain and the Windows SDK.
- CMake 4.2 or later. 4.2 is the first CMake with the `Visual Studio 18 2026`
  generator (CMake 4.2 release notes,
  https://cmake.org/cmake/help/latest/release/4.2.html: "The Visual Studio 18
  2026 generator was added"), and an older CMake does not generate for
  Visual Studio 18.
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

The examples series starts with a single-process loopback example and a
two-process UDP client/server pair ([examples/README.md](examples/README.md)):

    bin\Release\example_hello.exe
    bin\Release\example_client_server_server.exe
    bin\Release\example_client_server_client.exe

For `example_client_server_server.exe` and `example_client_server_client.exe`,
run the server first and then the client; the server listens on
`127.0.0.1:40000`, echoes one reliable-ordered message, and exits cleanly once
the client disconnects. For a first loopback try from your own project,
see the development-token note under "Installing and embedding the SDK" below.

Every UDP rocketnet server binds the wildcard address (`0.0.0.0:40000` for the
example server), so on an interactive desktop the first run of
`example_client_server_server.exe`, `functional.exe` or a game server raises
the Windows Defender Firewall dialog.
Loopback traffic works whatever is chosen there; allow access only on the
networks remote clients connect from.

The embedded XDP program (`server_xdp/`) is packed as a source tarball by the build and
compiles later on the target kernel, which needs BTF; it is not compiled by `make` here.
The generated header lives in each build tree, so concurrent configurations do
not rewrite the checkout. Windows copies the committed header into its build tree.

## Pinned sodium

The default build uses the committed `sodium/sodium.c` and `sodium/sodium.h`
pair. It needs no crypto download, code generator or system libsodium.
`sodium/README.md` records provenance, licenses, exact regeneration and parity
checks. The fixed-vector test is `bin/test_sodium`.

For integration testing against an independently supplied libsodium, configure
with `-DROCKETNET_USE_SYSTEM_SODIUM=ON`. That opt-in uses pkg-config on POSIX,
or `ROCKETNET_SODIUM_INCLUDE_DIR` and `ROCKETNET_SODIUM_LIBRARY` on Windows.
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
find_package(RocketNet 1.0.0 EXACT CONFIG REQUIRED)
target_link_libraries(game PRIVATE RocketNet::rocketnet)        # static
# Or: target_link_libraries(game PRIVATE RocketNet::rocketnet_shared)
```

Supply the install prefix through `CMAKE_PREFIX_PATH`. The package resolves
Threads and the Linux bpf/xdp dependencies on the consuming machine. Sodium
is compiled into each SDK library with private symbol names; no sodium header,
DLL or package is required by consumers. Dependency metadata contains no
producer paths. Shared consumers inherit the Windows DLL import definition.
The Unix shared-library soname follows `ROCKETNET_ABI_VERSION`.
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
minting by design: a client that calls `rocketnet_write_dev_connect_token`
against it fails to link (on MSVC, `unresolved external symbol rocketnet_write_dev_connect_token`). For a first loopback try, embed the
checkout and link both the server and the client to the development library,
`rocketnet_dev`, which mints development tokens and accepts the published
development keys. It is a build-tree aid, never installed and never shipped:

```cmake
add_subdirectory(path/to/rocketnet rocketnet)
target_link_libraries(game_server PRIVATE rocketnet_dev)
target_link_libraries(game PRIVATE rocketnet_dev)
```

`python3 tools/check_package.py build --config Release --embedded` installs and
moves the package, then builds and runs separate C/C++ consumers against both
libraries. It also checks an embedded build with `BUILD_SHARED_LIBS=ON` and
verifies that the XDP archive contains the current sources. These checks are in CI.

## ABI compatibility

The current public layouts are preserved. New allocation-free `rocketnet_abi_*`
queries expose the SDK revision, compiler-derived struct sizes, field counts,
offsets and widths for both configs, packet context and all four stats structs.
They are safe before init and write no caller-owned struct. Unknown names return
`SIZE_MAX` for size/offset, or `-1` for count. Native dynamic loaders may use
these before calling struct-writing functions.

An incompatible layout or calling-contract change must increment
`ROCKETNET_ABI_VERSION` and update native ABI regression checks. Additive functions alone
do not require a revision change. Changes to public fields must also update the
ABI metadata and native regression checks. The existing unsized C entry points still
require matching headers and library; this does not make an old native binary
safe to use with an arbitrary newer library. Sized evolution APIs remain a
separate compatibility project.

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

`rocketnet_server_required_memory(config)` returns the bytes of the caller memory
block the server needs for `config` at its `max_clients`. It works before
`rocketnet_init`, opens no sockets, starts no workers and allocates no memory.
Pass `NULL` config for defaults. The query reads the supplied configuration only:
it does not apply environment overrides, check keys, probe hardware or approve a
configuration against the component budgets. `rocketnet_client_required_memory` is
the client form. The detailed per-transport plan and the kernel-facing host memory
estimate are internal to the transport layer (`sdk/rocketnet_layers.h`), not part
of the public client and server API.

The plan includes the transport, protocol tables and ACK history, reassembly and
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
rings libxdp sizes, libbpf/libxdp and the XDP program's maps, the io_uring
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
./bin/transport_memory 1000 1192 16 portable 0
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

## Server transports

Transport selection is runtime: one build carries every transport its
platform has — on Linux `xdp` (AF_XDP, privileged), `gso` (`SO_REUSEPORT` +
`sendmmsg`/`recvmmsg` + UDP GSO/GRO), `io_uring` (liburing multishot recv +
batched send), and `portable`; on macOS `msgx` (Darwin batch pump) and
`portable`; on Windows `iocp` (overlapped UDP with USO/URO), `rio` (Registered
I/O), and `portable`; plus the in-memory `loopback` transport (pin-only) on
every platform — and the endpoint walks an ordered candidate list at create. The list rides the config's transport field or the
per-endpoint `ROCKETNET_SERVER_TRANSPORT` and `ROCKETNET_CLIENT_TRANSPORT`
environment variables (precedence: default < struct < env). The old names
`ROCKETNET_DRIVER`, `ROCKETNET_TRANSPORT`, `ROCKETNET_SERVER_DRIVER` and
`ROCKETNET_CLIENT_DRIVER` are refused aloud and never applied:

    ./bin/functional                                # auto: the platform's recommended ladder
                                                #   (linux: xdp,gso,portable — falls on refusal)
    ROCKETNET_SERVER_TRANSPORT=portable ROCKETNET_CLIENT_TRANSPORT=portable ./bin/functional
                                                # a one-element list is a pin: that transport
                                                #   or loud failure, never a silent fall
    ROCKETNET_SERVER_TRANSPORT=gso,portable ./bin/functional  # exactly these, exactly this order

Names known but absent on a platform — or not applicable to an endpoint, like
`gso` for a client — are skipped with a log line, so one fleet-wide value deploys
everywhere; a list whose every name is skipped falls to that endpoint's own auto
order, loudly, rather than bricking an endpoint the value was not addressing.
Unknown names refuse loudly at create. The granted transport is reported by
the transport name query on each client and server, and the create log line. Queue-count bounds are per transport, refused loudly and never
clamped: the xdp transport takes any `num_xdp_queues` from 1, the gso transport refuses
past its 32-queue geometry. The GSO transport still probes UDP_SEGMENT and UDP_GRO at
create and degrades to plain sendmmsg/recvmmsg batching where a kernel lacks them —
that degradation is internal to the transport, below the ladder; likewise the Darwin
batch pump's live-refusal pause. The legacy `ROCKETNET_PUMP=portable` override is
absorbed into this system with its exact old meaning — the msgx pump off —
honored and named in the logs, with `ROCKETNET_SERVER_TRANSPORT=portable` and
`ROCKETNET_CLIENT_TRANSPORT=portable` as its successors;
`ROCKETNET_SERVER_NUM_QUEUES` and `ROCKETNET_SERVER_NUM_OS_CPUS` likewise keep
working as loudly-logged deprecated aliases of `ROCKETNET_NUM_XDP_QUEUES` and
`ROCKETNET_NUM_OS_CPUS`. Run `./bin/functional` against each rung. The unit
suite, `./bin/test`, does not start a transport, so a transport pin does not change
what it asserts.

The SDK test suite temporarily clears config overrides derived from both
config tables and their legacy aliases, then restores them on return.
`ROCKETNET_SERVER_TRANSPORT` and `ROCKETNET_CLIENT_TRANSPORT` remain intentional
test pins, and the old transport names are cleared; `ROCKETNET_TEST_VERBOSE`
keeps its diagnostic meaning. Passing ordinary runs print test names and
explicitly named platform skips. Failures retain the captured diagnostic log.

### Windows IOCP server

Native transport authors: [TRANSPORTS.md](TRANSPORTS.md) describes registration,
capabilities, ownership and the independently built extension example.

Select `transport = "iocp"` in the existing server config, or set
`ROCKETNET_SERVER_TRANSPORT=iocp`. The transport uses overlapped UDP with completion batches;
send buffers are recycled only after completion, and stop cancels and drains
outstanding operations. IPv6 binds accept both address families. The client
continues to use the portable path.

The existing `num_xdp_queues` field selects IOCP workers, following the GSO
transport's reuse of this field: zero selects four workers; 1 through 32 are valid.
There are 128 posted receives per worker, 1024 receive staging slots per worker
per buffer, and 16384 send frames across all workers. These are resource bounds,
not timing or packet-rate requirements. A send frame still owned by the OS is
unavailable to the next fill, even after an application flush.

IOCP is currently an explicit selection; Windows `auto` keeps portable first
while this new transport is reviewed. Windows CI runs both configurations, plus
completion-lifetime, allocation-failure and shutdown tests with IOCP selected.
Compilation and loopback correctness do not establish NIC
line-rate performance; that requires measurements on suitable Windows hardware.
The current implementation shares one UDP socket between completion workers.
Its receive-queue contention must be measured before choosing whether Windows
needs RSS-sharded sockets or a different native backend for the line-rate tier.

## Deploying the XDP transport: provision, then verify-and-bind

The library never mutates machine state. `rocketnet_server_create()`
with the xdp transport is verify-and-bind only: a non-mutating pre-flight checks
that the machine is provisioned and refuses by name — each unmet condition logs
the knob, the wanted value, and the script that sets it — and the library's own
acts stay per-socket (its BPF program built, loaded and attached, AF_XDP binds,
the memlock rlimit). Provisioning is `scripts/rocketnet-provision`, a shipped
deliverable: no application that uses
rocketnet with XDP needs to reconstruct the host setup by hand. The script's header carries the authoritative
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
the busy-poll pair — is the 10 Gbps reference configuration on a provisioned
Linux server; the magnitude belongs to that machine, and the claim attaches to a
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

Honest labels: the committing xdp path (BPF program build/load/attach, AF_XDP
binds) is exercised by no CI leg and no test — CI is unprivileged and the suite
installs a commit barrier by design — so that tier's claims rest on a provisioned
server, where this configuration was measured. The pre-flight
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
flag. The canary `test_fp_contract_off` in `test.cpp` asserts the floor holds on every platform
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
buffers were queued takes the publication mutex. Hardware throughput and
pressure measurements are required for this changed XDP path.
