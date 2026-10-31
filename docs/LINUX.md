# Linux guide

The guide a Linux server operator reads first: how to build, which transport a
server gets on a given kernel and why, how to pin one, what `gso`, `io_uring`
and `xdp` need from the host, how socket errors and ICMP behave, the fd and
memlock limits for 1000 clients, what each transport needs inside a container,
and the commands that verify a host. Each claim names the file that backs it; find the code by the symbol or message
the text quotes.

## 1. Build

The build needs CMake 3.20 or later (`CMakeLists.txt`, `BUILDING.md`)
and, on Debian or Ubuntu, the packages CI installs: `cmake`, `build-essential`,
`clang`, `libbpf-dev`, `libxdp-dev`, `liburing-dev` and `xxd` (`BUILDING.md`, Dependencies).
`dpkg-query` in section 10 checks they are present.

XDP needs libbpf and an AF_XDP provider: libxdp on newer distributions,
or the AF_XDP APIs in stock libbpf 0.5 on Ubuntu 22.04. On 22.04 omit
`libxdp-dev` from the package command; configure reports
`XDP: using legacy AF_XDP APIs from libbpf (Ubuntu 22.04)`.
The installed package retains the provider used for its build.
liburing is optional; without suitable headers and the library, io_uring is
omitted from the available transports and its name is skipped in a list.
Jammy's liburing 2.1 also lacks the multishot receive headers needed by that
transport. Neither case prevents building XDP. See BUILDING.md, "Ubuntu
22.04 build compatibility", for build checks and the separate kernel/NIC
runtime qualification boundary.

The XDP program is compiled at build time, never on the server host.
`cmake/EmbedXdp.cmake` compiles `sdk/xdp/server_xdp.c` to `server_xdp.o`
with a clang that has the BPF target, against the kernel UAPI headers
(`linux-libc-dev`, which `build-essential` pulls in) and libbpf's
`bpf/bpf_helpers.h`, and embeds the object's bytes in the library;
`rocketnet_server_socket_create` opens them with `bpf_object__open_mem`. The
build is tolerant, for a deployment that never runs XDP (a host without root,
such as AWS GameLift): a configure that finds no such clang, or no libbpf or
AF_XDP provider, prints `XDP off: no clang with the BPF target was found, so XDP is
compiled out of this build` (or `XDP off: libbpf or AF_XDP headers/library were not found, so
XDP is compiled out of this build`) and compiles the whole XDP
transport out, with no embedded object, no XDP or netlink gateway code in the
library and no libbpf or libxdp link. `-DROCKETNET_XDP=OFF` compiles it out on
purpose. In that build a transport list naming only `xdp` refuses server start with
`ROCKETNET_STATUS_TRANSPORT_NOT_BUILT`, and `auto` falls to the next rung. The
server host needs no compiler, no `make`, no kernel headers and no temporary
directory to start `xdp`.

A Linux client build needs none of the server libraries. liburing, libbpf
and libxdp are server-only — the io_uring and XDP server transports link
them, and no client code does — so a machine building only a client needs
none of their development headers or libraries: configure with both
`-DROCKETNET_XDP=OFF` and `-DROCKETNET_IO_URING=OFF`, then build
`rocketnet` and `rocketnet_shared` and install as below. The shared library
carries no `liburing`, `libbpf` or `libxdp` link (`readelf -d` shows no such
`NEEDED` entry), and the installed CMake package requires none of the three,
so `find_package(RocketNet)` succeeds on a machine without them. CI builds
this configuration on Linux and runs a C client against both installed
libraries (`tools/check_package.py --client`); BUILDING.md, "Building a
Linux client without the server libraries", is the full paragraph.

`make` is the front door. It configures `build/` as **Release** with
`-DROCKETNET_DEV=ON` (`make ROCKETNET_DEV=OFF` builds the production library;
`BUILDING.md`, "Production builds"), shows the configure status lines for XDP
and `liburing`, and runs the child make with `-j` set to the processor count
capped at 8; `make JOBS=<n>` or the caller's own `-j` overrides it (`Makefile`).
CI's Release rows run `make` (`.github/workflows/ci.yml`, "Build (Release,
through the Makefile)"):

```sh
make JOBS=4
```

A Debug build is configured directly, as CI's Debug row does
(`.github/workflows/ci.yml`): `cmake -B build -DCMAKE_BUILD_TYPE=Debug`
then `cmake --build build --parallel 4`. Both write their executables and
shared libraries into `bin/` (`CMakeLists.txt`), and `make` reconfigures
`build/` to Release with `-DROCKETNET_DEV=ON` unless `ROCKETNET_DEV` is set
(`Makefile`), so use one or the other in a checkout.

The executables this guide runs: `./bin/test` (the unit suite, no sockets,
`BUILDING.md`), `./bin/functional` (real sockets and real transports,
`BUILDING.md`), `./bin/test_transport` (registry order and transport contract,
`TRANSPORTS.md`), `./bin/test_lifecycle`, `./bin/transport_memory`
(`BUILDING.md`) and `./bin/test_xdp_filter` (`TRANSPORTS.md`).

## 2. The transport ladder

`sdk/rocketnet_drivers.inc` is the registry. Each row carries a name, a
platform mask and a server and client preference rank; a lower nonzero rank
comes first in `auto`, zero means the endpoint does not have that transport
(`TRANSPORTS.md`, `sdk/rocketnet_driver.cpp`). The Linux rows:

