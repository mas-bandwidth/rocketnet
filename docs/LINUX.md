# Linux guide

The guide a Linux server operator reads first: how to build, which transport a
server gets on a given kernel and why, how to pin one, what `gso`, `io_uring`
and `xdp` need from the host, how socket errors and ICMP behave, the fd and
memlock limits for 1000 clients, what each transport needs inside a container,
and the commands that verify a host. Each claim names the file that backs it.
Line numbers can drift as the code changes. A section whose executable proof is unavailable says **pending**.

## 1. Build

The build needs CMake 3.20 or later (`CMakeLists.txt:1`, `BUILDING.md:13-15`)
and, on Debian or Ubuntu, the packages CI installs: `cmake`, `build-essential`,
`clang`, `libbpf-dev`, `libxdp-dev`, `liburing-dev` and `xxd` (`BUILDING.md`, Dependencies).
`dpkg-query` in section 10 checks they are present.

libbpf and libxdp are required: configure stops without them
(`cmake/RocketNetDependencies.cmake:19-31`). liburing is optional: without it
the `io_uring` transport is built to refuse and configure prints
`liburing not found: the io_uring server transport is built to refuse`
(`cmake/RocketNetDependencies.cmake:32-50`, `CMakeLists.txt:22`). The libbpf
floor is 1.0 (`BUILDING.md:479-482`).

The XDP program is compiled at build time, never on the server host.
`cmake/EmbedXdp.cmake` compiles `server_xdp/server_xdp.c` to `server_xdp.o`
with a clang that has the BPF target, against the kernel UAPI headers
(`linux-libc-dev`, which `build-essential` pulls in) and libbpf's
`bpf/bpf_helpers.h`, and embeds the object's bytes in the library;
`rocketnet_server_socket_create` opens them with `bpf_object__open_mem`. The
build is tolerant, for a deployment that never runs XDP (a host without root,
such as AWS GameLift): a configure that finds no such clang, or no libbpf or
libxdp, prints `XDP compiled out: no clang with the BPF target was found` (or
`XDP compiled out: libbpf or libxdp was not found`) and compiles the whole XDP
transport out, with no embedded object, no XDP or netlink gateway code in the
library and no libbpf or libxdp link. `-DROCKETNET_XDP=OFF` compiles it out on
purpose. In that build a transport list naming `xdp` refuses it at create with
`ROCKETNET_STATUS_TRANSPORT_NOT_BUILT`, and `auto` falls to the next rung. The
server host needs no compiler, no `make`, no kernel headers and no temporary
directory to start `xdp`.

`make` is the front door. It configures `build/` as **Release** and runs the
child make with `-j` set to the processor count capped at 8; `make JOBS=<n>`
or the caller's own `-j` overrides it (`Makefile:1-6`). CI's Release rows run
`make` (`.github/workflows/ci.yml`, "Build (Release, through the Makefile)"):

```sh
make JOBS=4
```

A Debug build is configured directly, as CI's Debug row does
(`.github/workflows/ci.yml:201-205`): `cmake -B build -DCMAKE_BUILD_TYPE=Debug`
then `cmake --build build --parallel 4`. Both write their executables and
shared libraries into `bin/` (`CMakeLists.txt:34-38`), and `make` reconfigures
`build/` to Release (`Makefile:6`), so use one or the other in a checkout.

The executables this guide runs: `./bin/test` (the unit suite, no sockets,
`BUILDING.md:23-34`), `./bin/functional` (real sockets and real transports,
`BUILDING.md:36-48`), `./bin/test_transport` (registry order and transport contract,
`TRANSPORTS.md:145-151`), `./bin/test_lifecycle`, `./bin/transport_memory`
(`BUILDING.md:284-303`) and `./bin/test_xdp_filter` (`TRANSPORTS.md:256-270`).

## 2. The transport ladder

