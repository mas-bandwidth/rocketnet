# LAN proof

One command runs one cell. The native cells are gso, io_uring and xdp on
Linux, iocp and rio on Windows, and msgx on macOS. The results table of
every cell is recorded in `docs/runs/lan-1000-proof.md`.

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
half its core count. It starts the server pinned to the transport (Linux
and macOS by ssh with the copied remote script, the open-file limit raised
on macOS; Windows under an exclusive lock; the xdp server inside a guest
on a dedicated test interface), starts shards of at most 250 clients on
the client machines and never on the server's machine, collects every
log, writes the allocation manifest, runs
`tools/check_split_load.py --load proof`, and prints one table row. Every
remote step is bounded by a timeout and teardown stops only the processes
the run started.

A result row is ten fields separated by `|`: cell, server label, server
NIC speed in megabits, clients, duration, connected throughout, timeouts
(the server's disconnect count), delivery verdict, minimum per-client
kbps, verdict. A speed that cannot be read is `unknown`. A slower switch
link is recorded when it is known.

The xdp cell runs in a guest on a test interface. The script refuses that
cell when the inventory marks the interface as the uplink.

## Recorded cells

| cell | server | speed | clients | duration | connected | timeouts | delivery | min kbps | verdict |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |

The bounded Linux gso cell (1000 clients, 120 s) is run through the script
before this table is filled; its row, the five logs, the allocation
manifest and the start and end loads are kept with the run directory named
in the session report, and the row is copied into
`docs/runs/lan-1000-proof.md` with the remaining cells.
