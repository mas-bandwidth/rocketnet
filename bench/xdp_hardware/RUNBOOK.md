# The XDP hardware tier: runbook

This tier is the only one that runs the committing XDP path on a real NIC
driver (ixgbe, i40e or ice): native attach, zero-copy AF_XDP per queue,
multi-queue steering, IRQ and core pinning, throughput and latency. It runs
on one dedicated machine, held from the fleet, one test at a time. The NIC's
hardware counters and `rocketnet-provision verify` judge the results, not
counters the code under test increments.

XDP takes over the whole machine: NIC queues, IRQ affinity, isolated cores
and attached programs. Never run this tier on a shared bench, and never on a
machine that serves anything else. A member advertises the `xdp` gate fact
only when `rocketnet-provision verify` passes on it.

Files:

| File | What it is |
|---|---|
| `hold.sh` | The lease: an exclusive `flock` under `/run`, the host snapshot, and the traps that run restore on every exit path |
| `restore.sh` | Puts the machine back: stops the run's processes, detaches, sweeps, verifies, and diffs the snapshot |
| `run.sh` | One full pass, under `hold.sh`, printing one `ROW` line per row |
| `xdp_hw_receiver.cpp` | The library's own server transport on the xdp driver. It reads the counters and answers `run.sh` on stdin and stdout |
| `xdp_hw_sender.cpp` | `flood`: an AF_XDP transmitter that sends RocketNet frames. `probe`: a library client that sends timestamped payloads |
| `counters.py` | The judge: exact identities, NIC tolerances, the knee |
| `Makefile` | Builds the two programs against the CMake build's libraries |

The model is `tla/XdpHardwareBench.tla`, described in
`tla/README-XdpHardwareBench.md`. `hold.sh` and `restore.sh` name the model
actions they implement.

## Parameters, never constants

`run.sh`, `hold.sh` and `restore.sh` all take one file: a machine conf,
sourced as shell. Every machine-specific fact is set there. Nothing about
the interface, driver, queue count or core range is hard-coded. Example,
for a two-port NIC cabled port to port:

```sh
# /etc/rocketnet-xdp-hw.conf -- this machine's parameters
XDPHW_IFACE=enp1s0f0             # the receiver port: the library's server binds here
XDPHW_QUEUES=7                   # combined channels on the receiver port = server num_xdp_queues
XDPHW_RANGE=1-15                 # the isolated range the box booted with (B1)
XDPHW_OS_CPUS=1                  # first worker core: workers on XDPHW_OS_CPUS + queue
XDPHW_MAIN_CORE=0                # the receiver's main thread, outside the worker cores
XDPHW_ADDRESS=198.18.0.1         # the receiver port's IPv4 (benchmark range, RFC 2544)
XDPHW_PORT=40000
XDPHW_SENDER_IFACE=enp1s0f1      # the sender port, cabled to the receiver port
XDPHW_SENDER_NETNS=rnxdp-sender  # its namespace (self-contained mode)
XDPHW_SENDER_ADDRESS=198.18.0.2
XDPHW_SENDER_QUEUES=2            # AF_XDP transmit queues on the sender port
XDPHW_SENDER_CORES=9,10          # one core per sender queue, isolated, not a worker core
XDPHW_FLOWS=64                   # distinct UDP source ports, so RSS spreads them
XDPHW_RATES="500000 1000000 2000000 4000000 6000000 8000000 10000000 12000000 14000000"
XDPHW_WARMUP=30                  # bench/PROTOCOL.md: 30 s warm-up, discarded
XDPHW_WINDOWS=5                  # five measured windows per step
XDPHW_WINDOW_SECONDS=60
XDPHW_TOL_ABS=32                 # NIC-counter tolerance, frames per window
XDPHW_TOL_REL=0.000001           # or this fraction of sent, whichever is larger
# XDPHW_SENDER_EXEC="ssh -T sender-host"   # second-machine mode, below
```