`sdk/rocketnet_drivers.inc:5-12` is the registry. Each row carries a name, a
platform mask and a server and client preference rank; a lower nonzero rank
comes first in `auto`, zero means the endpoint does not have that transport
(`TRANSPORTS.md:7-11`, `sdk/rocketnet_driver.cpp:84-100`). The Linux rows:

| transport | server rank | client rank | row |
| --- | ---: | ---: | --- |
| `xdp` | 10 | 0 | `sdk/rocketnet_drivers.inc:7` |
| `gso` | 20 | 0 | `sdk/rocketnet_drivers.inc:6` |
| `io_uring` | 25 | 0 | `sdk/rocketnet_drivers.inc:11` |
| `portable` | 30 | 30 | `sdk/rocketnet_drivers.inc:5` |
| `loopback` | pin only | pin only | `sdk/rocketnet_drivers.inc:10` |

So the Linux server `auto` order is **xdp → gso → io_uring → portable**
(`TRANSPORTS.md:188-189`), and `./bin/test_transport` fails if it is anything else
(`test_driver.cpp`). A build without liburing has no `io_uring` in the
list: xdp → gso → portable (`test_driver.cpp`, `TRANSPORTS.md:183-186`).
`loopback` is in-process, never chosen by `auto`, and selected only by name
(`TRANSPORTS.md:18-23`, `sdk/rocketnet_driver.h:13`). The Linux client has one
transport in `auto`, `portable` (`test_driver.cpp`); on Linux it
batches with `sendmmsg`/`recvmmsg` and waits on an eventfd beside its socket
(`sdk/rocketnet_driver_builtin.cpp:54`, `:87-100`,
`sdk/rocketnet_platform.cpp:645-668`).

Because `gso` ranks ahead of `io_uring`, `auto` grants `io_uring` only on a
host where `gso` refuses. To run `io_uring`, pin it (section 3).

**How create walks it.** Candidates are tried left to right and the first
grant wins (`sdk/rocketnet_server_transport.cpp:1182-1186`,
`:1209-1300`). Each refusal logs a warning,
`server transport <name> refused. trying the next candidate`
(`sdk/rocketnet_server_transport.cpp:1283-1284`), and the transport logs why
before it (for example the `xdp pre-flight: ...` lines in section 6). A port-0
public address refuses `xdp`, which binds the port it is given and discovers
nothing (`sdk/rocketnet_drivers.inc:7` third column,
`sdk/rocketnet_server_transport.cpp:1245-1270`). An operator memory budget,
`ROCKETNET_SERVER_MEMORY_BUDGET_BYTES`, refuses any candidate whose plan
exceeds it (`sdk/rocketnet_server_transport.cpp:1092-1141`).

**How to read what was granted.** `rocketnet_server_transport_name` and
`rocketnet_client_transport_name` return the transport that won
(`sdk/rocketnet.h:731-732`, `:1077`, `:1230`). At INFO level the server logs
`server transport ladder granted <name> after refusals: <list>` when an
earlier candidate refused (`sdk/rocketnet_server_transport.cpp:1344`) and
`server transport started on <address> [<name>] (<development|production> keys)`
(`sdk/rocketnet_server_transport.cpp:1352-1353`). A list with no grant ends in
the error
`server transport ladder exhausted with no grant. refused: <list>. skipped for platform: <list>`
(`sdk/rocketnet_server_transport.cpp:1312-1314`).

## 3. Pinning by environment

The config's `transport` field is the candidate list; `NULL`, empty or `auto`
is the platform order (`sdk/rocketnet.h:752`, `:811`,
`sdk/rocketnet_config.cpp:840-842`). The environment overrides it per
endpoint: `ROCKETNET_SERVER_TRANSPORT` and `ROCKETNET_CLIENT_TRANSPORT`
(precedence default < struct < env, `BUILDING.md:330-332`;
`sdk/rocketnet_config.cpp:313-319`). The rules (`sdk/rocketnet_config.h:165-176`):

