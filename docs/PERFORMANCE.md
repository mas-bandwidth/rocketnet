# Performance

**1000 players at 60 Hz, sending about 900,000 packets/s at peak.** XDP,
GSO, io_uring, IOCP and RIO reached that rate on the measured machines,
including with simulated game work. Their main-thread network work averaged
0.19–0.93 ms across the per-second p99 readings.

macOS is being measured separately and will be added later.

These are measurements of commits on `main` on November 13, 2026, using
Release development builds. They qualify the workloads, machines and commits
below. They do not establish the same capacity on every machine or qualify a
production build.

| Server transport and machine | Smallest measured network count reaching peak | Peak packets/s, no game work / with game work | Main p99 range (ms/frame) | Result across the four workloads |
| --- | --- | --- | --- | --- |
| XDP, EPYC 9254 / Intel X710 | 1 XDP thread | 900.3k / 900.3k | 0.493–0.586 at 1 thread | All four reach rate; 100% delivery both ways |
| GSO, EPYC 9254 / Intel X710 | 1 worker | 900.3k / 900.2k | 0.495–0.661 | All four reach rate; 100% delivery both ways |
| io_uring, EPYC 9254 / Intel X710 | 1 worker in the two peak rows; 2 in the four-row set | 900.3k / 900.2k at either count | 0.504–0.671 at 2 workers | All four reach rate at 2; small server queue drops in unloaded peak |
| IOCP, AWS m7i.16xlarge | 2 completion workers + 2 send threads | 900.3k / 900.3k | 0.189–0.270 | All four reach rate; input delivery 99.973–100% |
| RIO, AWS m7i.16xlarge | 5 lanes | 900.3k / 900.2k | 0.583–0.899 at 5 lanes | All four reach rate; 100% delivery both ways |

Here and below, **main p99 is the mean of the per-second p99 readings**,
not a percentile calculated over all frames. It measures elapsed network
work on the main thread, not the whole frame or all networking CPU cost.
The detailed rows include maxima, game-thread work, delivery and refusals.

The peak workload offers 900,000 unreliable messages/s plus a small reliable
and protocol overhead. At the report's assumed 1304 wire bytes per packet,
900.3k packets/s is approximately **9.39 Gb/s**, about 94% of a 10 Gb/s link.
This conversion is an estimate from packet rate, not a measured byte counter.
It is not a claim that 10.00 Gb/s of application data reached the clients.

## Workload and measurement

Separate machines run the server and 1000 client players at 60 Hz. Each row
has a nominal 60-second load, with joining and draining around it.

| Traffic | Peak | Average |
| --- | --- | --- |
| Server to each client, every tick | 15 unreliable messages of 1200 bytes | 8 unreliable messages of 1200 bytes |
| Target server message rate | 900,000/s | 480,000/s |
| Server to each client, every 3 seconds | One reliable message of 100 bytes | One reliable message of 100 bytes |
| Each client to server, every tick | One input message of 256 bytes | One input message of 256 bytes |

Each unreliable server message occupies one packet. There are no large
messages. Packet rates include protocol traffic, so they need not equal the
message rate exactly. Average traffic is approximately 5.01 Gb/s at the
reported 480.3k packets/s and the same wire-size assumption.

The four workloads are peak and average traffic, each with and without
simulated game work. In the loaded rows, `--game-load 0.5` requests half a
frame of work per game thread, while `--world-ms 10` requests 10 ms of world
work on the main thread. The unloaded rows set both to zero. These are test
jobs, not a game simulation.

The game owns the thread pool. The test uses enkiTS to split clients among
game threads, which update their clients and write their messages directly.
`--space-send wide --space-update split --space-net task --space-split top`
starts the network task at the top of the frame, overlapping world work.
`--game-threads` counts the main thread as well as the game workers.

The row summarizer selects server reports with `25 <= t <= 55` seconds.
It averages packet-rate, frame-count, game-thread and per-second p99 fields;
its main maximum is the largest reported maximum in that window. Frame
rates below are the report's mean frame counts per approximately one-second
report interval. They are not evidence of an exact 60 Hz clock. Game-thread
min/mean/max entries average the respective per-report values, rather than
reporting the single worst game thread over the whole run.

Delivery percentages and lost-message totals come from the drained player
report and can cover more than the steady timing window. “1000 players”
means all 1000 connected; it does not mean zero packet loss. The measurement
acceptance used a 99.9% player-delivery bound, with offered rate and frame
rate checked separately. A reconciliation PASS alone cannot establish that
all offered traffic was sent: the failing four-lane RIO row is an example.

