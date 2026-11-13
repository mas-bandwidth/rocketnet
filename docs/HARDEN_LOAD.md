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
| `--space-send serial\|wide` | Who sends the players' messages. `serial` (the default) is the frame above: the jobs write and stage them and one walk at the end of the task commits them. `wide`: each game thread starts and finishes its own players' messages itself (`rocketnet_server_start_message`, then `rocketnet_server_finish_client_message`), so the packet is built and sealed on the thread that wrote it and no serial commit is left; a refusal ends that player's frame. Not with `--big-every`: large messages are not written from the game threads. |
| `--space-update main\|split` | The server update. `main` (the default) is one call, `rocketnet_server_update`. `split` is three: `rocketnet_server_update_receive`, `rocketnet_server_update_client` over ranges of client slots as jobs on the game threads, then `rocketnet_server_update_finish`. |
| `--space-net main\|task` | Where send_packets and the update run. `main` (the default): send_packets and the update at the main thread's frame start, as above. `task`: both run at the end of the split-off task, on whichever thread runs the task (the pool's wait runs queued tasks, so the main thread can take a share: `main_in_jobs_ms` shows it), beside the main thread's frame, so the main thread's first half is only the read of what that update received. `harden_frame:` then gives `input_age_ms`, from the update finishing to the main thread handing its inputs to the world update. |
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

The p99 in `harden_tick:` and `harden_frame:` is that second's own, over that second's frames. At the end the server
prints `harden_frame_run:`, the whole run's `main_rn_ms` over every frame (`run_p99`, `run_p999`, `run_max`, `frames`),
beside the mean and the maximum of the per-second p99s (`per_second_p99 mean`, `max`). The mean of per-second p99s is
not a p99 of the run: a slow stretch inside one second moves that second's p99 alone. The reconcile's
`harden_frame: work_p99_us` is the whole run's, the same as `run_p99`.

With the three flags set, every number a row prints leads with what rocketnet takes from the main thread (`main_rn_ms`). The main thread's wait to join the task at the top of a frame is not in it: it is in `join_ms` and counted in `late`. `rn_core_ms` counts the update's range jobs once, in the threads that run them, and the main thread's own serial calls only. `harden_threads:` names the modes (`send_mode`, `update_mode`, `net_mode`) and prints the task's send and update milliseconds (`task_send_ms`, `task_update_ms`), the serial part that is left. The flags are the owner's layout one step at a time: each changes where one piece of rocketnet's work runs and uses only the public API (`sdk/rocketnet.h`).

`scripts/space-frame-row` runs one such row on the XDP transport in the layout the game runs (all three flags set), over two
machines, and can profile the server with perf (`--profile`).

The players' input message defaults to 256 bytes (`--input-bytes`, about 100 to 200 kbps a player). It was 64 bytes
before the frame shape, so every existing row that does not name `--input-bytes` now sends four times the input.

## The verdict

**The chains.** Each direction is judged on one equation over named counters:

    library_sent = library_received + named_drops

`named_drops` is the sum of:
- the sender's kernel send-buffer errors;
- the receiver's UDP receive errors, the cell's no-port drops (below) and IP discards;
- the qdisc's drops;
- the device's tx and rx drops;
- the datagrams the server refused before its library counted them received, on every transport and with or without
  a hostile role: the transport's PACKETS_RECEIVED less the library's received (a worker's prefilter counts its
  refusals into the first only), plus the size drops the receive path counts outside PACKETS_RECEIVED
  (URING_RECEIVE_DROPPED_SIZE, IOCP_ and RIO_RECEIVE_TOO_SHORT and TOO_LARGE). PREFILTER_TOO_SHORT and TOO_LARGE
  are not added whole: they count drops inside PACKETS_RECEIVED and outside it.

Each step is also printed beside the equation: send_gap, path_gap, receive_gap and wire_gap. The kernel counts an skb,
so a GSO send of N datagrams counts as one, and UDP GRO does the same on receive. A qdisc drop of a GSO skb names one
datagram for several, and so does a GSO skb that arrives whole (veth, UDP_GRO) at a closed port or a full socket.
While any of those drops, a gap of up to the send_gap passes, printed as `gso_slack`. A
duplicate row passes a gap of zero or below.