- A one-element list is a pin: that transport or a loud failure, never a fall.
- A longer list is tried in exactly that order; nothing is appended.
- A known name this platform or endpoint does not have (`gso` for a client) is
  skipped with `transport <name> is not available here. skipped`
  (`sdk/rocketnet_config.cpp:899-913`); a list whose every name is skipped
  falls to the endpoint's own `auto` with a warning
  (`sdk/rocketnet_config.cpp:930-941`).
- An unknown name, an empty element, or `auto` inside a list fails create
  (`sdk/rocketnet_config.cpp:861-878`).

Every other server config field has the overlay `ROCKETNET_SERVER_<FIELD>`
(`sdk/rocketnet_config.cpp:344-375`), for example
`ROCKETNET_SERVER_NUM_XDP_QUEUES` and `ROCKETNET_SERVER_NUM_OS_CPUS`
(`sdk/rocketnet.h:805`, `sdk/rocketnet_internal.h:480-481`).

CI's transport matrix pins each Linux server transport against the portable
client on four cases, `test_transport_env_pin_is_honoured` among them, which proves
the pinned transport is the one granted (`.github/workflows/ci.yml:280-305`,
`:321`, `:358`). The same matrix, `xdp` aside (section 6):

```sh
for pair in "portable portable" "gso portable" "io_uring portable" "loopback loopback"; do set -- $pair; for c in test_loopback_transport_udp test_loopback_transport_ipv6 test_transport_dual_stack_ipv4_client test_transport_env_pin_is_honoured; do ROCKETNET_SERVER_TRANSPORT=$1 ROCKETNET_CLIENT_TRANSPORT=$2 ./bin/functional --run $c >/dev/null || { echo "FAIL $1/$2 $c"; break 2; }; echo "ok $1/$2 $c"; done; done
```

## 4. gso

`gso` (`sdk/rocketnet_server_gso.h`, `sdk/rocketnet_server_gso_linux.cpp`)
needs no capability or privilege (`sdk/rocketnet_server_gso.h:26-29`). It
binds an `SO_REUSEPORT` group of `num_xdp_queues` sockets, default 4
(`sdk/rocketnet_server_gso.h:41`, `sdk/rocketnet_server_gso_linux.cpp:558-559`),
at most 32: a larger count refuses the rung, never clamps
(`sdk/rocketnet_server_gso.h:44`, `sdk/rocketnet_driver_builtin.cpp:154-158`).
Each socket has its own receive worker thread
(`sdk/rocketnet_server_gso_linux.cpp:679-680`).

**The plain tier.** Create probes `UDP_SEGMENT` and `UDP_GRO` on the bound
sockets (`sdk/rocketnet_server_gso_linux.cpp:639`, `:654`). A kernel that
refuses either is a tier, not an error: no `UDP_SEGMENT` means plain
`sendmmsg` batching, no `UDP_GRO` means plain `recvmmsg`
(`sdk/rocketnet_server_gso.h:26-28`). The create line names the tier:
`gso transport: <n> queue reuseport group on port <p>, UDP_SEGMENT yes|no (plain sendmmsg), UDP_GRO yes|no (plain recvmmsg)`
(`sdk/rocketnet_server_gso_linux.cpp:674-677`). A kernel that accepted the
probe but refuses a live GSO send (`EINVAL`, `EIO`, `EOPNOTSUPP`) logs
`gso transport: live GSO send refused (...). degrading to plain sendmmsg` and
resends that batch as plain packets (`sdk/rocketnet_server_gso_linux.cpp:1024-1033`).
The degradation is inside the transport, below the ladder (`BUILDING.md:350-353`).

## 5. io_uring

`io_uring` (`sdk/rocketnet_server_uring.h`, `sdk/rocketnet_server_uring_linux.cpp`)
uses one ring per server, touched only by the main thread, and no worker
threads (`sdk/rocketnet_server_uring_linux.cpp:19-20`). It binds the same
`SO_REUSEPORT` group as `gso`: `num_xdp_queues` sockets, default 4, at most
32 (`sdk/rocketnet_server_uring.h:33-37`,
`sdk/rocketnet_server_uring_linux.cpp:667-668`,
`sdk/rocketnet_driver_builtin.cpp:175-180`).