## Results by transport

In these tables, **game work** is `none` or `half` as defined above. Main and
game times are milliseconds per frame. `s2c` means server to client; `c2s`
means client to server. Network workers are separate from the game pool.

### XDP

One XDP thread reaches both peak and average rate, with and without game
work. All eight rows, including the two-thread comparison, delivered 100%
in both directions with no message-start refusals.

| Row | XDP threads | Load / game work | Packets/s | Frames/s | Main p99 / max | Game min / mean / max |
| --- | ---: | --- | ---: | ---: | --- | --- |
| R5 | 1 | Peak / none | 900.3k | 60.57 | 0.523 / 0.671 | 3.26 / 3.40 / 3.71 |
| R6 | 1 | Peak / half | 900.3k | 60.47 | 0.586 / 0.715 | 2.76 / 3.21 / 3.73 |
| R7 | 1 | Average / none | 480.3k | 60.50 | 0.493 / 0.502 | 1.79 / 1.92 / 2.22 |
| R8 | 1 | Average / half | 480.3k | 60.40 | 0.507 / 0.859 | 1.18 / 1.76 / 2.94 |
| R1 | 2 | Peak / none | 900.3k | 60.37 | 0.497 / 0.510 | 3.32 / 3.46 / 3.75 |
| R2 | 2 | Peak / half | 900.3k | 60.50 | 0.582 / 0.647 | 2.81 / 3.30 / 4.01 |
| R3 | 2 | Average / none | 480.3k | 60.40 | 0.498 / 0.653 | 1.83 / 1.97 / 2.41 |
| R4 | 2 | Average / half | 480.3k | 60.50 | 0.592 / 0.788 | 1.53 / 1.92 / 2.46 |

The reconciliation chain reported FAIL in seven rows because it subtracted
1–7 stray no-port drops even though the libraries' sent and received packet
counts matched exactly. R3 passed the chain check. These failures are
accounting failures, separate from the measured 100% player delivery.

### GSO

One worker reaches all four workloads with the documented socket buffers.
All rows below have 100% delivery both ways and zero message-start refusals.

| Row | Workers | Load / game work | Packets/s | Frames/s | Main p99 / max | Game min / mean / max |
| --- | ---: | --- | ---: | ---: | --- | --- |
| G1 | 1 | Peak / none | 900.3k | 60.47 | 0.528 / 0.749 | 4.26 / 4.38 / 4.66 |
| G2 | 1 | Peak / half | 900.2k | 60.43 | 0.661 / 0.839 | 3.34 / 3.86 / 4.66 |
| G3 | 1 | Average / none | 480.3k | 60.57 | 0.495 / 0.599 | 2.30 / 2.41 / 2.67 |
| G4 | 1 | Average / half | 480.3k | 60.50 | 0.530 / 0.733 | 1.81 / 2.14 / 2.75 |
| G5 | 2 | Peak / none | 900.3k | 60.50 | 0.514 / 0.765 | 3.81 / 3.94 / 4.21 |

The kernel reported 124,107–570,623 `SndbufErrors` per row as the 1 MB send
buffer filled. The worker retained the unsent tail and retried; library
sent and received counts matched exactly. The chain check incorrectly
counted those refused-then-retried sends as drops and failed every s2c row.
G2 and G3 also had the small no-port accounting discrepancy on c2s.
These rows do not show player loss from those send-buffer refusals.

### io_uring

Two workers reach all four workloads. One worker also reaches both peak
workloads on this bare-metal pair; average rows were not repeated at one.
The earlier AWS c6in.8xlarge measurement needed two workers. The machines
and NICs differ, so the new one-worker result does not replace that AWS limit.

| Row | Workers | Load / game work | Packets/s | Frames/s | Main p99 / max | Game min / mean / max | Lost s2c messages |
| --- | ---: | --- | ---: | ---: | --- | --- | ---: |
| U1 | 2 | Peak / none | 900.3k | 60.47 | 0.522 / 0.711 | 4.26 / 4.39 / 4.76 | 30 |
| U2 | 2 | Peak / half | 900.2k | 60.57 | 0.671 / 0.768 | 3.58 / 4.06 / 4.83 | 0 |
| U3 | 2 | Average / none | 480.3k | 60.47 | 0.504 / 0.583 | 2.35 / 2.47 / 2.74 | 0 |
| U4 | 2 | Average / half | 480.3k | 60.50 | 0.508 / 0.604 | 1.87 / 2.27 / 2.98 | 0 |
| U5 | 1 | Peak / none | 900.3k | 60.47 | 0.518 / 0.806 | 4.38 / 4.51 / 4.76 | 0 |
| U6 | 1 | Peak / half | 900.2k | 60.57 | 0.506 / 0.568 | 3.55 / 4.04 / 4.83 | 0 |

