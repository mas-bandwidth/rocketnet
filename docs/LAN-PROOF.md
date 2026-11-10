# LAN proof

One command runs one cell. The native cells are gso, io_uring and xdp on
Linux, iocp and rio on Windows, and msgx on macOS. The results table of every
completed cell is recorded below.

The inventory has one row per machine: label, ssh name, role, os, test
interface, and whether that interface is the machine uplink. The real
inventory lives outside this repository. `scripts/lan-proof.inventory.example`
has placeholder names only.

Add `--dry-run` to a complete `scripts/lan-proof` command to print its plan
without contacting a host; the inventory, cell and duration are still required.
`scripts/lan-proof-selftest` checks that dry run and the result-row parser
on canned logs, and it opens no socket.

The orchestration host needs Bash, Python 3, SSH/SCP and a `timeout` command.
Remote build steps, and the selftest's build fixtures, require `timeout` or
`gtimeout` with `--kill-after` support. A macOS host may need that tool installed
separately; its absence fails the build fixture rather than establishing a
passing LAN selftest. Live hosts also need the per-platform build tools and
the bench-lock utilities used by the script. Inventory access and provisioned
test interfaces are prerequisites, not work performed by a dry run.

A live run, for example the bounded Linux gso cell:

```text
scripts/lan-proof --inventory INVENTORY --cell gso --duration 120 \
    --clients 1000 --commit "$(git rev-parse HEAD)" --run-dir DIR --run-id 0123456789abcdef
```

uses the full commit ID (not the literal string `HEAD`), checks every host is
at that head and builds it, records each host's
OS, kernel and every test interface's model, driver, negotiated speed and
duplex, and refuses to start when a host's foreign load is at or above
half its core count. Samples are retained every 60 seconds while the cell
runs and at its end; server runtime load may not exceed its logical cores,
and client end load may not exceed each client's logical cores. On Linux
and macOS hosts, SSH and copy steps have a 60-second overall bound and each
remote build has a 900-second overall bound. A Windows host's commands may
first queue for its bench lock, so their bounds add a 900-second lock wait:
960 seconds for an SSH step, 1800 seconds for a build, and the lock wait
plus the cell duration plus 600 seconds for the copy step and for the
server's exclusive hold. It starts the server pinned to the transport (Linux
and macOS by ssh with the copied remote script, the open-file limit raised
on macOS; Windows under an exclusive lock; the xdp server inside a guest
on a dedicated test interface by default), starts shards of at most 250 clients on
the client machines and never on the server's machine, with no client host
running more shard processes than half its logical cores, collects every
log, writes the allocation manifest, runs
`tools/check_split_load.py --load proof`, and prints one table row. Every
remote step is bounded by a timeout and teardown stops only the processes
the run started.

Host facts and load are read with each machine's own tools. Linux reads
`ethtool`, `lspci`, `ip` and `/proc/loadavg`. macOS reads `ifconfig` (address
and media, which gives speed and duplex), `networksetup` (the hardware port),
`route get default`, `sysctl -n vm.loadavg` and `sysctl -n hw.ncpu`. A Windows
machine is driven from its Linux subsystem shell, and its facts are native:
`Get-NetAdapter`, `Get-NetIPAddress` and `Get-CimInstance`. Windows has no
load average, so its load is the busy share of its logical processors plus
the processor queue length at the moment of the sample. Every load, core
count and address is checked as a number or an IPv4 address before a run
is admitted, and a host whose facts do not parse refuses the run.

The Windows build uses the multi-config Visual Studio generator,
`cmake -B build -G "Visual Studio 18 2026" -A x64` then
`cmake --build build --config Release --parallel 4`, in a shared build slot,
and runs `bin/Release/load_split.exe`. Every command the script runs on a
Windows machine is inside its bench lock: the copy of the remote step script,
the checkout, the build, the facts and load samples, log reads and process
checks take a shared slot, and the server cell holds the exclusive lock until
the server has exited, so no other timed work shares the machine during the
cell. A command of the cell itself runs inside that held lock, and the
server's ready line comes back on the lock holder's own output.

A result row is eleven fields separated by `|`: cell, server label, server
NIC speed in megabits, clients, duration, connected throughout, timeouts
(the server's disconnect count), delivery verdict for each reliability mode,
delivery verdict for each size class, minimum per-client kbps, verdict. The
merger validates every direction, mode and size-class stream before those
verdicts can pass; when the aggregate delivery check fails, individual
categories are `unknown` rather than all being labeled failed. A speed or NIC
model that cannot be read is `unknown`. A
slower switch link is recorded when it is known.

For the default xdp guest path, the inventory's server interface is a dedicated
Linux bridge connected to the test LAN. It must not be the host's uplink.
The script starts the server with the host's own kernel
inside a `virtme-ng` guest with three CPUs,
`irqaffinity=0 isolcpus=1-2 nohz_full=1-2`, and
`--network bridge=<test bridge>`. The guest holds the whole server, so its
memory is computed, not fixed: the script runs `load_split plan-server <clients>` on the built head and sizes the guest as
`(server_public_required_bytes + clients * 9848630) * 5 / 4 + 1 GiB`, rounded
up to whole GiB. The 9,848,630-byte (`9848630`) per-client figure is a chosen
per-client reserve allowance in `scripts/lan-proof-remote` (compared with the
1,742,815-byte per-client touched-memory figure measured in
[LOAD.md](LOAD.md#the-nightly-af_xdp-cells-guest-memory)); the 1 GiB is the
guest kernel's own. The recorded 1,000-client plan was 18394142400 bytes,
giving a 34 GiB guest; use the current planner output for another build or
configuration. The script refuses the xdp cell when the host's
`MemAvailable` is below the guest size. The guest provisions its bridged LAN
interface, and the clients use the address printed by the guest's ready line.
The script refuses the xdp cell when the inventory marks this interface as the
uplink, unless the separate bare-metal mode below is explicitly selected.

The optional `--xdp-uplink IFACE` selects a bare-metal XDP server instead of
the guest. The named interface must match the inventory's server interface;
this mode permits that interface to be the uplink. The script checks that the
interface is provisioned and has no attached XDP program, then arms a
`systemd-run` detach timer for the requested duration plus 300 seconds before
starting the server. Normal teardown detaches, cancels the timer and checks
the interface. This mode changes a live interface and can interrupt host
connectivity; a dry run only displays the plan. The historical
[XDP result](runs/lan-1000-proof-xdp.md) used this mode, not the default guest
path, and retains its original delivery and timing verdicts.

## Recorded cells

The results table of every native cell (gso, io_uring and xdp on Linux; iocp
and rio on Windows; msgx on macOS) is recorded in
[runs/lan-1000-proof.md](runs/lan-1000-proof.md), one row per completed cell,
each with its manifest, log digests and load samples. A run is kept only
when every host's foreign load was under half its cores at the start, the
server host stayed at or under its core count while the cell ran, and every
client host stayed at or under its core count at the end; otherwise it is
discarded and the reason recorded there.