Only Linux exposes the kernel, qdisc and device counters those step lines read. On a server (or players) running
Windows or macOS the step lines that need a counter the OS does not expose are printed `not available on this OS` and
are not part of the verdict; the verdict is the library equation alone. A library gap with no drop named anywhere is a
failure on every OS (the line `library gap with no named drop` says so), so a macOS server's chain can fail only on a
real loss. A Windows server's adapter discards (ReceivedDiscardedPackets) are read over the cut's window, which includes
the stop window, so the delta is named as `device_rx_dropped` and the chain does not require it to close exactly (the
library gap may be that far below zero). A `--profile` row prints the process's handle count and does not judge it,
since the profiler's symbol load opens handles inside the run.

**The players.** Players must deliver unreliable messages at or above `--min-delivery`, with no errors and no
unplanned ends among the stable clients.

**The frame.** The frame is an aim, reported and not judged: the cell's verdict is the chains and the players.
Work is what rocketnet takes from the main thread in one frame. In the blocking shape it is the frame from its start to
after send_packets, less the stand-in world update (update, receive, the write with the wait for the jobs, and send).
In the space shape it is the sum of three intervals: the first half's send_packets, then update and receive, then the
main thread's own writes and the second send_packets. The join wait at the top of the frame (printed as `join_ms`), the
world update and the task hand-off are not in it. Its
p99 is held to `--frame-p99-us`, 5000 by default, and printed with its max as `aim=MET` or `aim=MISSED`. The rest of a
60 Hz game frame belongs to the game. Each `harden_tick:` also prints `jobs_core_ms`, the jobs' busy time per frame
summed over the jobs, because the game needs those cores too.

**The server.** The library must conserve its counters, and fds, threads and library bytes must stay flat from the
post-warmup sample to the end.

Host-wide kernel counters include traffic that is not the cell's. Run cells on hosts that carry nothing else.

**Strays.** A machine with a public address takes datagrams from the internet at closed ports all day (scanners), and
the kernel's NoPorts counts them with the cell's. On the X710 pair on 2026-11-13 the XDP rows counted one to seven a
row while the library sent and received exactly the same count, so the chain read them as drops the library never had
and failed with a negative gap; the row captured with a log of the ICMP port unreachables each machine sent had one at
each machine, both from one internet host to the machine's public address, inside the measured window. `scripts/harden-pair` therefore installs on each Linux machine an nftables table (`inet
harden_cell`, through `sudo -n`, removed at the end) whose counter `strays` counts the IPv4 UDP datagrams that reach a
local address, find no socket and come from any host but the other end's test address; each role reads it at its cuts
(`harden_load --stray-table harden_cell`). The chain prints

    arrived_udp_noports = cell_noports + stray_noports

and judges only `cell_noports`. A datagram from the other end is never a stray, so the cell's own no-port drops (a
player's socket closed while the server still sent to it) stay in the equation; a stray count above the host's NoPorts
is a broken counter, not applied, and fails the chain. Each role reads the stray counter before NoPorts at its start
and after it at each cut, so a stray that lands between two reads can only raise the gap (a fail). One miss remains, a stray in NoPorts and not
in the counter, which would offset one unnamed loss: a stray to a socket that closes in the microseconds between the
nftables socket lookup and UDP's. Where no counter could be installed (no nftables or no sudo, a
Windows server) the stray count is 0 and the chain is what it was. The teardown's datagrams to the players' closed
ports land after both cuts and are in neither.

**Checking the check.** `scripts/harden-pair --plant-loss N` (also on `scripts/space-frame-row`) drops one in N of the
server's datagrams in nftables on the players' machine before its UDP sees them: a real loss that no counter the chain
reads names. The row must fail on chains with a positive `library_gap` and the line `library gap with no named drop`.
`tests/test_unit_harden_chain.cpp` holds the same rows as a table (clean, unnamed strays, planted loss, strays that try
to cover a loss, duplicates, the refusals with a hostile role) for the unit suite, and
`tests/test_unit_harden_stream.cpp` plants late, reordered and duplicate sequences on one receive stream
(`tests/load/harden_stream.h`): a late arrival takes back the lost or head count it was given, one older than the
window is counted `late_past_window` only, and no count goes below zero.

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
