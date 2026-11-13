# harden_load: the server transport hardening load

`tests/load/harden_load.cpp` is a functional and bench tool. It is never a unit test, because it uses real sockets,
real time and real threads. It builds in development builds only (`ROCKETNET_DEV`), since it reads the library's
counters through `rocketnet_layers.h`. Every cell is judged by equations over named counters, never by "it did not
crash".

## Roles

One binary runs four roles, each in its own process. The server and its load can therefore sit on different machines
or in different network namespaces.

| role | what it does |
|---|---|
| `server --address A` | A 60 Hz game frame through the public API: update, receive until NULL, write, send_packets. |
| `clients --address A --key K` | `--clients N` connected players on `--threads T` threads. They join at `--join-rate`, and the last `--churn-clients` churn (clean leave, silent stop, immediate reconnect). Every message is verified byte for byte. |
| `hostile --address A --key K` | Traffic from addresses that are not players, at `--pps`, as a mix of `--kinds`: garbage, prefixed, truncated, badsig, expired, reqreplay, replay. Replay is Linux only and needs CAP_NET_RAW. |
| `reconcile --logs a,b,c` | Reads every role's `harden_term:` lines and judges the cell. |

All roles share one wall-clock end, `--end-at`. The end is two cuts, one per direction, and each is read while its
direction is silent. The timeline is in `harden_server_main`.

## The space shape

Each tick the server writes `--base` unreliable messages to every connected client. On `--burst-clients` clients a
tick (rotating) it writes a burst of `--burst` messages, with sizes cycling through `--sizes`. It also writes one
reliable ordered message every `--reliable-every` ticks, and one `--big-bytes` fragmented reliable message every
`--big-every` ticks.

## The wide fill: `--fill-jobs N`

With N > 0, N job threads write the unreliable messages between update and send_packets, under the threading contract
in `rocketnet_layers.h`:
- Each job owns a contiguous run of the connected clients, so each client has one filler.
- Each message is written in place as one SINGLE payload: transport `start_packet`, then
  `rocketnet_message_single_finish`, then `finish_packet`. The message goes at `frame + payload_index`, because
  `start_packet` returns the frame.
- Main joins the jobs, then writes the reliable and big messages through the message server, then calls send_packets.

N = 0 (the default) keeps every write on main.

## The verdict

**The chains.** Each direction is judged on one equation over named counters:

    library_sent = library_received + named_drops

`named_drops` is the sum of:
- the sender's kernel send-buffer errors;
- the receiver's UDP receive errors, no-port drops and IP discards;
- the qdisc's drops;
- the device's tx and rx drops;
- the datagrams the server transport refused before counting them received (PREFILTER_TOO_SHORT and TOO_LARGE).

Each step is also printed beside the equation: send_gap, path_gap, receive_gap and wire_gap. The kernel counts an skb,
so a GSO send of N datagrams counts as one, and UDP GRO does the same on receive. A qdisc drop of a GSO skb names one
datagram for several, and so does a GSO skb that arrives whole (veth, UDP_GRO) at a closed port or a full socket.
While any of those drops, a gap of up to the send_gap passes, printed as `gso_slack`. A
duplicate row passes a gap of zero or below.

**The players.** Players must deliver unreliable messages at or above `--min-delivery`, with no errors and no
unplanned ends among the stable clients.

**The frame.** The frame passes when work p99 is at or under `--frame-p99-us`.

**The server.** The library must conserve its counters, and fds, threads and library bytes must stay flat from the
post-warmup sample to the end.

Host-wide kernel counters include traffic that is not the cell's. Run cells on hosts that carry nothing else.

## Scripts

- `scripts/harden-rows` defines the rows: control, space, churn, impair, duplicate, hostile and longrun. It is sourced
  by the two scripts below.
- `scripts/harden-cell` runs a cell on one Linux machine, across two network namespaces over a veth, with the server
  on a third of the CPUs. Run it as root, under nova-reserve:

      nova-reserve run -- sudo scripts/harden-cell --bin bin --transport gso --row space --burst-clients 3 \
          --fill-jobs 4 --out ../cells/<cell> --slot 90 --seconds 30 --clients 100 --threads 4

- `scripts/harden-pair` runs a cell on two machines over a real link, driven by ssh from a control machine that
  holds its own harden_load for the reconcile. A netem row puts netem only on the cell's UDP port: a prio root with
  netem on band 4, plus a u32 filter.

      scripts/harden-pair --ssh "ssh -i KEY -o IdentitiesOnly=yes -o BatchMode=yes" \
          --server-host user@server --client-host user@clients --server-ip <private address> \
          --remote /path/to/checkout --transport io_uring --row impair --fill-jobs 6 \
          --out <dir> --seconds 60 --clients 1000 --threads 16 --local-bin ./bin

  With `--server-os windows` the server is a Windows machine and the players stay on Linux:
  - each server command runs in Git for Windows' bash;
  - the tool is `<checkout>/bin/Release/harden_load.exe`;
  - netem goes on the client machine only;
  - the chain stands on library terms, since the tool reads no kernel counters on Windows.

  A RIO server with more than one lane needs `--server-env ROCKETNET_SERVER_BIND_IPV4_ADDRESS=<private address>`.

Both scripts exit 0 only on PASS.
