# LAN proof, xdp cell, 1000 clients

Date: 2026-10-25 (UTC, from `date -u`). Head: 58ac8beb.

Verdict: NOT MADE. The cell was not run, so there is no row, no merger
verdict, no delivery verdicts, no receipts and no min per-client kbps.

## Machines

| label | role | OS, kernel | NIC | driver | speed, duplex |
|---|---|---|---|---|---|
| linux-server-1 | xdp server (the XDP bench) | Linux 6.8.0-142-generic | Intel X540-AT2 port 1 | ixgbe 6.2.5 | 10000 Mb/s, full |
| linux-server-1 | (other port of the same card) | | Intel X540-AT2 port 0 | ixgbe | no carrier |
| linux-client-1 | client shards | Linux 7.0.0-38-generic, 16 threads | onboard | atlantic | 1000 Mb/s |
| linux-client-2 | client shards | Linux 7.0.0-38-generic, 16 threads | onboard | atlantic | 1000 Mb/s |

The only cabled port on linux-server-1 (X540 port 1, 10000 Mb/s) carries the
machine's LAN address and its default route, and is its only management link.
The machine has no Linux bridge.

## Why the cell was not made

`scripts/lan-proof` at this head runs the xdp server inside a virtme-ng guest
attached with `--network bridge=<test bridge>`, and before starting it runs
`lan-proof-remote check-xdp-bridge <iface>`, which refuses an interface that
is not a Linux bridge, has no member NIC, carries a default route or contains
the default-route interface. On linux-server-1 the check against the cabled
port gives:

```text
lan-proof-remote: xdp bridge: enp9s0f1 is not a Linux bridge
exit=1
```

Putting that port in a bridge would still be refused, because the port (or
the bridge, once it takes the address) carries the default route. So with
this runner and only this port cabled, the cell cannot start. It was not
worked around: the runner was not changed, XDP was not attached on the host,
and the host's network was not changed.

What the cell needs, one of:

- X540 port 0 cabled to the LAN switch, plus a Linux bridge over it with no
  default route (the setup docs/LAN-PROOF.md describes), or
- a runner change, made on its own card, that allows the xdp server on the
  management port with a timed self-detach armed before the attach.

## Commands

```text
ssh <linux-server-1> 'bash -s check-xdp-bridge enp9s0f1' < scripts/lan-proof-remote
ssh <linux-server-1> 'ethtool enp9s0f1; ethtool -i enp9s0f1; ethtool enp9s0f0; ip -br link show type bridge; lspci'
ssh <linux-client-N> 'uname -r; nproc; cat /proc/loadavg; cat /sys/class/net/<if>/speed; ethtool -i <if>'
```

## Cells

| cell | result |
|---|---|
| xdp, 1000 clients, 600 s, `--load proof` | not made: no bridged test interface on the XDP bench; the runner's bridge check refuses the only cabled port |
