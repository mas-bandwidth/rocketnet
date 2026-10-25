# LAN proof

One command runs one cell. The cells are gso, io_uring, xdp, iocp, rio and msgx. The results table of every cell is recorded in `docs/runs/lan-1000-proof.md`.

The inventory has one row per machine: label, ssh name, role, os, test interface, and whether that interface is the machine uplink. The real inventory lives outside this repository. `scripts/lan-proof.inventory.example` has placeholder names only.

`scripts/lan-proof --dry-run` prints every step and contacts no host. `scripts/lan-proof-selftest` checks that dry run and the result-row parser on a canned log, and it opens no socket.

A result row is ten fields separated by `|`: cell, server label, server NIC speed, clients, duration, connected throughout, timeouts, delivery verdicts, minimum per-client kbps, verdict. A speed that cannot be read is `unknown`. A slower switch link is recorded when it is known.

The xdp cell runs in a guest on a test interface. The script refuses that cell when the inventory marks the interface as the uplink.

This commit's script refuses a live run. The dry run is the part that is here.