**Receive.** Each socket has a provided buffer ring of 4096 buffers of 2048
bytes and one multishot `RECVMSG` over it; the kernel fills ring buffers and
posts one completion per datagram with no syscall per datagram, and the main
thread copies each datagram out and returns the buffer to the ring
(`sdk/rocketnet_server_uring.h:48-53`,
`sdk/rocketnet_server_uring_linux.cpp:20-26`, `:751-778`, `:459`). A multishot
that ends (for example `ENOBUFS` when the ring ran dry) is counted and armed
again (`sdk/rocketnet_server_uring_linux.cpp:570-585`,
`sdk/rocketnet_server_uring.h:65-66`). The ring has 4096 submission entries and
a completion queue twice that (`sdk/rocketnet_server_uring.h:39-41`,
`sdk/rocketnet_server_uring_linux.cpp:731-742`). **Send** submits one batched
`SENDMSG` per staged slot and reaps within a fixed per-call completion budget
(`ROCKETNET_URING_SEND_BUDGET`), waiting at most one bounded
`ROCKETNET_URING_SEND_WAIT_SECONDS`; sends still pending when the call returns
stay in the completion queue for a later call and are counted
`SEND_DEFERRED`, so a send flush never waits unboundedly
(`sdk/rocketnet_server_uring_linux.cpp:28-33`, `TRANSPORTS.md:176-179`).

**What the host needs.**

- liburing at build time (section 1). A build without it keeps the row with no
  platform, so the name is skipped (`TRANSPORTS.md:183-186`).
- A kernel that accepts the ring, `SENDMSG`, `RECVMSG`, `ASYNC_CANCEL` and a
  provided buffer ring. `rocketnet_server_uring_available()` probes this once
  (`sdk/rocketnet_server_uring_linux.cpp:383-421`); on no, create logs
  `io_uring transport refused: this kernel does not accept the io_uring ring, opcodes or provided buffer ring the transport needs`
  and the ladder falls (`sdk/rocketnet_server_uring_linux.cpp:679-683`).
  Multishot receive must be in the liburing headers the build used
  (`sdk/rocketnet_server_uring_linux.cpp:38-40`).
- `/proc/sys/kernel/io_uring_disabled` at 0. The kernel's own documentation
  defines 0 as enabled for all, 1 as limited to privileged processes and the
  `io_uring_group`, 2 as disabled (Linux
  `Documentation/admin-guide/sysctl/kernel.rst`, `io_uring_disabled`). CI
  offers the `io_uring` cell only when the value is 0
  (`.github/workflows/ci.yml:324-333`). The transport does not read the sysctl;
  a refusing kernel shows up as the probe refusal above.
- Enough `RLIMIT_MEMLOCK` for the ring and the provided buffer rings, which
  depends on the kernel. The transport never raises the limit (no `setrlimit` in
  `sdk/rocketnet_server_uring_linux.cpp`) and refuses at create when the kernel
  will not grant the memory. Measured with
  `bash -c 'ulimit -l <KiB> && ./bin/transport_memory 1000 1416 16 io_uring 0'`:
  on kernel 7.0 the default 4 queues refuse at 512 KiB and create at 768 KiB;
  on kernel 6.8 they create at 64 KiB. The usual default soft limit of
  8192 KiB covers 32 queues on 7.0 (section 10).

The create line is
`io_uring transport: <n> queue reuseport group on port <p>, 4096 entry ring, 4096 receive buffers per queue`
(`sdk/rocketnet_server_uring_linux.cpp:782-783`); a kernel without
`IORING_FEAT_NODROP` is refused: `rocketnet_server_uring_create` logs an
error, destroys what it built and returns NULL, since a ring that drops
completions on overflow would lose receives and sends without a count.