Size the cores together. One receiver queue takes one isolated core. Some
drivers add a misc interrupt that carries the bare interface name
(ixgbe-class), so make the isolated range one core larger there. The sender
cores must be isolated too, and must not overlap
`XDPHW_OS_CPUS .. XDPHW_OS_CPUS + XDPHW_QUEUES - 1` or the receiver
interrupt cores.

## Cold bring-up of a new machine

You do this once per machine, as root, before any run. The harness never
does any of it. Every step changes the machine. After the last step, the
machine's state is the baseline that each run's snapshot records.

1. **The NIC and its driver.** Use a NIC whose driver supports AF_XDP zero
   copy. Intel 82599/X520/X540 (ixgbe), 700-series X710/XL710/XXV710 (i40e)
   and E810 (ice) are the drivers this runbook names. Each driver documents
   its own zero-copy support. For the 700 series, the distro driver can be
   too old. Build `i40e` from github.com/mas-bandwidth/ethernet-linux-i40e
   (B3). Record `ethtool -i <iface>` and `uname -r` with every result.
2. **Toolchain** (BUILDING.md lists it): cmake, a C++17 compiler, clang with
   the bpf target, libbpf 1.0 or later, libxdp, xdp-tools (`xdp-loader`),
   bpftool, ethtool, iproute2 with `ss --xdp`, python3, util-linux
   (`flock`, `taskset`), and linuxptp (`phc_ctl`). Install them with the
   machine's own package manager during bring-up. Runs never install
   anything.
3. **The boot tier** (`scripts/rocketnet-provision` header, B1 and B2). Add
   `nosmt irqaffinity=0 isolcpus=<range> nohz_full=<range>` to
   `GRUB_CMDLINE_LINUX`, then run `update-grub` and reboot. `<range>` must
   cover the receiver workers, their interrupts and the sender cores.
4. **The cabling.** There are two modes, and each result names the one it
   used.
   - *Self-contained (preferred):* a second port of the same NIC, cabled
     straight back to the receiver port. The sender port lives in its own
     network namespace with its own pinned cores, so the kernel never routes
     between the two ports inside the host:

     ```sh
     ip netns add rnxdp-sender
     ip link set enp1s0f1 netns rnxdp-sender
     ip netns exec rnxdp-sender ip link set lo up
     ip netns exec rnxdp-sender ip addr add 198.18.0.2/24 dev enp1s0f1
     ip netns exec rnxdp-sender ip link set enp1s0f1 up
     ip netns exec rnxdp-sender ethtool -L enp1s0f1 combined 2
     ```
   - *Second machine:* the sender port is on another machine on a direct
     cable. Set `XDPHW_SENDER_EXEC` to the command prefix that runs a
     command there, with the built `bin/xdp_hw_sender` at the same path. The
     sender-side snapshot and counters are then read through that prefix.
5. **The receiver port.** Give it the address, a gateway route through the
   sender's address, and a permanent neighbour entry for that gateway. The
   library's xdp driver addresses every transmitted frame to the
   interface's gateway, and it fails create without one:

   ```sh
   ip addr add 198.18.0.1/24 dev enp1s0f0
   ip link set enp1s0f0 up
   ip route add 198.18.1.0/24 via 198.18.0.2 dev enp1s0f0
   ip neigh replace 198.18.0.2 lladdr "$(ip netns exec rnxdp-sender cat /sys/class/net/enp1s0f1/address)" dev enp1s0f0 nud permanent
   ```
6. **Quiet the wire.** Turn off LLDP agents and IPv6 on both ports
   (`sysctl -w net.ipv6.conf.<iface>.disable_ipv6=1`, inside the namespace
   for the sender). Then the only frames on the cable are the test's, plus
   ARP, which the permanent neighbour entries suppress. The NIC tolerance
   covers what is left. If a window goes past it, that is a finding.
7. **The receive side scaling hash.** Spread UDP over the 4-tuple:
   `ethtool -N enp1s0f0 rx-flow-hash udp4 sdfn`. The sender varies the
   source port across `XDPHW_FLOWS` flows, and the per-queue NIC counters
   show the spread for every window.