| transport | server rank | client rank | row |
| --- | ---: | ---: | --- |
| `xdp` | 10 | 0 | `sdk/rocketnet_drivers.inc` |
| `gso` | 20 | 10 | `sdk/rocketnet_drivers.inc` |
| `io_uring` | 25 | 0 | `sdk/rocketnet_drivers.inc` |
| `portable` | development: 30; production: 50 | 30 | `sdk/rocketnet_drivers.inc` |
| `loopback` | pin only | pin only | `sdk/rocketnet_drivers.inc` |

The Linux server `auto` order in both builds is **xdp → gso → io_uring → portable**. A build without liburing omits
`io_uring` from the list. The portable client side remains available.
`loopback` is in-process, never chosen by `auto`, and selected only by name
(`TRANSPORTS.md`, `sdk/rocketnet_driver.h`). The Linux client `auto` order is
**gso → portable** (`sdk/rocketnet_drivers.inc`). The Linux portable transport
batches with `sendmmsg`/`recvmmsg` and waits on an eventfd beside its socket
(`sdk/rocketnet_driver_builtin.cpp`,
`sdk/rocketnet_platform.cpp`).

Because `gso` ranks ahead of `io_uring`, `auto` grants `io_uring` only on a
host where `gso` refuses. To run `io_uring`, pin it (section 3).

**How start walks it.** Public create parses the list; server start tries
candidates left to right and the first grant wins (`sdk/rocketnet_server_transport.cpp`). Each refusal logs a warning,
`server transport <name> refused. trying the next candidate`
(`sdk/rocketnet_server_transport.cpp`), and the transport logs why
before it (for example the `xdp pre-flight: ...` lines in section 6). A port-0
public address refuses `xdp`, which binds the port it is given and discovers
nothing (`sdk/rocketnet_drivers.inc` third column,
`sdk/rocketnet_server_transport.cpp`). An operator memory budget,
`ROCKETNET_SERVER_MEMORY_BUDGET_BYTES`, refuses any candidate whose plan
exceeds it (`sdk/rocketnet_server_transport.cpp`).

**How to read what was granted.** `rocketnet_server_transport_name` and
`rocketnet_client_transport_name` return the transport that won
(`sdk/rocketnet.h`). At INFO level the server logs
`server transport ladder granted <name> after refusals: <list>` when an
earlier candidate refused (`sdk/rocketnet_server_transport.cpp`) and
`server transport started on <address> [<name>] (<development|production> keys)`
(`sdk/rocketnet_server_transport.cpp`). A list with no grant ends in
the error
`server transport ladder exhausted with no grant. refused: <list>. skipped for platform: <list>`
(`sdk/rocketnet_server_transport.cpp`).

## 3. Pinning by environment

The config's `transport` field is the candidate list; `NULL`, empty or `auto`
is the platform order (`sdk/rocketnet.h`,
`sdk/rocketnet_config.cpp`). The environment overrides it per
endpoint: `ROCKETNET_SERVER_TRANSPORT` and `ROCKETNET_CLIENT_TRANSPORT`
(precedence default < struct < env, `BUILDING.md`;
`sdk/rocketnet_config.cpp`). The rules (`sdk/rocketnet_config.h`):

- A list with at least one name that is not skipped (below) is a strict pin:
  those transports in that order, or a loud failure, never a fall to `auto`.
- A server pin of `portable` is available in both development and production.
- A longer list is tried in exactly that order; nothing is appended.
- A known name this platform or endpoint does not have (`iocp` on Linux, `msgx`) is
  skipped with `transport <name> is not available here. skipped`
  (`sdk/rocketnet_config.cpp`); a list whose every name is skipped
  falls to the endpoint's own `auto` with a warning
  (`sdk/rocketnet_config.cpp`), so one fleet-wide line deploys
  unchanged to mixed machines.
- An unknown name, an empty element, or `auto` inside a list fails create
  (`sdk/rocketnet_config.cpp`).

The public header lists each field's environment overlay; callback and
context fields, `loopback_hub`, `server_secret_key`, and `config_bytes` have
none. Deployment controls include
`ROCKETNET_SERVER_NUM_XDP_QUEUES` and `ROCKETNET_SERVER_NUM_OS_CPUS`
(`sdk/rocketnet.h`, `sdk/rocketnet_internal.h`).

CI's development-build transport matrix pins each Linux server transport against the portable
client on four cases, `test_transport_env_pin_is_honoured` among them, which proves
the pinned transport is the one granted (`.github/workflows/ci.yml`). A local
server matrix, excluding privileged XDP (section 6), is below. Run the io_uring
cell only when that backend was built and its runtime probe can succeed; a
skipped unavailable name must not count as a successful pin.

```sh
(
  for pair in portable:portable gso:portable io_uring:portable loopback:loopback; do
    for c in test_loopback_transport_udp test_loopback_transport_ipv6 \
             test_transport_dual_stack_ipv4_client test_transport_env_pin_is_honoured; do
      ROCKETNET_SERVER_TRANSPORT=${pair%%:*} ROCKETNET_CLIENT_TRANSPORT=${pair##*:} \
        ./bin/functional --run "$c" || exit 1
    done
  done
)
```

## 4. gso