## 6. xdp and the provisioning script

`xdp` (AF_XDP, `sdk/rocketnet_server_socket_linux.cpp`) is verify-and-bind
only: the library never changes machine state, and a read-only pre-flight
refuses by name on any unmet condition (`BUILDING.md:403-411`,
`sdk/rocketnet_server_socket_linux.cpp:831-838`). Provisioning is
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
`BUILDING.md:413-442` walks a fresh box: boot tier, then
`scripts/rocketnet-provision eth0 7`, then `... verify`, then the server, and
`scripts/rocketnet-provision sweep` after a crash.

**What the server needs.**

- No compiler: the XDP program is compiled and embedded at build time
  (section 1), and the server loads it from memory.
- Root: the pre-flight refuses a non-root process with
  `xdp pre-flight: not root (R1 of scripts/rocketnet-provision)`
  (`sdk/rocketnet_server_socket_linux.cpp:905-912`), and any unmet condition
  ends in `xdp pre-flight failed: this machine is not provisioned for the XDP data plane`
  (`sdk/rocketnet_server_socket_linux.cpp:1315-1317`). On an unprivileged host
  `auto` therefore falls to `gso` (`.github/workflows/ci.yml:218-221`).
- A real port and a global address: port 0 refuses `xdp` (section 2), and an
  IPv6 link-local address is refused by name
  (`sdk/rocketnet_server_socket_linux.cpp:929`).
- Queues: `num_xdp_queues`, default 7; worker CPUs start at `num_os_cpus`,
  default 1 (`sdk/rocketnet_driver_builtin.cpp:195-196`, `BUILDING.md:464-466`).
- Memory: each queue's UMEM is 65536 frames of 4096 bytes, 256 MiB
  (`sdk/rocketnet_constants.h:50-51`, `sdk/rocketnet_server_socket_linux.cpp:840-848`);
  seven queues are 1,879,048,192 bytes of UMEM inside a plan of roughly 4.2 GiB
  (`BUILDING.md:232-233`, `:282`), within the 4096 MiB `ROCKETNET_XDP_MEMORY_MIB`
  budget (`BUILDING.md:225-229`, `sdk/rocketnet_server_socket_linux.cpp:894-901`).
- Both families on one interface: `rocketnet_server_create` names an IPv4
  and an IPv6 address on the same port, and `xdp` carries both destinations when both
  addresses are on that interface (`sdk/rocketnet_server_socket_linux.cpp:1848`).
  Two interfaces refuse with `TRANSPORT_FAMILIES_UNSUPPORTED`
  (`sdk/rocketnet_server_socket_linux.cpp:1479`). One interface takes one
  XDP program.

Checks that need no root, as CI runs them
(`.github/workflows/ci.yml:475-480`, `:492-494`):

```sh
bash scripts/rocketnet-provision selftest
python3 tools/check_xdp_archive.py
```

Live AF_XDP runs as root only inside a virtme-ng guest of the host's own
kernel, never on the host (`.github/workflows/ci.yml:290-295`, `:335-349`;
`TRANSPORTS.md:260-270`). The guest needs `vng`, a readable
`~/vm-kernels/vmlinuz-$(uname -r)` and a writable `/dev/kvm`
(`.github/workflows/ci.yml:339-345`). The command is one quoted string so
older and newer `vng` releases pass it to the guest shell alike:

```sh
vng --run "$HOME/vm-kernels/vmlinuz-$(uname -r)" --memory 1536M --cpus 3 --append "irqaffinity=0 isolcpus=1-2 nohz_full=1-2" --rwdir "$PWD" -- "sudo ./bin/functional --run test_xdp_transport_max_size_both_ways_ipv4 && sudo ./bin/functional --run test_xdp_transport_max_size_both_ways_ipv6"
vng --run "$HOME/vm-kernels/vmlinuz-$(uname -r)" --memory 1536M --cpus 3 --append "irqaffinity=0 isolcpus=1-2 nohz_full=1-2" --rwdir "$PWD" -- "sudo ./bin/test_xdp_filter -v"
```