8. **The runtime tier:** `scripts/rocketnet-provision enp1s0f0 7` (R1 to R7),
   then `scripts/rocketnet-provision enp1s0f0 7 verify`. A run starts only
   when verify passes.
9. **Build:** `make` at the repository root, then
   `make -C bench/xdp_hardware`.
10. **The first run's counter check.** Driver counter names differ by
   driver and by version. Read `ethtool -S` on both ports once. Confirm that
   `counters.py` recognises the per-queue rx packet names
   (`rx_queue_N_packets` on ixgbe and ice, `rx-N.packets` on i40e) and the
   sender's `tx_packets`. Also confirm that every drop, miss and no-buffer
   counter the driver has matches its drop pattern. A window line names
   every nonzero drop counter, and an unrecognised name shows as an `H1` or
   `H2` failure, never as a silent pass.

The namespace, the address, the route and the neighbour entry do not
survive a reboot. Repeat steps 4 to 8 after one. The first run after a
reboot has no earlier snapshot to compare with, and says so.

## A run

```sh
bench/xdp_hardware/run.sh /etc/rocketnet-xdp-hw.conf
```

`run.sh` re-executes itself under `hold.sh`. While another run holds the
machine, it refuses with exit 75 and names the holder's pid and start time.
Results go to `bench/xdp_hardware/results/<UTC>/` (`XDPHW_OUT`
overrides), with every raw capture beside the verdicts: `ethtool -S` at
every window edge, the receiver's `SNAP` lines, the sender's `DONE` lines,
`/proc/interrupts` affinities, `xdp-loader status`, and the verify output
before and after.

## The rows

Each row prints `ROW <name> PASS|FAIL <detail>`. A full pass is every row
green.

1. **`preflight_verify`**: `rocketnet-provision <iface> <queues> verify
   <range>` passes, and the sender port carries its address. If it fails,
   the run stops with nothing attached. The harness never provisions.
2. **`native_attach_on_the_nic`**: the receiver's transport reports driver
   `xdp`, `ip -d link` shows a driver-mode program (`xdp`, never
   `xdpgeneric`), and `xdp-loader status` shows native mode. The library's
   SKB fallback log line fails the row.
3. **`zero_copy_bind_per_queue`**: the receiver reads every AF_XDP socket it
   holds with `getsockopt(XDP_OPTIONS)`. Each queue's socket on the
   receiver port must report `XDP_OPTIONS_ZEROCOPY`. A fall to copy mode
   fails the row and never counts as a pass.
4. **`irq_and_core_pinning`**: each of the interface's IRQ lines in
   `/proc/interrupts` has a single-CPU `smp_affinity_list`, in the isolated
   set, no CPU shared. For every queue, a receiver thread's
   `Cpus_allowed_list` is exactly `num_os_cpus + queue`. Drivers whose IRQ
   names do not contain the interface name (mlx5-class) fail the row with
   that reason. Pin and check those with the driver's own tooling.