All six rows have zero start refusals, 1000 connected players, 100% c2s
delivery, zero pin refusals and zero socket receive drops. U2 recorded one
overrun in 4878 frames; U4 recorded four in 4865; U6 recorded two in 4922.

Unloaded peak with two workers lost packets in all four attempts:

| Attempt | Lost s2c messages | Server qdisc drops | Main p99 / max (ms) | Packets/s | Frames/s |
| --- | ---: | ---: | --- | ---: | ---: |
| U1 | 30 | 2 | 0.522 / 0.711 | 900.3k | 60.47 |
| Repeat 2 | 75 | 5 | 0.500 / 0.674 | 900.3k | 60.43 |
| Repeat 3 | 90 | 6 | 0.498 / 0.742 | 900.3k | 60.60 |
| Repeat 4 | 195 | 13 | 0.509 / 0.815 | 900.3k | 60.57 |

Each qdisc drop corresponds to 15 lost packets: one peak tick's GSO batch.
The server uses `mq` with `fq_codel`; the chain counted a dropped batch as
one packet, leaving 14 packets unnamed per drop. Delivery was
99.9997–100.0000% after rounding, but the loss counts establish that these
were not lossless rows. The other five original rows had no measured loss.

`URING_SEND_DEFERRED` equaled the kernel's `SndbufErrors`: 8.6–10.7 million
at peak and 2.1–2.2 million at average load. In these builds it counts
SENDMSG completions returning `-EAGAIN`, followed by retry after `POLLOUT`.
Those retries also make the chain check fail by being counted as drops.
They are separate from the actual qdisc loss above. The documented 1 MB
send buffer was used without tuning.

### IOCP

Two completion workers and two send threads reach all four workloads.
Server-to-client delivery is 100% in all four; input loss remains visible.

| Row | Load / game work | Packets/s | Frames/s | Main p99 / max | Game min / mean / max | c2s delivery | Lost inputs |
| --- | --- | ---: | ---: | --- | --- | ---: | ---: |
| M1 | Peak / none | 900.3k | 60.87 | 0.190 / 1.344 | 3.05 / 3.14 / 4.83 | 99.995% | 240 |
| M2 | Peak / half | 900.3k | 60.63 | 0.260 / 0.321 | 1.98 / 2.53 / 3.71 | 99.973% | 1219 |
| M3 | Average / none | 480.3k | 60.80 | 0.189 / 0.299 | 1.61 / 1.68 / 3.63 | 100% | 0 |
| M4 | Average / half | 480.3k | 60.63 | 0.270 / 0.310 | 1.02 / 1.35 / 2.38 | 99.984% | 726 |

M1 and M2 recorded 738 and 7405 message-start refusals over the run; the
harness retried and reached the offered rate. M3 and M4 had none. The
reported overruns were 0, 3, 0 and 3 respectively. M1 includes a 1.344 ms
main-thread receive spike, despite its low mean per-second p99.

The chain check passed M3 and failed the other three, leaving c2s gaps of
246, 1282 and 750 packets unnamed. ENA receive-discard counters rose while
UDP receive errors stayed zero, consistent with loss below IOCP. They also
rose in M3, which lost no player inputs, so these machine counters do not
attribute every discard to a player or isolate its time within the run.
The failed checks are not merely the stray no-port accounting issue.

With one completion worker and two send threads, loaded peak fell to
371.1k packets/s with about 567k starts refused/s. In that run the send
threads shared processors with RSS. This establishes the limit of that
measured layout, not an isolated worker-count comparison. The measured main
build fixes send threads at two; the earlier branch's one-send-thread test
also missed peak (696.7k packets/s, 47 frames/s).

### RIO

Five lanes reach all four workloads; six also pass. Four lanes fail the
peak rate even though the packets actually sent are delivered successfully.
All rows in the table have 100% delivery both ways and pass reconciliation.