`test_xdp_filter` judges the BPF program with the kernel's verifier and
`BPF_PROG_TEST_RUN` (`TRANSPORTS.md:256-261`), on the same `server_xdp.o` the
library embeds. In a 1536M guest with two isolated CPUs and no clang on
`PATH`, all 18 cases print `ok`, including `xdp_on_link_reply_reaches_the_peer`
and `xdp_create_fails_part_way_tears_down`.

## 7. Socket errors and ICMP

Linux reports an ICMP error (port unreachable, host unreachable) to a UDP
socket only when the socket is connected or has `IP_RECVERR` set (Linux
`net/ipv4/udp.c` and `net/ipv6/udp.c`, `__udp4_lib_err` and
`__udp6_lib_err`). rocketnet's sockets are never connected and never set
`IP_RECVERR`: they are created at `sdk/rocketnet_platform.cpp:310`,
`sdk/rocketnet_server_gso_linux.cpp:276` and
`sdk/rocketnet_server_uring_linux.cpp:203`, and send with `sendto`,
`sendmmsg` or `SENDMSG` to an address per packet
(`sdk/rocketnet_platform.cpp:526`, `:540`, `:817`). So a dead or unreachable
peer produces no error on the receive path of `portable`, `gso` or
`io_uring`. On `xdp`, ICMP is not UDP, so the XDP program passes it to the
kernel stack (`TRANSPORTS.md:216-218`). This command shows the kernel rule on
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
  drain and is logged at debug, except `EAGAIN` and `EINTR`
  (`sdk/rocketnet_platform.cpp:584-593`, `:938-949`); a failed `sendto` is
  logged at debug and the packet is dropped (`:526-546`); a `sendmmsg` refused
  with `EAGAIN`, `EWOULDBLOCK` or `ENOBUFS` drops the rest of the batch, and
  any other errno drops the one refused entry and continues with the next
  (`:871-898`).
- **gso**: a receive worker's `recvmmsg` returning nothing or an error ends
  that drain (`sdk/rocketnet_server_gso_linux.cpp:450-460`); a refused
  `sendmmsg` drops the rest of the flush and counts the packets in
  `ROCKETNET_SERVER_GSO_COUNTER_SEND_DROPPED`, never resending
  (`sdk/rocketnet_server_gso_linux.cpp:1036-1045`,
  `sdk/rocketnet_server_gso.h:96`).
- **io_uring**: a send completion with a negative result counts in
  `ROCKETNET_SERVER_URING_COUNTER_SEND_DROPPED`
  (`sdk/rocketnet_server_uring_linux.cpp:553-554`,
  `sdk/rocketnet_server_uring.h:61`); a multishot receive that ends is armed
  again (section 5); a submit error other than `EINTR`, `EAGAIN` or `EBUSY` is
  logged (`sdk/rocketnet_server_uring_linux.cpp:623-624`).

`functional/posix_hard_killed_peer.cpp` and `functional/posix_socket_errors.cpp`
cover a hard-killed peer and the socket error paths per transport cell.

## 8. Limits for 1000 clients

`ROCKETNET_MAX_CLIENTS` is 1000 (`sdk/rocketnet.h:100`), and the server's
`max_clients` defaults to it (`sdk/rocketnet_internal.h:475`).

**File descriptors.** A server opens a fixed set of sockets, independent of
`max_clients`: the plan's `sockets` field (`bench/transport_memory.cpp:87`).
For 1000 clients: `portable` 1, `gso` 4, `io_uring` 4 plus its ring, `xdp` 7
(the plan output in section 10). The default soft limit of 1024 is ample; the
section 10 commands create 1000-client `gso` and 32-queue `io_uring` servers
under `ulimit -n 64`. A client opens one UDP socket and, on Linux, one eventfd
(`sdk/rocketnet_driver_builtin.cpp:48`, `:54`,
`sdk/rocketnet_platform.cpp:668`), so a process that runs N clients needs about
2N descriptors: raise `ulimit -n` past 2000 for 1000 clients in one process.

