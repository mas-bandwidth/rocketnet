# Building rocketnet

rocketnet builds on Linux, macOS and Windows with CMake. On Linux and macOS the
Makefile is the front door:

    make

Binaries and libraries land in `bin/`. Run the test suite with:

    ./bin/test

Dependencies, per platform, as CI installs them:

    # Linux -- the build links the XDP dataplane, and liburing is the bench
    # harness's io_uring rung
    sudo apt install cmake build-essential libbpf-dev libxdp-dev libsodium-dev liburing-dev xxd

    # macOS
    brew install libsodium pkgconf

Windows has no Makefile path; CMake is driven directly, with libsodium from
vcpkg, and the binaries land under `bin/<config>/`:

    cmake -B build -A x64 -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_INSTALLATION_ROOT/scripts/buildsystems/vcpkg.cmake"
    cmake --build build --config Release --parallel
    ./bin/Release/test.exe

The embedded XDP program (`server_xdp/`) is packed as a source tarball by the build and
compiles later on the target kernel, which needs BTF; it is not compiled by `make` here.

## Server transport drivers

Driver selection is runtime (#106): one build carries every driver its platform
has — on Linux the AF_XDP driver (privileged), the GSO driver (SO_REUSEPORT +
sendmmsg/recvmmsg + UDP GSO/GRO, non-root) and the portable UDP driver; on macOS
the Darwin batch pump and portable — and the transport walks an ordered candidate
list at create. The list rides the config's `driver` field or the
`ROCKETNET_DRIVER` environment variable (precedence: default < struct < env):

    ./bin/test                                # auto: the platform's recommended ladder
                                              #   (linux: xdp,gso,portable — falls on refusal)
    ROCKETNET_DRIVER=portable ./bin/test      # a one-element list is a pin: that driver
                                              #   or loud failure, never a silent fall
    ROCKETNET_DRIVER=gso,portable ./bin/test  # exactly these, exactly this order

Names known but absent on a platform — or not applicable to an endpoint, like
`gso` for a client — are skipped with a log line, so one fleet-wide value deploys
everywhere; a list whose every name is skipped falls to that endpoint's own auto
order, loudly, rather than bricking an endpoint the value was not addressing.
Unknown names refuse loudly at create. The granted driver is reported by
`rocketnet_server_transport_driver` / `rocketnet_client_transport_driver` and the
create log line. Queue-count bounds are per driver, refused loudly and never
clamped: the xdp driver takes any `num_xdp_queues` from 1, the gso driver refuses
past its 32-queue geometry. The GSO driver still probes UDP_SEGMENT and UDP_GRO at
create and degrades to plain sendmmsg/recvmmsg batching where a kernel lacks them —
that degradation is internal to the driver, below the ladder; likewise the Darwin
batch pump's live-refusal pause. The pre-#106 `ROCKETNET_PUMP=portable` override is
absorbed into this system with its exact old meaning — the msgx pump off —
honored and named in the logs, with `ROCKETNET_DRIVER=portable` as its successor;
`ROCKETNET_SERVER_NUM_QUEUES` and `ROCKETNET_SERVER_NUM_OS_CPUS` likewise keep
working as loudly-logged deprecated aliases of `ROCKETNET_NUM_XDP_QUEUES` and
`ROCKETNET_NUM_OS_CPUS`. Run the same `./bin/test` against each rung — the suite is
driver-blind.

## Deploying the XDP driver: provision, then verify-and-bind

The library never mutates machine state. `rocketnet_server_transport_create()`
with the xdp driver is verify-and-bind only: a non-mutating pre-flight checks
that the machine is provisioned and refuses by name — each unmet condition logs
the knob, the wanted value, and the script that sets it — and the library's own
acts stay per-socket (its BPF program built, loaded and attached, AF_XDP binds,
the memlock rlimit). Provisioning is `scripts/rocketnet-provision`, a shipped
deliverable: the setup was hand-run once for the space game and no game that uses
rocketnet with XDP reconstructs it. The script's header carries the authoritative
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
    #  case, paired in the fail-site enumeration receipt on PR #110, and
    #  the WHOLE SCRIPT is driven door-to-fail by a full-flow verify case
    #  (there is no level above the whole script). it
    #  needs no root and touches only a private temp dir it creates and
    #  removes. CI runs it on every push, so neither the matcher halves nor
    #  the enforcement paths can drift silently.)

This configuration — isolated cores 1-15, seven queues, per-queue IRQ pinning,
the busy-poll pair — is the 10 Gbps point, as measured on the space game's Linux
server; the magnitude belongs to that machine, and the claim attaches to a
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
installs a commit barrier by design — so that tier's claims rest on the space
game's provisioned server, where this configuration was measured. The pre-flight
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
flag.