5. **`throughput_by_hardware_counters`**: a 30 s warm-up is discarded. Then,
   for each rate in `XDPHW_RATES`, `XDPHW_WINDOWS` windows of
   `XDPHW_WINDOW_SECONDS` run (bench/PROTOCOL.md). At each window's end the
   sender stops and waits for every transmit completion. The receiver is
   drained when two snapshots 200 ms apart agree, so no frame is in flight.
   Only then are the edges read. `counters.py` judges each window:
   - *Exact* (a difference of one fails, because it is an uncounted path,
     never noise):
     - E1: the stats map's REDIRECTED, summed across CPUs, equals the
       transport's PACKETS_RECEIVED plus the sockets' `rx_dropped` and
       `rx_ring_full`.
     - E2: on every window whose NIC drop counters read zero, the sender's
       sent count equals received plus every in-process drop (the sockets'
       and the stats map's five).
     - E3: every REFUSAL-class counter on the receiver, and the sender's
       `tx_invalid_descs`, stay at 0.
   - *Hardware, within tolerance:*
     - H1: the receiver NIC's per-queue rx packets sum equals the program's
       verdict sum.
     - H2: the sender NIC's tx packets equals sent.
     - H3: on a window where the NIC drops, sent equals received plus every
       counted drop, the NIC's included.

   The NIC counters are compared within tolerance, never exactly, because
   the harness cannot read them atomically with its own counters. The
   tolerance is `max(XDPHW_TOL_ABS, XDPHW_TOL_REL x sent)`: 32 frames, or
   one in a million, per window.

   A *clean* window has zero NIC drops, zero socket drops and zero
   refusals on both ends. The sweep stops after the first step that is not
   clean. **The knee** is the highest step whose every window is clean and
   exact. The step table gives the median, min and max of the NIC-counted
   rate for each step.
6. **`latency_by_hardware_timestamps`**: after the sweep, the sender's
   `probe` connects a library client to the receiver with a development
   connect token. It sends `XDPHW_PROBE_COUNT` payloads at
   `XDPHW_PROBE_INTERVAL_US`, each carrying its send time, and the receiver
   reports p50, p90, p99, p99.9 and max. **These are software timestamps
   (`CLOCK_MONOTONIC`, one host).** The library's XDP program requests no
   receive metadata, so frames on the AF_XDP path carry no hardware receive
   timestamp. Also, the two ports of one NIC each have their own PTP clock.
   `ethtool -T` is saved with the run, so the report can name what the NIC
   offers. A hardware one-way figure needs the program to read
   `bpf_xdp_metadata_rx_timestamp` and both ends on one disciplined clock.
   That would be a change to `server_xdp/` and is not part of this
   harness.
7. **`restore_leaves_the_baseline`**: printed by `restore.sh` after the run.
   See the next section.

## Restore

`restore.sh` runs on every exit path: a pass, a failed row, Ctrl-C or
`kill`. It runs from the `EXIT` trap in `hold.sh`, which `INT` and `TERM`
also reach. Its steps:

1. Stop the run's receiver, sender and probe. The pids file records each
   process's start time, so a recycled pid is never signalled.
2. Detach any XDP program still on either port.
3. Confirm that no AF_XDP socket is left (`ss -a --xdp`).
4. Run `rocketnet-provision sweep` to remove any pin root the run left.
5. Run `rocketnet-provision <iface> <queues> verify` again.
6. Take the snapshot again and diff it against the one taken at hold.

The snapshot covers `ip -d link`, `ethtool -l -k -x -g -c -n`, the IRQ
affinities, the busy-poll knobs, the port's routes and permanent
neighbours, the identity lines of `bpftool prog show`, `bpftool net show`,
`/sys/fs/bpf`, and the sender port's link, channels, features, indirection
table and address. It leaves out counters and timestamps on purpose.

The row passes only with verify passing and an empty diff. Any other
outcome writes `/run/rocketnet-xdp-hw/restore-failed`. Every later hold
refuses until a person has read it, put the machine back, and run
`hold.sh <conf> --clear-failed`.

`bpftool prog show` lists every program on the host. A dedicated machine
runs nothing else that loads BPF programs. A host agent that does will
show up in the diff, and the fix is to stop it, not to filter it.

## After a crash

A run that dies without its trap (`kill -9`, power, a kernel panic) loses
its `flock` along with its process, but the lock file still names it. The
next `hold.sh` sees that, runs `restore.sh --recover` first, and only then
takes the machine. To recover without starting a run:

```sh
bench/xdp_hardware/hold.sh /etc/rocketnet-xdp-hw.conf --recover
bench/xdp_hardware/hold.sh /etc/rocketnet-xdp-hw.conf --status
```

After a reboot, `/run` is empty. Attached programs and bpffs pins do not
survive a reboot either, so verify is the whole check. Redo bring-up steps
4 to 8 before the next run.

## What the tier claims

Throughput numbers belong to this machine, NIC, driver, kernel and
toolchain (bench/PROTOCOL.md). Every result carries all of them and the
cabling mode. A pass here shows that the committing XDP path works on this
driver, and how fast. It does not cover other drivers or other machines.