| Row | Lanes | Load / game work | Packets/s | Frames/s | Main p99 / max | Game min / mean / max |
| --- | ---: | --- | ---: | ---: | --- | --- |
| RIO-R5 | 5 | Peak / none | 900.3k | 60.57 | 0.587 / 0.828 | 2.89 / 3.02 / 4.10 |
| RIO-R7 | 5 | Peak / half | 900.2k | 60.60 | 0.840 / 1.255 | 2.15 / 2.66 / 3.64 |
| RIO-R8 | 5 | Average / none | 480.3k | 60.50 | 0.899 / 1.053 | 1.51 / 1.59 / 2.39 |
| RIO-R9 | 5 | Average / half | 480.3k | 60.50 | 0.583 / 0.869 | 1.13 / 1.46 / 2.27 |
| RIO-R1 | 6 | Peak / none | 900.3k | 60.47 | 0.799 / 0.889 | 2.97 / 3.09 / 4.00 |
| RIO-R2 | 6 | Peak / half | 900.3k | 60.50 | 0.931 / 1.370 | 2.25 / 2.78 / 3.83 |
| RIO-R3 | 6 | Average / none | 480.3k | 60.63 | 0.912 / 1.150 | 1.56 / 1.63 / 3.20 |
| RIO-R4 | 6 | Average / half | 480.3k | 60.60 | 0.608 / 0.678 | 1.12 / 1.47 / 2.57 |
| RIO-R6 | 4 | Peak / none | 545.1k | 60.40 | 1.994 / 2.476 | 1.66 / 1.77 / 3.80 |

The five- and six-lane rows have zero start refusals. Four lanes refuse
about 374k message starts/s; their 100% delivery describes only what was
sent. The loaded-peak five-lane and loaded-average six-lane reports record
0.03 overruns/s. The previous branch's five-lane failure preceded the larger
send capacity; the measured main result supersedes that limit.

## Time available for the game

Main-thread network time alone understates networking cost: game threads
build and encrypt messages, and dedicated network workers perform I/O.
The following loaded rows show the main-to-game distribution and the
measured remaining game-thread budget.

| Loaded row | Main network mean (ms) | Mean game network (ms) | Main / mean game | Mean game-thread time left (ms) | State-to-send mean (ms) |
| --- | ---: | ---: | ---: | ---: | ---: |
| XDP, 1 thread, peak | 0.456 | 3.21 | 0.14 | 5.12 | 10.19 |
| XDP, 1 thread, average | 0.429 | 1.76 | 0.24 | 6.57 | 9.84 |
| GSO, 1 worker, peak | Not supplied | 3.86 | 0.12 | 4.48 | 10.52 |
| GSO, 1 worker, average | Not supplied | 2.14 | 0.21 | 6.19 | 9.50 |
| io_uring, 2 workers, peak | 0.445 | 4.06 | 0.11 | 4.27 | 10.58 |
| io_uring, 2 workers, average | 0.443 | 2.27 | 0.20 | 6.06 | 9.63 |
| IOCP, 2 completion + 2 send, peak | 0.162 | 2.53 | 0.06 | 5.80 | 10.96 |
| IOCP, 2 completion + 2 send, average | 0.161 | 1.35 | 0.12 | 6.98 | 9.81 |
| RIO, 5 lanes, peak | 0.457 | 2.66 | 0.17 | 5.67 | 10.89 |
| RIO, 5 lanes, average | 0.471 | 1.46 | 0.32 | 6.88 | 9.78 |

“Time left” subtracts the measured game work and network work per game
thread from the frame budget. It is not simply the unused half of a frame.
It is a mean budget estimate, not a guarantee for the slowest thread or
frame, and excludes the dedicated network workers. State-to-send latency
runs from the world state used for the packet to its send call; it is not
network transit time. Without game work the recorded means are roughly
18–23 ms; starting the network task alongside world work changes that
pipeline, giving the loaded rows above roughly 9–11 ms.

## Machines, placement and source commits

| Rows | Server | Clients and network | Server placement |
| --- | --- | --- | --- |
| XDP | Latitude m4.metal.large, AMD EPYC 9254, 24 cores; Intel X710 | Second m4.metal.large; 10 GbE | OS CPU 0; two-thread rows: XDP CPUs 1–2, main 3, 20 game workers 4–23; one-thread rows: XDP CPU 1, main 2, 21 game workers 3–23 |
| GSO and io_uring | Same bare-metal X710 server; GCC 15.2; io_uring kernel 7.0.0-43-generic, liburing 2.14 | Same X710 client machine; 32 client threads | OS CPU 0; network worker(s) CPU 1 (and 2); main 3; 20 game workers 4–23 |
| IOCP | AWS m7i.16xlarge, 64 logical processors, Windows Server 2025, MSVC Release | AWS c8gn.8xlarge, 16 client threads; same subnet in us-east-1d | RSS processors 2,4; completion workers 3,5; send threads 6,8; main 10; 53 game workers 11–63 |
| RIO | AWS m7i.16xlarge, Windows Server 2025, MSVC Release | AWS c8gn.8xlarge, 32 client threads; same subnet and cluster placement group in us-east-1a | Lane i: RSS processor 2i, worker 2i+1; five lanes: main 10, 53 game workers 11–63; six lanes: main 12, 51 game workers 13–63 |

