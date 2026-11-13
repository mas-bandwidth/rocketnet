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

N = 0 (the default) keeps every write on main. N is at most 64.

## The frame shape: `--frame-shape space|blocking`

The server role has two frame shapes. `blocking` (the default) is the frame above, with the main thread blocked
through all of networking. It is kept for comparison: a row measured in it is a comparison, never a result.

`space` is the frame of space game's server, run on a thread pool of `--game-threads N` threads in all (N at least 2):
the main thread first and special, the rest game threads. The pool is space's `ThreadPool` API
(`tests/load/thread_pool.h`) over enkiTS, vendored in `tests/load/enkiTS` for this tool only and never linked into the
SDK. The frame is the first half on the main thread (send_packets, update, receive), the world update, then the second
half split off as a task on the game threads that writes every player's messages as jobs and stages them through the
public API while the main thread runs to the end of its frame; the main thread joins the task at the top of the next
frame, and the packets leave at that frame's send_packets. Messages are written and staged per client by the jobs, and
committed in one walk at the end of the task.

| flag | meaning |
|---|---|
| `--game-threads N` | The pool, N threads in all. The game threads are made before the main thread pins itself. |
| `--pin-first-cpu C` | The main thread on cpu C and thread i on C + i (advisory on macOS, a group affinity on Windows). A setting the system refused is printed as `harden_pool_refused:`. |
| `--world-ms MS` | The main thread's own share of a stand-in world update. |
| `--game-load F` | Stand-in game jobs that keep each game thread F (0 to 1) of the frame busy. The main thread does its own share while they run. |
| `--space-split bottom\|top` | Where the network task is split off: after the world update (space's order, the default) or before it, so that it runs beside the world update. |
| `--load peak\|average\|custom` | Space game's real load: 15 (peak) or 8 (average) unreliable 1200-byte messages per client per tick, written by the game threads, one small reliable message per client every 3 seconds, no large messages. Under `--frame-shape space` the default is peak; `custom` keeps `--base`, `--burst` and the rest. |
| `--input-bytes B` | The message each player sends each tick (256 by default, about 100 to 200 kbps a player). |

With the space shape each second also prints three lines. `harden_frame:` leads with what rocketnet takes from the
main thread (`main_rn_ms` p99 and max), then the join, the task (its wall time and the core time it takes from the
game threads), the world update, the stand-in game's core time, rocketnet's whole core time, the core time left over
for the game (`leftover_core_ms`: the frame on every thread less rocketnet, the game jobs and the main thread's world
share), and the latency a frame's packets add (from the game state they were built from to the send_packets that
sends them). `harden_threads:` is one line per thread class: the main thread's network milliseconds a frame, its part
done in jobs, the game threads' minimum, mean and maximum, their ratio, and what each game thread has left of the
frame (`leftover_ms`: the frame less its share of the game jobs less its mean network work). `harden_thread_net_ms:`
lists each thread's network milliseconds a frame by thread index, to see how even the split is.

`scripts/space-frame-row` runs one such row on the XDP transport in the layout the game runs, over two machines, and
can profile the server with perf (`--profile`).


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

**The frame.** The frame is an aim, reported and not judged: the cell's verdict is the chains and the players.
Work is rocketnet's whole span on the game thread in one frame, one clock read at the frame's start and one after send,
with update, receive, the write (the wait for the fill jobs included) and send between them and nothing left out. Its
p99 is held to `--frame-p99-us`, 5000 by default, and printed with its max as `aim=MET` or `aim=MISSED`. The rest of a
60 Hz game frame belongs to the game. Each `harden_tick:` also prints `jobs_core_ms`, the fill jobs' busy time per frame
summed over the jobs, because the game needs those cores too.

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

Both scripts exit 0 only on PASS.