`gso` (`sdk/rocketnet_server_gso.h`, `sdk/rocketnet_server_gso_linux.cpp`)
needs no capability or privilege (`sdk/rocketnet_server_gso.h`). It
binds an `SO_REUSEPORT` group of `num_xdp_queues` sockets, default 4
(`sdk/rocketnet_server_gso.h`, `sdk/rocketnet_server_gso_linux.cpp`),
at most 32: a larger count refuses the rung, never clamps
(`sdk/rocketnet_server_gso.h`, `sdk/rocketnet_driver_builtin.cpp`).
Each socket has its own receive worker thread, and the server has one
transmit worker. Three server-wide send banks hold 4096 slots each, with
only the current producer bank available to a fill. Adding sockets does not
increase this fill capacity. See [staging and drain limitations](TRANSPORT-INTERNALS.md#current-staging-and-drain-limitations),
including the current fairness issue under send pressure
(`sdk/rocketnet_server_gso_linux.cpp`).

**The plain tier.** Create probes `UDP_SEGMENT` and `UDP_GRO` on the bound
sockets (`sdk/rocketnet_server_gso_linux.cpp`). A kernel that
refuses either is a tier, not an error: no `UDP_SEGMENT` means plain
`sendmmsg` batching, no `UDP_GRO` means plain `recvmmsg`
(`sdk/rocketnet_server_gso.h`). The create line names the tier:
`gso transport: <n> queue reuseport group on port <p>, UDP_SEGMENT yes|no (plain sendmmsg), UDP_GRO yes|no (plain recvmmsg)`
(`sdk/rocketnet_server_gso_linux.cpp`). A kernel that accepted the
probe but refuses a live GSO send (`EINVAL`, `EIO`, `EOPNOTSUPP`) logs
`gso transport: live GSO send refused (...). degrading to plain sendmmsg` and
resends that batch as plain packets (`sdk/rocketnet_server_gso_linux.cpp`).
The degradation is inside the transport, below the ladder (`BUILDING.md`).

## 5. io_uring

`io_uring` (`sdk/rocketnet_server_uring.h`, `sdk/rocketnet_server_uring_linux.cpp`)
uses one ring per server, touched only by the main thread, and no I/O worker
threads (`sdk/rocketnet_server_uring_linux.cpp`). It binds the same
`SO_REUSEPORT` group as `gso`: `num_xdp_queues` sockets, default 4, at most
32 (`sdk/rocketnet_server_uring.h`,
`sdk/rocketnet_server_uring_linux.cpp`,
`sdk/rocketnet_driver_builtin.cpp`). All sockets share one 2048-slot send
array; pending completions keep their slots occupied. More queues do not
increase that capacity. See [staging limitations](TRANSPORT-INTERNALS.md#current-staging-and-drain-limitations)
before sizing a large server.

**Receive.** Each socket has a provided buffer ring of 4096 buffers of 66 KiB
(`ROCKETNET_URING_RECV_BUFFER_BYTES`: one buffer holds a `UDP_GRO`-coalesced
receive of up to 64 KiB plus its headers, 264 MiB per queue, allocated at create)
and one multishot `RECVMSG` over it; the kernel fills ring buffers and
posts one completion per receive buffer with no syscall per datagram.
With GRO, that buffer can contain several datagrams; the main thread splits
and copies them before returning the buffer to the ring
(`sdk/rocketnet_server_uring.h`,
`sdk/rocketnet_server_uring_linux.cpp`). A multishot
that ends (for example `ENOBUFS` when the ring ran dry) is counted and armed
again (`sdk/rocketnet_server_uring_linux.cpp`,
`sdk/rocketnet_server_uring.h`). The ring has 4096 submission entries and
a completion queue twice that (`sdk/rocketnet_server_uring.h`,
`sdk/rocketnet_server_uring_linux.cpp`). **Send** submits one
`SENDMSG` per send run (one segmented `SENDMSG` per run of equal-sized packets to
one destination when `UDP_SEGMENT` is available, else one per packet) and reaps
within a fixed per-call completion budget (`ROCKETNET_URING_SEND_BUDGET`);
`UDP_SEGMENT` and `UDP_GRO` are probed at create as `gso` probes them. Sends still pending when the call returns
stay in the completion queue for a later call and are counted
`SEND_DEFERRED`, so a send flush never waits unboundedly
(`sdk/rocketnet_server_uring_linux.cpp`, `TRANSPORTS.md`).

**What the host needs.**

- liburing at build time (section 1). A build without it keeps the row with no
  platform, so the name is skipped (`TRANSPORTS.md`).
- A kernel that accepts the ring, `SENDMSG`, `RECVMSG`, `ASYNC_CANCEL` and a
  provided buffer ring. `rocketnet_server_uring_available()` probes this once
  (`sdk/rocketnet_server_uring_linux.cpp`); on no, create logs
  `io_uring transport refused: this kernel does not accept the io_uring ring, opcodes or provided buffer ring the transport needs`
  and the ladder falls (`sdk/rocketnet_server_uring_linux.cpp`).
  Multishot receive must be in the liburing headers the build used
  (`sdk/rocketnet_server_uring_linux.cpp`).
- Kernel policy that permits this process to create rings. Where
  `/proc/sys/kernel/io_uring_disabled` exists, 0 permits all users, 1 permits
  processes with `CAP_SYS_ADMIN` or membership in `io_uring_group`, and 2
  disables new rings. See the [kernel sysctl reference](https://docs.kernel.org/admin-guide/sysctl/kernel.html#io-uring-disabled).
  CI offers the io_uring cell only when the value is 0
  (`.github/workflows/ci.yml`); that CI filter is stricter than the general
  runtime permission rule. The transport does not read the sysctl; a refusing
  kernel shows up as the probe refusal above.
- Enough `RLIMIT_MEMLOCK` for the ring and the provided buffer rings, which
  depends on the kernel. The transport never raises the limit (no `setrlimit` in
  `sdk/rocketnet_server_uring_linux.cpp`) and refuses at create when the kernel
  will not grant the memory. Measured with
  `bash -c 'ulimit -l <KiB> && ./bin/transport_memory 4 1416 16 io_uring 0'`:
  on kernel 7.0 the default 4 queues refuse at 512 KiB and create at 768 KiB
  (in 16 KiB steps, 656 KiB on 7.0.0-38: 464 KiB for 1 queue, 528 for 2, 912
  for 8); on kernel 6.8 they create at 64 KiB (16 KiB on 6.8.0-139). These are
  single-server measurements. The probe uses 4 clients because the ring and the
  provided buffers are fixed per queue and do not grow with `max_clients`, and
  the transport's operator memory budget (section 10) refuses a 1000-client
  16-slot geometry before any ring is created (`tools/check_memlock.sh`).
  On kernel 7.0, io_uring charges locked pages across processes of the same
  user and checks that total against the creating process's limit
  ([kernel accounting](https://github.com/torvalds/linux/blob/v7.0/io_uring/rsrc.c#L36-L52)).
  A limit that works for one server can therefore refuse concurrent servers
  with `ENOMEM`; size it for the account's concurrent rings, with headroom.

The create line is
`io_uring transport: <n> queue reuseport group on port <p>, 4096 entry ring, 4096 receive buffers of 67584 bytes per queue, UDP_SEGMENT yes|no (one SENDMSG per packet), UDP_GRO yes|no (one datagram per receive buffer)`
(`sdk/rocketnet_server_uring_linux.cpp`); a kernel without
`IORING_FEAT_NODROP` is refused: `rocketnet_server_uring_create` logs
`io_uring transport: this kernel drops completions on overflow (no IORING_FEAT_NODROP)`,
destroys what it built and returns NULL, since a ring that drops
completions on overflow would lose receives and sends without a count.

## 6. xdp and the provisioning script

`xdp` (AF_XDP, `sdk/rocketnet_server_socket_linux.cpp`) is verify-and-bind
only: the library does not provision host settings, and a read-only pre-flight
refuses by name on any unmet condition (`BUILDING.md`,
`sdk/rocketnet_server_socket_linux.cpp`). Provisioning is
`scripts/rocketnet-provision`, whose header is the authoritative condition list
(`scripts/rocketnet-provision:18-23`):

| condition | what | script |
| --- | --- | --- |
| B1 | `/proc/cmdline` carries `irqaffinity=0 isolcpus=<range> nohz_full=<range>` (and `nosmt`); grub and reboot | `:27-37` |
| B2 | `/sys/devices/system/cpu/isolated` is non-empty | `:38-39` |
| B3 | a vendor NIC driver recent enough for AF_XDP zero copy (reported, not verified) | `:40-45` |
| R1 | euid 0 | `:49` |
| R2 | the interface exists and is UP; the pre-flight checks it carries the public address | `:50-53` |
| R3 | no XDP program already attached | `:54-64` |
| R4 | each queue IRQ pinned to its own isolated CPU | `:65-74` |
| R5 | `napi_defer_hard_irqs` 2, `gro_flush_timeout` 200000 | `:75-77` |
| R6 | SMT off | `:78-81` |
| R7 | the NIC's combined channel count equals the queue count | `:82-85` |
| K1 | the kernel grants an AF_XDP socket and a BPF program load (pre-flight only) | `:87-91` |

The script's modes are `provision` (the default), `verify`, `sweep` and
`selftest` (`scripts/rocketnet-provision:93-117`, parsed at `:125-146`). The
boot tier is verified, never performed (`scripts/rocketnet-provision:25`).
`BUILDING.md` walks a fresh box: boot tier, then
`scripts/rocketnet-provision eth0 7`, then `... verify`, then the server, and
`scripts/rocketnet-provision sweep` after a crash.

### Example machine layout: two XDP queues on 16 cores

Disable hyperthreading, reserve CPU 0 for OS housekeeping, dedicate two
physical cores to XDP, and pin the game threads to the remaining cores. On a
16-core machine the layout is:

| CPU | Work |
| --- | --- |
| 0 | OS housekeeping and non-game interrupts |
| 1 | XDP queue 0 worker and NIC queue 0 interrupt |
| 2 | XDP queue 1 worker and NIC queue 1 interrupt |
| 3–15 | Game main thread and worker pool |

**Disable hyperthreading and isolate the cores at boot.** This layout uses this
kernel command line addition:

```text
nosmt irqaffinity=0 isolcpus=1-15 nohz_full=1-15
```

Add it to `GRUB_CMDLINE_LINUX_DEFAULT` in `/etc/default/grub`, preserving
other required boot arguments, then run `sudo update-grub` and reboot.
Verify after reboot:

```sh
cat /proc/cmdline
cat /sys/devices/system/cpu/smt/control
cat /sys/devices/system/cpu/online
cat /sys/devices/system/cpu/isolated
lscpu -e=CPU,CORE,SOCKET,ONLINE
```

SMT must be off (or unsupported). The example assumes online CPU IDs 0–15
represent distinct physical cores; inspect the topology after `nosmt` and
adapt the ranges for a different machine. At least four physical cores are
needed for this one-OS/two-XDP layout with a game core remaining. The code's
CPU arithmetic assumes contiguous CPU IDs; sparse IDs need an explicit map.
OS housekeeping on CPU 0 does not mean all kernel work can be eliminated
from the application cores.

**Choose an i40e driver with AF_XDP zero-copy support.** An X710 NIC
uses `i40e`, but the separately installed Intel driver and the driver shipped
with Linux have different feature support. Intel's standalone 2.30.18 driver
[explicitly disables AF_XDP zero-copy on Ubuntu and kernels 5.2 or newer](https://github.com/intel/ethernet-linux-i40e/blob/v2.30.18/src/i40e.h#L48-L55).
A fork of that driver
contains the same restriction. Rebuilding that source unchanged does not
provide zero-copy on Ubuntu 22.04.

Start qualification with Ubuntu's supplied `i40e` module on the GA
`linux-generic` 5.15 track. The
[Linux 5.15 implementation includes AF_XDP zero-copy](https://github.com/torvalds/linux/blob/v5.15/drivers/net/ethernet/intel/i40e/i40e_xsk.c).
Verify `uname -r`, `ethtool -i <interface>`, and `modinfo -n i40e`; a separately
installed module under `updates/` may override the distribution module.
On kernels/tools exposing XDP feature queries, `sudo xdp-loader features
<interface>` must report `NETDEV_XDP_ACT_XSK_ZEROCOPY: yes`. This query is not
available on every older kernel, so its absence is not a capability verdict.
In all cases, require a successful forced zero-copy bind and the kernel's
`XDP_OPTIONS_ZEROCOPY` result on every queue. Native XDP attachment alone is
not zero-copy proof. Physical-NIC qualification on 5.15 remains necessary.

**Provision two NIC queues.** Before starting the server, run the existing
provisioner with the actual interface name (the examples use `eno1`):

```sh
sudo scripts/rocketnet-provision eno1 2
sudo scripts/rocketnet-provision eno1 2 verify
```

This performs the runtime steps in the condition table above, including
channel count, busy-poll settings and IRQ affinity. Prevent irqbalance from
moving the queue IRQs afterwards; on a dedicated host with irqbalance
installed, disable it with `sudo systemctl disable --now irqbalance` before
provisioning. Verify the queue mapping is **queue 0 → CPU 1, queue 1 → CPU 2**.
For i40e the queue IRQ names are `i40e-eno1-TxRx-0` and `i40e-eno1-TxRx-1` in
`/proc/interrupts`. After finding their IRQ numbers, the corresponding
`/proc/irq/<irq>/smp_affinity_list` and `effective_affinity_list` must read
`1` and `2`. If using `smp_affinity` instead, the hexadecimal masks are `2`
and `4`: mask `1` means CPU 0. Re-discover IRQ numbers after reboot, driver
reload or channel changes. Keep other configurable IRQs on CPU 0.

**Place the application's threads to match.** The constants of this layout are
`NumOsCpus=1`, `NumXdpQueues=2` and `NumXdpCpus=NumXdpQueues`. In the server
config set:

```c
server_config.num_os_cpus = 1;
server_config.num_xdp_queues = 2;
server_config.transport = "xdp";
```

RocketNet pins each XDP worker to `num_os_cpus + queue`, giving CPUs 1 and 2;
each SCHED_RR worker spins on its dedicated isolated CPU by design (NAPI runs
in softirq on the queue CPUs; the isolated-CPU preflight is the guard). The
game pins each of its workers from within that thread with
`pthread_setaffinity_np`, using
`NUM_OS_CPUS + NUM_XDP_CPUS + thread_index`, and may request `SCHED_FIFO` priority.
With a pool of 13 threads, including the main thread, with indices 0–12 mapping
to CPUs 3–15. The game owns its thread placement; RocketNet does not pin the
game's worker pool.

An integration must run its pinning callback on every actual worker and
check affinity/priority return values. Do not treat a completed batch of
ordinary scheduler tasks as proof of placement: such a batch does not
guarantee every worker receives a callback. Inspect every thread's
`Cpus_allowed_list` under `/proc/<pid>/task/<tid>/status` and queue IRQ
counters during traffic. Confirm AF_XDP zero-copy on both NIC queues;
startup and an `xdp` transport label alone do not establish zero-copy.

### XDP requirements and checks

- No compiler: the XDP program is compiled and embedded at build time
  (section 1), and the server loads it from memory.
- Root: the pre-flight refuses a non-root process with
  `xdp pre-flight: not root (R1 of scripts/rocketnet-provision)`
  (`sdk/rocketnet_server_socket_linux.cpp`), and any unmet condition
  ends in `xdp pre-flight failed: this machine is not provisioned for the XDP data plane`
  (`sdk/rocketnet_server_socket_linux.cpp`). On an unprivileged host
  `auto` therefore falls to `gso` (`.github/workflows/ci.yml`).
- A real port and a global address: port 0 refuses `xdp` (section 2), and an
  IPv6 link-local address is refused by name
  (`sdk/rocketnet_server_socket_linux.cpp`).
- Queues: `num_xdp_queues`, default 7; worker CPUs start at `num_os_cpus`,
  default 1 (`sdk/rocketnet_driver_builtin.cpp`, `BUILDING.md`).
- Memory: each queue's UMEM is 49152 frames of 4096 bytes, 192 MiB:
  32768 send frames plus 16384 receive frames
  (`sdk/rocketnet_constants.h`, `sdk/rocketnet_server_socket_linux.cpp`);
  seven queues are 1,409,286,144 bytes of UMEM, within the 4096 MiB
  `ROCKETNET_XDP_MEMORY_MIB` budget. UMEM is one part of the server's full
  memory plan; use [the memory planner](MEMORY.md) for the selected configuration.
- Both families on one interface: `rocketnet_server_create` names an IPv4
  and an IPv6 address on the same port, and `xdp` carries both destinations when both
  addresses are on that interface (`sdk/rocketnet_server_socket_linux.cpp`).
  Two interfaces refuse with `TRANSPORT_FAMILIES_UNSUPPORTED`
  (`sdk/rocketnet_server_socket_linux.cpp`). One interface takes one
  XDP program.

Checks that need no root, as CI runs them
(`.github/workflows/ci.yml`):

```sh
bash scripts/rocketnet-provision selftest
python3 tools/check_xdp_archive.py
```

CI's live AF_XDP tests run as root inside a virtme-ng guest of the host's own
kernel, without attaching to the runner's host NIC (`.github/workflows/ci.yml`;
`TRANSPORTS.md`). The guest needs `vng`, a readable
`~/vm-kernels/vmlinuz-$(uname -r)` and a writable `/dev/kvm`
(`.github/workflows/ci.yml`). The command is one quoted string so
older and newer `vng` releases pass it to the guest shell alike:

```sh
vng --run "$HOME/vm-kernels/vmlinuz-$(uname -r)" --memory 1536M --cpus 3 --append "irqaffinity=0 isolcpus=1-2 nohz_full=1-2" --rwdir "$PWD" -- "sudo ./bin/functional --run test_xdp_transport_max_size_both_ways_ipv4 && sudo ./bin/functional --run test_xdp_transport_max_size_both_ways_ipv6"
vng --run "$HOME/vm-kernels/vmlinuz-$(uname -r)" --memory 1536M --cpus 4 --append "irqaffinity=0 isolcpus=1-3 nohz_full=1-3" --rwdir "$PWD" -- "sudo ./bin/test_xdp_filter -v build/generated/server_xdp.o"
```

`test_xdp_filter` judges the BPF program with the kernel's verifier and
`BPF_PROG_TEST_RUN` (`TRANSPORTS.md`), on the same `server_xdp.o` the
library embeds. The teardown case creates a two-queue veth and sets
`num_os_cpus=2`, so its workers target CPUs 2 and 3. The on-link reply case
uses one queue and `num_os_cpus=1`, so its worker targets CPU 1. The filter
guest therefore boots four CPUs and isolates 1-3 to satisfy both cases. With
those boot-tier settings, a provisioned
XDP data plane, and no clang on `PATH`, all 18 cases print `ok`, including
`xdp_on_link_reply_reaches_the_client` and
`xdp_create_fails_part_way_tears_down`. The explicit object path makes the
command work when the checkout's build directory differs from the absolute
path embedded in `server_xdp.o`. If the child server reports
`xdp pre-flight failed: this machine is not provisioned for the XDP data plane`,
check the guest's remaining conditions with section 6 and provision any missing
runtime settings with `sudo scripts/rocketnet-provision <interface> 2`, followed
by `sudo scripts/rocketnet-provision <interface> 2 verify`. Missing boot-tier
settings require a reboot before retrying.

The nightly 1,000-client xdp load cell needs a far larger guest: the server
and its 1,000 default-profile clients share one process, and the plan alone is
16.96 GiB and the measured peak RSS is 26.1 GiB. The step sizes the guest from
`LOAD_PLAN_ONLY=1 ./bin/load 1000 60` plus a measured per-client run reserve,
as `(plan + 1000 * 9848630) * 5 / 4 + 1 GiB` (34G today), so the runner needs
that much `MemAvailable`; a 16G guest is OOM-killed and a 24G guest is killed at the
shutdown wipe (`docs/LOAD.md`, "The nightly AF_XDP cell's guest memory").

## 7. Socket errors and ICMP

Linux reports an ICMP error (port unreachable, host unreachable) to a UDP
socket only when the socket is connected or has `IP_RECVERR` set (Linux
`net/ipv4/udp.c` and `net/ipv6/udp.c`, `__udp4_lib_err` and
`__udp6_lib_err`). rocketnet's server sockets are never connected and never
set `IP_RECVERR`: they are created at `sdk/rocketnet_platform.cpp`,
`sdk/rocketnet_server_gso_linux.cpp` and
`sdk/rocketnet_server_uring_linux.cpp`, and send with `sendto`,
`sendmmsg` or `SENDMSG` to an address per packet. So a dead or unreachable
client produces no error on a server's receive path on `portable`, `gso` or
`io_uring`. A client's portable socket is connected to its one server
(`rocketnet_platform_socket_connect`, standard/TRANSPORT.md t11: the client trusts only
the server), so the kernel drops datagrams from any other source and does
report an ICMP error for the server to the client's next receive; the
portable receive arms skip it as weather (`rocketnet_platform_receive_weather`)
and keep draining. On `xdp`, ICMP is not UDP, so the XDP program passes it to the
kernel stack (`TRANSPORTS.md`). This command shows the kernel rule on
the host:

```sh
python3 -c 'import socket,time
c=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); c.bind(("127.0.0.1",0)); port=c.getsockname()[1]; c.close()
for connected in (False,True):
    s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); s.bind(("127.0.0.1",0))
    (s.connect(("127.0.0.1",port)),s.send(b"x")) if connected else s.sendto(b"x",("127.0.0.1",port))
    time.sleep(0.1); s.setblocking(False)
    try: s.recvfrom(64); r="datagram"
    except BlockingIOError: r="no error reported"
    except ConnectionRefusedError: r="ECONNREFUSED"
    print("connected" if connected else "unconnected", r)'
```

It prints `unconnected no error reported` then `connected ECONNREFUSED`.

What each path does with the errors it can see:

- **portable** (server and client): a failed `recvfrom` or `recvmmsg` ends that
  drain and is logged at debug, except `EAGAIN` (the queue is empty) and the
  weather errnos `ECONNREFUSED`, `EHOSTUNREACH`, `ENETUNREACH`, `EHOSTDOWN`,
  `ENETDOWN`, `ENOBUFS` and `EINTR`, which are skipped
  (`sdk/rocketnet_platform.cpp`); a failed `sendto` is
  logged at debug and the packet is dropped; a `sendmmsg` refused
  with `EAGAIN`, `EWOULDBLOCK` or `ENOBUFS` drops the rest of the batch, and
  any other errno drops the one refused entry and continues with the next.
- **gso**: a receive worker's `recvmmsg` returning nothing or a non-`EINTR`
  error ends that drain; `EINTR` retries after checking the stop request
  (`sdk/rocketnet_server_gso_linux.cpp`). Every message a drain reads,
  accepted or dropped, and every interrupted call spends one unit of a read
  budget of `ROCKETNET_GSO_RECV_QUEUE_SIZE`; a spent budget ends the drain, so
  the worker publishes and tests its stop request however much the socket
  holds. A `sendmmsg` refused
  with `EAGAIN`, `EWOULDBLOCK`, `ENOBUFS`, `ENOMEM` or `EINTR` (a full
  socket) drops nothing: the TX worker keeps the bank's unsent rest, in
  order and ahead of every later bank, and sends it when main's next update
  wakes it (`ROCKETNET_GSO_SEND_RETRY_SECONDS` once main stops updating).
  Any other errno drops the one refused message and
  continues with the next, as the portable batch does; dropped packets
  count in `ROCKETNET_SERVER_GSO_COUNTER_SEND_DROPPED`, never resent
  (`sdk/rocketnet_server_gso_linux.cpp`,
  `sdk/rocketnet_server_gso.h`, `proofs/tla/RNGsoHandoff.tla` SentOrHeld).
- **io_uring**: a send completion with a negative result counts in
  `ROCKETNET_SERVER_URING_COUNTER_SEND_DROPPED`
  (`sdk/rocketnet_server_uring_linux.cpp`,
  `sdk/rocketnet_server_uring.h`); a multishot receive that ends is armed
  again (section 5); a submit error other than `EINTR`, `EAGAIN` or `EBUSY` is
  logged (`sdk/rocketnet_server_uring_linux.cpp`).

`tests/functional/posix_hard_killed_client.cpp` and `tests/functional/posix_socket_errors.cpp`
cover a hard-killed client and the socket error paths per transport cell.
Repeated empty or rejected datagrams are a separate case: some current
POSIX receive loops can continue draining them without a per-pass bound,
delaying publication and shutdown. See [current drain limitations](TRANSPORT-INTERNALS.md#current-staging-and-drain-limitations).

## 8. Limits for 1000 clients

`ROCKETNET_MAX_CLIENTS` defaults to 1000 (`sdk/rocketnet.h`), and the server's
runtime `max_clients` defaults to the smaller of 1000 and the build ceiling
(`sdk/rocketnet_internal.h`). [Memory and capacity](MEMORY.md#maximum-clients)
describes larger builds and their qualification limits.

**File descriptors.** A server opens a fixed set of sockets, independent of
`max_clients`: the plan's `sockets` field (`benchmarks/transport_memory.cpp`).
For 1000 clients: `portable` 1, `gso` 4, `io_uring` 4 plus its ring, `xdp` 7
(the plan output in section 10). The default soft limit of 1024 is ample; the
section 10 commands create 1000-client `gso` and four-queue `io_uring` servers
under `ulimit -n 64`. A client opens one UDP socket and, on Linux, one eventfd
(`sdk/rocketnet_driver_builtin.cpp`,
`sdk/rocketnet_platform.cpp`), so a process that runs N clients needs about
2N descriptors: raise `ulimit -n` past 2000 for 1000 clients in one process.

**Locked memory.** `portable` and `gso` need no memlock headroom: the section
10 commands create 1000-client servers of each under `ulimit -l 64`.
`io_uring` needs some, kernel-dependent (section 5). An 8192 KiB limit covers
the single-server measurements in section 10, but is not a concurrency budget.
Check the running process's soft and hard limits in `/proc/<pid>/limits`, and
budget for all io_uring servers running under that user.

**The functional suite and CI runners.** Measured on kernel 7.0.0-38 with `ulimit -l` steps: a single io_uring case passes
alone at 464 to 656 KiB, and a create and destroy loop needs more, since a
destroyed ring's pages are released by kernel work after the loop moves on
(`test_io_uring_transport_posix_server_create_destroy_1000` at 2064 KiB,
`test_io_uring_transport_posix_server_restart_same_port_100` at 2048 KiB). At
an 8192 KiB limit, 26 io_uring cases started at once (each of 13
cases twice, `./bin/functional --run <case>`) refused 5 at create with
`Cannot allocate memory`. So `./bin/functional` bounds the io_uring cases it
runs at once to `floor(RLIMIT_MEMLOCK soft limit / per-case need)`, capped at
its one job, where the per-case need is 4096 KiB on kernels after 6.8 and
128 KiB on 6.8 and older (`tests/suite_runner.h`,
`ROCKETNET_TEST_URING_CASE_MEMLOCK_KIB` overrides it); a limit under one case
fails the suite before any case runs, naming the limit. A suite cannot see the
other processes of its user, so every CI runner of the user on a host shares
the budget: `tools/check_memlock.sh`, which CI runs before the functional
suite on the Linux legs, prints the limit, the per-server need on this kernel
and PASS when the limit is at least the user's runners times the per-case
need, else FAIL. **A runner limit of 1048576 KiB (`LimitMEMLOCK=1G`)** suits a
host whose kernel is after 6.8; a host already higher keeps its limit. The same 26 cases at once,
with other work of that user on the host, refused 5 at 8192 KiB
in each of two runs, 6 at 65536 KiB in one of five runs, and none at
1048576 KiB in three runs (the only failures there were
the two copies of `test_io_uring_transport_server_probe` taking one port).
The kernel's count is invisible from user space, so the limit is sized well
past the formula. Set it on the runner services and on the user manager that
starts them, since a user service cannot exceed its manager's hard limit:

```
# /etc/systemd/system/user@<uid>.service.d/memlock.conf
[Service]
LimitMEMLOCK=1G

# ~/.config/systemd/user/<runner>-.service.d/memlock.conf
# (a prefix drop-in: every <runner>-*.service of that user)
[Service]
LimitMEMLOCK=1G
```

then `systemctl daemon-reload`, `systemctl --user daemon-reload`, and in a
maintenance window `systemctl restart user@<uid>.service` (it restarts the
runners) and confirm `Max locked memory` in `/proc/<runner pid>/limits`. For systemd services,
`LimitMEMLOCK=` controls this limit. A user service cannot raise its hard limit
above the user manager's; configure `user@<uid>.service` when that inherited
limit is too small. See [systemd's resource-limit documentation](https://www.freedesktop.org/software/systemd/man/systemd.exec.html#Process%20Properties).
Apply a user-manager restart only during a maintenance window: it stops the
user's other services too. Preserve an existing higher limit.
`xdp` raises its own `RLIMIT_MEMLOCK` to unlimited at create and refuses with
`server could not setrlimit` if it cannot (`BUILDING.md`,
`sdk/rocketnet_server_socket_linux.cpp`); as root on a host that
succeeds.

**Memory.** `./bin/transport_memory <clients> <payload> <slots> <transport> <queues> --plan-only`
prints the plan without opening a socket; without `--plan-only` it creates a
loopback server and checks the allocator against the plan
(`benchmarks/transport_memory.cpp`, `BUILDING.md`). At 1000 clients,
1416-byte payloads and 16 reassembly slots the plans are 483,668,499 bytes
(`portable`), 625,876,730 (`gso`), 594,710,098 (`io_uring`) and 4,269,352,940
(`xdp`, 7 queues, of which 1,879,048,192 UMEM). This entire numeric set is
historical: it predates the ACK-stats delta/history allocations, and the
portable figure also predates the portable TX worker and its banks. Preserve
these values as historical measurements only; use the exact-build probe below
for current capacity planning. The plan excludes OS stacks,
kernel queues, loader and application memory
(`sdk/rocketnet_server_transport.cpp`, `BUILDING.md`).

## 9. Containers

What each transport needs, from the requirements above:

- **portable** and **gso**: nothing beyond a UDP socket; no capability
  (`sdk/rocketnet_server_gso.h`). An unprivileged container's `auto`
  grants `gso`, since `xdp` refuses for want of root (section 6).
- **io_uring**: kernel policy that permits the process to create rings
  (section 5), and a seccomp profile that allows the `io_uring_setup`, `io_uring_enter` and `io_uring_register`
  calls liburing makes. A refusing profile makes the probe fail and the
  ladder falls (section 5). Pin it, since `auto` grants `gso` first.
- **xdp**: euid 0 in the container (R1), the kernel's grant of an AF_XDP
  socket and a BPF program load (K1), permission to raise `RLIMIT_MEMLOCK`
  to unlimited (section 8), the NIC itself in the container's network
  namespace, and bpffs at `/sys/fs/bpf` for the instance's pin root
  (`BUILDING.md`). The boot tier belongs to the host. Where bpffs is
  shared across pid namespaces, run `sweep` from the namespace that owns the
  servers (`BUILDING.md`, `scripts/rocketnet-provision:60-64`).

No CI leg runs a transport inside a container; these needs are read from the
code and not measured.

## 10. Verify a host

From the checkout, in order. The `vng` lines need the guest tier of
section 6.

```sh
uname -sr
nproc
cat /proc/sys/kernel/io_uring_disabled
ulimit -n
ulimit -l
dpkg-query -W cmake build-essential clang libbpf-dev libxdp-dev liburing-dev xxd
make JOBS=4
./bin/test
./bin/test_transport
./bin/test_lifecycle
./bin/functional
python3 tools/check_memory.py ./bin/transport_memory
./bin/transport_memory 1000 1416 1 portable 0 --plan-only
./bin/transport_memory 1000 1416 1 gso 0 --plan-only
./bin/transport_memory 1000 1416 1 io_uring 4 --plan-only
./bin/transport_memory 1000 1416 1 xdp 7 --plan-only
bash -c 'ulimit -n 64 && ulimit -l 64 && ./bin/transport_memory 1000 1416 1 gso 0'
bash -c 'ulimit -n 64 && ./bin/transport_memory 1000 1416 1 io_uring 4'
```

`./bin/functional` with no pin runs over the `auto` ladder, which on an
unprivileged host is `gso` (`.github/workflows/ci.yml`).
Then the section 3 matrix loop, the section 6 no-root checks and `vng` lines,
and the section 7 ICMP check.