The Linux library pins its own network workers. The bare-metal servers
retain `isolcpus=1-2`; XDP provisioning matches NIC queues to XDP threads.
GSO and io_uring run unprivileged: requests for real-time priority were
refused, so their network workers ran at default priority. io_uring's
memlock limit was 8 MiB. Both use granted receive/send socket buffers of
25,000,000/1,000,000 bytes. These are different machines from the older
AWS GSO and io_uring runs.

Windows rows bind a specific IPv4 address and steer RSS to the receive
processors. IOCP uses the development layout override
`ROCKETNET_TEST_IOCP_LAYOUT=2,4/3,5`; RIO uses built-in placement from core 0.
Their results are not evidence that an unconfigured wildcard bind and
default RSS achieve the same throughput.

| Transport | Measured main commit | Evidence row IDs |
| --- | --- | --- |
| XDP | [59890655d](https://github.com/mas-bandwidth/rocketnet/commit/59890655d1f35133732073d6b1786a1407a134c7) | R1–R8, `rm-xdp-{peak,avg}-{noload,load}-q{1,2}` |
| GSO | [3218cf5f6](https://github.com/mas-bandwidth/rocketnet/commit/3218cf5f64dbad2babdd5a2cfc98bb7d9a53884c) | G1–G5, `rm-gso-doc-{peak,avg}-{noload,load}-q{1,2}` |
| io_uring | [3afea2aed](https://github.com/mas-bandwidth/rocketnet/commit/3afea2aed6f010a043ae194760a719f51f9b2380) | U1–U6, `rm-uring-{peak,avg}-{noload,load}-q{1,2}`; U1 repeats `-r2` through `-r4` |
| IOCP | [3218cf5f6](https://github.com/mas-bandwidth/rocketnet/commit/3218cf5f64dbad2babdd5a2cfc98bb7d9a53884c) | `m1-peak-noload`, `m2-peak-load`, `m3-avg-noload`, `m4-avg-load`, `m5-peak-load-r1` |
| RIO | [3218cf5f6](https://github.com/mas-bandwidth/rocketnet/commit/3218cf5f64dbad2babdd5a2cfc98bb7d9a53884c) | RIO-R1–R9, `rm-rio-{peak,avg}-{noload,load}-l{4,5,6}` |

Server, clients and reconciliation binaries used each row's matching
commit. The run records are maintained in `REMEASURE-MAIN.md` under the
measurement archive `review-2026-11-10/final-fable`, with Linux cells in
`harden-pair-cells` and Windows cells in the corresponding
`remeasure-iocp` and `remeasure-rio` directories.
The archive is separate from this repository. The commands below make the
method repeatable; this page does not substitute for those raw logs.

### Per-transport thread settings

The measured commits predate the dedicated count fields. They use the old
`num_xdp_queues` field and load-tool `--num-xdp-queues` / controller
`--xdp-queues` flag for several transports. Those historical names in the
reproduction commands below are intentional.

[PR #1115](https://github.com/mas-bandwidth/rocketnet/pull/1115) introduces
one field and environment setting per transport. At the time of this
update it is open; the following names and defaults were checked against
its public header at `f5cf0354d70c46fcb0f4eff1af3ab93e5829015f`. They are
not fields present in the measured commits. Check the header of the
version you build before using them.

| Transport | Config field | Environment variable | Proposed default | Measurement supporting the count |
| --- | --- | --- | ---: | --- |
| XDP | `num_xdp_threads` | `ROCKETNET_SERVER_NUM_XDP_THREADS` | 1 | R5–R8; match the NIC queue count |
| GSO | `num_gso_workers` | `ROCKETNET_SERVER_NUM_GSO_WORKERS` | 1 | G1–G4 |
| io_uring | `num_io_uring_workers` | `ROCKETNET_SERVER_NUM_IO_URING_WORKERS` | 2 | U1–U4; earlier AWS loaded peak needed 2 |
| RIO | `num_rio_lanes` | `ROCKETNET_SERVER_NUM_RIO_LANES` | 5 | RIO-R5/R7/R8/R9; 4 lanes miss peak |
| IOCP receive | `num_iocp_completion_workers` | `ROCKETNET_SERVER_NUM_IOCP_COMPLETION_WORKERS` | 2 | M1–M4 |
| IOCP send | `num_iocp_send_threads` | `ROCKETNET_SERVER_NUM_IOCP_SEND_THREADS` | 2 | M1–M4 |

In that change, zero selects the default and the environment overrides the
config field. Windows needs a specific bind address for multiple RIO lanes
or IOCP completion workers: an unspecified count on a wildcard bind falls
back to one. See [Windows](WINDOWS.md), [Linux](LINUX.md)
and [Deployment](DEPLOYING.md) for the version's setup.

## Run it yourself

Build the row's commit on both machines and a matching local reconciliation
binary on the controller. These commands repeat the measured revisions;
newer versions can change flags, placement and results. See
[Building](BUILDING.md) for dependencies and [Load testing](LOAD.md) for the
load tools. Use a fresh output directory for each run.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DROCKETNET_DEV=ON -DROCKETNET_BUILD_TOOLS=ON \
  -DROCKETNET_BUILD_DEV_LIBRARIES=ON \
  -DCMAKE_RUNTIME_OUTPUT_DIRECTORY="$OUTPUT_DIR/bin"
cmake --build build --config Release --target harden_load --parallel 4
```

`OUTPUT_DIR` is an absolute output directory. On multi-configuration
Windows builds, locate the resulting Release executable. XDP needs its
AF_XDP dependencies and BPF-capable clang; io_uring needs liburing and
`ROCKETNET_IO_URING=ON`. Confirm in CMake output that the requested transport
was included, and in `harden_ready` that it was actually granted.

### Linux rows

On the XDP server, match the NIC's combined queues to the selected count
and verify provisioning at the measured commit:

```sh
sudo scripts/rocketnet-provision "$SERVER_DEV" 1 provision
sudo scripts/rocketnet-provision "$SERVER_DEV" 1 verify
```

Provisioning changes queue and interrupt settings. Retain the existing
isolated CPU range 1–2 for comparison with these rows. Then, from the
controller, run the one-thread loaded peak row:

```sh
scripts/space-frame-row \
  --server-host "$SERVER_HOST" --client-host "$CLIENT_HOST" \
  --server-ip "$SERVER_IP" --remote "$SERVER_CHECKOUT" \
  --client-remote "$CLIENT_CHECKOUT" --local-bin "$LOCAL_BIN" \
  --out "$OUT_DIR" --server-dev "$SERVER_DEV" --client-dev "$CLIENT_DEV" \
  --load peak --game-threads 22 --first-cpu 2 --queues 1 \
  --game-load 0.5 --world-ms 10 --split top --input-bytes 256 \
  --seconds 60 --clients 1000 --threads 32
```

The script uses passwordless `sudo -n` for the XDP server. For two XDP
threads, provision two queues and use `--queues 2 --first-cpu 3
--game-threads 21`. Change load to `average` for average rows; set game
load and world work to zero for unloaded rows.

GSO and io_uring use `scripts/harden-pair` directly. On the measured server,
the socket limits were set as follows and restored afterward:

```sh
sudo sysctl -w net.core.rmem_max=25000000 net.core.wmem_max=1000000
```

Set `TRANSPORT` to `gso` or `io_uring`, `NETWORK_THREADS` to 1 or 2,
`LOAD` to `peak` or `average`, and `GAME_LOAD`/`WORLD_MS` to `0`/`0` or
`0.5`/`10`. Set host, path and interface variables for the two machines.

```sh
scripts/harden-pair \
  --server-host "$SERVER_HOST" --client-host "$CLIENT_HOST" \
  --server-ip "$SERVER_IP" --remote "$SERVER_CHECKOUT" \
  --client-remote "$CLIENT_CHECKOUT" --local-bin "$LOCAL_BIN" \
  --out "$OUT_DIR" --server-dev "$SERVER_DEV" --client-dev "$CLIENT_DEV" \
  --transport "$TRANSPORT" --row space --xdp-queues "$NETWORK_THREADS" \
  --seconds 60 --clients 1000 --threads 32 \
  --server-args "--frame-shape space --game-threads 21 --pin-first-cpu 3 --space-send wide --space-update split --space-net task --space-split top --game-load $GAME_LOAD --world-ms $WORLD_MS --load $LOAD" \
  --client-args "--input-bytes 256"
```

Unlike XDP, these worker counts do not require matching the NIC queue
count. The measured NIC retained two combined queues. Run unprivileged
to repeat the reported default-priority GSO/io_uring setup. Read the limits
before changing them with `sysctl net.core.rmem_max net.core.wmem_max` and
restore those recorded values after the session.

### Windows rows

Use the matching Windows and Linux builds with `scripts/harden-pair
--server-os windows`. Set the specific bind address to the server's private
address and match RSS to the intended lane processors before starting.
The recorded five-lane RIO configuration was:

```powershell
Set-NetAdapterRss -Name 'Ethernet 2' -BaseProcessorNumber 0 -MaxProcessorNumber 8 -MaxProcessors 5 -NumberOfReceiveQueues 5
Get-NetAdapterRss -Name 'Ethernet 2'
```

Use the adapter's actual name. Six-lane comparison rows used maximum
processor 10 and six processors/queues. IOCP used two RSS queues spanning
processors 2 and 4. Retain the read-back output, rather than assuming the
request was granted.

From a POSIX controller with SSH access to both machines, use a Windows
checkout path such as `C:/rn/main` for `SERVER_CHECKOUT`, and the Linux
checkout path for `CLIENT_CHECKOUT`. `SERVER_IP` is the server's private
IPv4 address without a port; the controller uses port 40000. `LOCAL_BIN`
is the controller's directory containing the matching `harden_load`.
Select the four workloads with `LOAD=peak` or `average` and
`GAME_LOAD=0 WORLD_MS=0` or `GAME_LOAD=0.5 WORLD_MS=10`.

For IOCP, set the measured RSS layout on the Windows server (replace the
adapter name), then run the controller command:

```powershell
Set-NetAdapterRss -Name 'Ethernet 2' -BaseProcessorNumber 2 -MaxProcessorNumber 4 -MaxProcessors 2 -NumberOfReceiveQueues 2
Get-NetAdapterRss -Name 'Ethernet 2'
```

```sh
scripts/harden-pair \
  --server-host "$SERVER_HOST" --client-host "$CLIENT_HOST" \
  --server-ip "$SERVER_IP" --remote "$SERVER_CHECKOUT" \
  --client-remote "$CLIENT_CHECKOUT" --local-bin "$LOCAL_BIN" --out "$OUT_DIR" \
  --server-os windows --transport iocp --row bar --xdp-queues 2 \
  --client-dev ens5 --seconds 60 --clients 1000 --threads 16 \
  --server-env "ROCKETNET_SERVER_BIND_IPV4_ADDRESS=$SERVER_IP" \
  --server-env "ROCKETNET_TEST_IOCP_LAYOUT=2,4/3,5" \
  --server-args "--frame-shape space --load $LOAD --game-threads 54 --pin-first-cpu 10 --game-load $GAME_LOAD --world-ms $WORLD_MS --space-send wide --space-update split --space-net task --space-split top" \
  --client-args "--input-bytes 256"
```

For RIO, apply the five-lane RSS command above and run:

```sh
scripts/harden-pair \
  --server-host "$SERVER_HOST" --client-host "$CLIENT_HOST" \
  --server-ip "$SERVER_IP" --remote "$SERVER_CHECKOUT" \
  --client-remote "$CLIENT_CHECKOUT" --local-bin "$LOCAL_BIN" --out "$OUT_DIR" \
  --server-os windows --transport rio --row space --xdp-queues 5 \
  --client-dev ens34 --seconds 60 --clients 1000 --threads 32 \
  --server-env "ROCKETNET_SERVER_BIND_IPV4_ADDRESS=$SERVER_IP" \
  --server-args "--frame-shape space --load $LOAD --game-threads 54 --pin-first-cpu 10 --game-load $GAME_LOAD --world-ms $WORLD_MS --space-send wide --space-update split --space-net task --space-split top" \
  --client-args "--input-bytes 256"
```

`ens5` and `ens34` are the recorded client interfaces; substitute the actual
interface when repeating on another host. For six RIO lanes, set six RSS
queues through processor 10 and use `--xdp-queues 6`, pool 52 and main
processor 12. For four, set four RSS queues through processor 6 and use
count 4, pool 56 and main processor 8. Restore the original RSS settings
after a comparison session.

### Read and profile a row

The controller saves each role's log and runs reconciliation. To repeat
that check directly with the matching binary revision:

```sh
bin/harden_load reconcile --logs "$SERVER_LOG,$CLIENT_LOG" \
  --min-delivery 99.9 --frame-p99-us 5000
```

Keep `server.log`, `clients.log`, `reconcile.txt`, command lines, source
commits, build configuration, affinity/RSS read-back and host counters.
Check all of the following together:

- `harden_ready`: actual transport, buffer sizes and thread setup.
- `harden_tick`: connected players, sent packet rate, refused starts and frames.
- `harden_frame`: per-second main p99/max, latency, join wait and overruns.
- `harden_threads`: game-thread network work and remaining-time accounting.
- `harden_players`: connected players, loss and delivery in each direction.
- Reconciliation chains: named drops and gaps, with the known limits above.

### Linux CPU profiles: XDP, GSO and io_uring

A short Linux CPU profile can explain where network and game threads spend
time. Record it during a steady interval of the same row, using the actual
server process ID and a new output directory. Start when `harden_tick`
reaches about t=40, keeping the capture inside the load interval. Obtain
the PID from the server session and verify it identifies the server role;
several `harden_load` processes can otherwise be confused.

```sh
mkdir -p "$PROFILE_DIR"
ps -L -p "$SERVER_PID" -o pid,tid,psr,comm > "$PROFILE_DIR/threads.txt"
sudo perf record -F 999 -g -p "$SERVER_PID" -o "$PROFILE_DIR/perf.data" -- sleep 8
sudo perf report -f -i "$PROFILE_DIR/perf.data" --no-children \
  --sort pid,symbol -n --stdio -g none > "$PROFILE_DIR/perf.rep"
```

Save the thread IDs and names from `/proc/$SERVER_PID/task` during capture,
match them to the report and separate main, game and network workers.
Inspect `perf evlist -i "$PROFILE_DIR/perf.data"` for the selected event.
Self samples are usable without reliable stack unwinding; call graphs need
a suitable build. Requested sample frequency is not an exact CPU timer.
Use the capture's actual duration and frame rate when estimating time per
frame, and keep kernel work in the accounting.

For a rough sampling estimate, sum a function's self samples over one
thread class and divide by `999 * capture_seconds * measured_frames_per_second`,
then multiply by 1000 for core-ms/frame. Divide again by the number of game
or network workers only when reporting the mean worker. This assumes the
requested sampling frequency; elapsed-call timings and sampled CPU time
measure different things. Preserve total CPU cost and the unlisted tail.

### Windows profiles: IOCP and RIO

Repeat the chosen controller command in a new output directory, adding
these controller options before the command ends:

```sh
--profile-after 30 --profile-seconds 10
```

These are options to `scripts/harden-pair`, not a separate shell command.
The script forwards them to the Windows server and copies its `profile.txt`
into `OUT_DIR`. For a direct Windows server invocation, the equivalent
arguments are `--profile-after 30 --profile-seconds 10 --profile-out profile.txt`.
Keep the matching executable and its symbols together; retain the recorded
Release optimization settings when comparing runs.

The built-in profiler in `tests/load/harden_load.cpp` takes a ten-second
window starting 30 seconds after its profiler starts. It samples the main
thread's program counter approximately once per millisecond and reads CPU
time for every accessible process thread. It does not sample the game or
network workers' call stacks.

Read the output as follows:

- `harden_profile`: actual window duration, main sample count and whether
  DbgHelp was available. A nonempty file with zero samples is not a profile.
- `harden_profile_thread`: thread ID, CPU percentage of one core, main-thread
  marker, creation time and start routine. Match the worker count and creation
  order to the startup layout. A raw address indicates unresolved symbols.
- `harden_profile_phase`: main-thread samples by update, receive, write,
  send, sleep and other phase.
- `harden_profile_main` / `harden_profile_in`: flat main-thread function
  shares overall and within each phase. Sleep samples are elapsed waiting,
  not networking CPU consumption.

Convert a thread's CPU percentage to core-ms/frame with
`cpu_percent / 100 * 1000 / measured_frames_per_second`, and sum the network
workers for total network-worker CPU cost. The profiler periodically
suspends the main thread and itself uses a thread and handles. Keep an
unprofiled rate/delivery row beside it; its handle growth and timing changes
are not evidence of a transport leak or an unprofiled performance result.
The main-run evidence above supplies elapsed row timings; it does not
supply a new Windows per-function table.

The older [function profiles](https://github.com/mas-bandwidth/rocketnet/blob/2da198e234dab41c118f58fa3e0ec263b562c1d4/docs/PERFORMANCE.md#profiles)
and [hostile-traffic experiment](https://github.com/mas-bandwidth/rocketnet/blob/2da198e234dab41c118f58fa3e0ec263b562c1d4/docs/PERFORMANCE.md#xdp-under-hostile-traffic)
remain available at their original document revision. They used measurement
branches, including AWS io_uring hardware, and are not new profiles of the
main commits in this report. This update replaces their throughput headline
with the main measurements above; it does not claim that those older
function costs or hostile-traffic results were requalified on main.
