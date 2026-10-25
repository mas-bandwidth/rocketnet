# LAN proof

One command runs one cell. The native cells are gso, io_uring and xdp on
Linux, iocp and rio on Windows, and msgx on macOS. The results table of every
completed cell is recorded below.

The inventory has one row per machine: label, ssh name, role, os, test
interface, and whether that interface is the machine uplink. The real
inventory lives outside this repository. `scripts/lan-proof.inventory.example`
has placeholder names only.

`scripts/lan-proof --dry-run` prints every step and contacts no host.
`scripts/lan-proof-selftest` checks that dry run and the result-row parser
on canned logs, and it opens no socket.

A live run, for example the bounded Linux gso cell:

```text
scripts/lan-proof --inventory INVENTORY --cell gso --duration 120 \
    --clients 1000 --commit HEAD --run-dir DIR --run-id 0123456789abcdef
```

checks every host is at the same head and builds it, records each host's
OS, kernel and every test interface's model, driver, negotiated speed and
duplex, and refuses to start when a host's foreign load is at or above
half its core count. Samples are retained every 60 seconds while the cell
runs and at its end; server runtime load may not exceed its logical cores,
and client end load may not exceed each client's logical cores. SSH and copy steps have a
60-second overall bound, and each remote build has a 900-second overall
bound. It starts the server pinned to the transport (Linux
and macOS by ssh with the copied remote script, the open-file limit raised
on macOS; Windows under an exclusive lock; the xdp server inside a guest
on a dedicated test interface), starts shards of at most 250 clients on
the client machines and never on the server's machine, with no client host
running more shard processes than half its logical cores, collects every
log, writes the allocation manifest, runs
`tools/check_split_load.py --load proof`, and prints one table row. Every
remote step is bounded by a timeout and teardown stops only the processes
the run started.

A result row is eleven fields separated by `|`: cell, server label, server
NIC speed in megabits, clients, duration, connected throughout, timeouts
(the server's disconnect count), delivery verdict for each reliability mode,
delivery verdict for each size class, minimum per-client kbps, verdict. The
merger validates every direction, mode and size-class stream before those
verdicts can pass; when the aggregate delivery check fails, individual
categories are `unknown` rather than all being labeled failed. A speed or NIC
model that cannot be read is `unknown`. A
slower switch link is recorded when it is known.

For an xdp cell, the inventory's server interface is a dedicated Linux bridge
backed by X540 port 0, cabled to the LAN switch. It must not be the host's
uplink. The XDP cell has no recorded result until that test port is cabled and
the bridge exists. The script starts the server with the host's own kernel
inside a `virtme-ng` guest, using 1536 MiB,
three CPUs, `irqaffinity=0 isolcpus=1-2 nohz_full=1-2`, and
`--network bridge=<test bridge>`. The guest provisions its bridged LAN
interface, and the clients use the address printed by the guest's ready line.
The script refuses the xdp cell when the inventory marks this interface as the
uplink.

## Recorded cells

The results table of every native cell (gso, io_uring and xdp on Linux; iocp
and rio on Windows; msgx on macOS) is recorded in
[runs/lan-1000-proof.md](runs/lan-1000-proof.md), one row per completed cell,
each with its manifest, log digests and load samples. A run is kept only
when every host's foreign load was under half its cores at the start, the
server host stayed at or under its core count while the cell ran, and every
client host stayed at or under its core count at the end; otherwise it is
discarded and the reason recorded there.