**Locked memory.** `portable` and `gso` need no memlock headroom: the section
10 commands create 1000-client servers of each under `ulimit -l 64`.
`io_uring` needs some, kernel-dependent (section 5); keep `ulimit -l` at
8192 KiB or more. `xdp` raises its own `RLIMIT_MEMLOCK` to unlimited at create and refuses with
`server could not setrlimit` if it cannot (`BUILDING.md:407-408`,
`sdk/rocketnet_server_socket_linux.cpp:1605-1614`); as root on a host that
succeeds.

**Memory.** `./bin/transport_memory <clients> <payload> <slots> <transport> <queues> --plan-only`
prints the plan without opening a socket; without `--plan-only` it creates a
loopback server and checks the allocator against the plan
(`bench/transport_memory.cpp:65-113`, `BUILDING.md:294-303`). At 1000 clients,
1416-byte payloads and 16 reassembly slots the plans are 483,668,499 bytes
(`portable`), 625,876,730 (`gso`), 594,710,098 (`io_uring`) and 4,269,352,940
(`xdp`, 7 queues, of which 1,879,048,192 UMEM). The plan excludes OS stacks,
kernel queues, loader and application memory
(`sdk/rocketnet_server_transport.cpp:1117`, `BUILDING.md:247-255`).

## 9. Containers

What each transport needs, from the requirements above:

- **portable** and **gso**: nothing beyond a UDP socket; no capability
  (`sdk/rocketnet_server_gso.h:29`). An unprivileged container's `auto`
  grants `gso`, since `xdp` refuses for want of root (section 6).
- **io_uring**: a kernel with `io_uring_disabled` at 0 and a seccomp profile
  that allows the `io_uring_setup`, `io_uring_enter` and `io_uring_register`
  calls liburing makes. A refusing profile makes the probe fail and the
  ladder falls (section 5). Pin it, since `auto` grants `gso` first.
- **xdp**: euid 0 in the container (R1), the kernel's grant of an AF_XDP
  socket and a BPF program load (K1), permission to raise `RLIMIT_MEMLOCK`
  to unlimited (section 8), the NIC itself in the container's network
  namespace, and bpffs at `/sys/fs/bpf` for the instance's pin root
  (`BUILDING.md:430-435`). The boot tier belongs to the host. Where bpffs is
  shared across pid namespaces, run `sweep` from the namespace that owns the
  servers (`BUILDING.md:436-442`, `scripts/rocketnet-provision:60-64`).

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
ROCKETNET_SERVER_TRANSPORT=portable ROCKETNET_CLIENT_TRANSPORT=portable ./bin/test_lifecycle
./bin/functional
python3 tools/check_memory.py ./bin/transport_memory
./bin/transport_memory 1000 1416 16 portable 0 --plan-only
./bin/transport_memory 1000 1416 16 gso 0 --plan-only
./bin/transport_memory 1000 1416 16 io_uring 0 --plan-only
./bin/transport_memory 1000 1416 16 xdp 7 --plan-only
bash -c 'ulimit -n 64 && ulimit -l 64 && ./bin/transport_memory 1000 1416 16 portable 0'
bash -c 'ulimit -n 64 && ulimit -l 64 && ./bin/transport_memory 1000 1416 16 gso 0'
bash -c 'ulimit -n 64 && ./bin/transport_memory 1000 1416 16 io_uring 32'
```

`./bin/functional` with no pin runs over the `auto` ladder, which on an
unprivileged host is `gso` (`.github/workflows/ci.yml:218-221`, `:263-266`).
Then the section 3 matrix loop, the section 6 no-root checks and `vng` lines,
and the section 7 ICMP check.
