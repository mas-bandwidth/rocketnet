# Performance

RocketNet's load target is **1000 players at 60 Hz**, with **900,000 server
packets/s at peak** and **480,000/s at average load**. Meeting the offered
rate, delivering the traffic and receiving it promptly are separate checks.
A harness PASS alone does not establish all three.

## Current qualification status

The following results are the measurement owner's current reported results
for the specified configurations. Some were measured on fixes awaiting
integration; they do not qualify an unmeasured assembled release. The GSO receive-handoff fix is on main; the XDP, RIO and IOCP
receive-handoff fixes are awaiting the hot-path integration. io_uring does
not require that receive-delay repair. Raw run
receipts are not published in this repository. Treat the numbers as scoped
measurement records, not a guarantee for another machine or workload.

| Server transport | Measured configuration | Throughput and refusals | Receive result and remaining limit |
| --- | --- | --- | --- |
| XDP | Linux, AMD EPYC 9254 / Intel X710, one XDP thread | The 1000-player peak rate is established on this rig | Receive-handoff changes require the corresponding final-tree measurement record before claiming that the current build also meets the receive-delay target |
| io_uring | AWS c8gn.8xlarge server / c7gn.16xlarge clients, two workers | All twelve rows passed: four workloads, three repetitions; about 900.3k packets/s at peak, 480.3k at average, zero refused starts or lost messages | Reported receive delay mean 6.3–10.5 ms, p99 14.5–18.8 ms; measured tree and placement only |
| GSO | Linux X710 pair, one worker, receive-handoff fix | All four workloads passed in three repetitions | Reported receive wait mean 5.9–12.3 ms, p99 13.8–19.3 ms; this does not establish the same capacity on the AWS rig |
| GSO | AWS pair, one worker | Loaded peak misses the offered rate: 845–877k packets/s; one sample refused 44,696 starts. Each average workload also failed one of three samples (772 and 807 refused starts). Two workers passed loaded peak in three samples | Two-worker result covers that workload, not a complete four-workload qualification. The supplied summary does not identify a full commit pair for this follow-up |
| RIO | Windows m7i.16xlarge server, configured RSS, receive-handoff fix | Six lanes passed all four workloads in three repetitions with zero refused starts. Five-lane follow-up samples also passed | Reported receive wait mean 5.5–11.1 ms at six lanes; five-lane samples 4.8–8.8 ms. Placement and NIC interrupt load matter |
| IOCP | Windows m7i.16xlarge, two completion workers and two send threads, receive-handoff fix | Approximately 898–900k packets/s, but loaded peak still refuses 18.7k–56.5k starts per run | Reported receive delay mean 7–11 ms. Lower receive delay does not close the peak refusal limit |
| msgx | Mac Studio M3 Ultra, Aquantia 10GBASE-T, three transmit workers | Average reaches about 480k packets/s without refused starts. No measured peak row reaches 900k | Address topology and interface contention affect throughput; the macOS timing column below is state-to-send, not receive delay |

**RIO is the Windows choice for this peak workload**, with the measured RSS
and worker placement; IOCP is the fallback. IOCP's remaining peak refusals make its near-target
packet rate insufficient for the same qualification. Configure and verify
placement using [Windows](WINDOWS.md); a default wildcard bind is not a
substitute for the measured setup. Windows measurements used Windows Server
2025 and do not expand the Windows 11 support row in
[Platforms](PLATFORMS.md).

The receive summaries above do not provide a complete timestamp-origin
formula. Their reported receive wait/delay is not RTT or state-to-send time,
and a nonzero value does not by itself identify an extra transport update.
Do not infer a universal zero-added-delay claim from these means. The final
build must meet both its rate and receive-delay criteria on the target rig.

These figures also do not establish the less-than-100-microsecond main-call
target at 1000 players. A mean of per-second p99 readings is not the whole
run's p99. Keep whole-run percentiles, per-second samples and maxima distinct.

## Workload and measurement

Use separate server and client machines. Connect all 1000 players and report
peak and average traffic, each with and without game work.

| Traffic | Peak | Average |
| --- | --- | --- |
| Server to each client, every tick | 15 unreliable messages of 1200 bytes | 8 unreliable messages of 1200 bytes |
| Target server message rate | 900,000/s | 480,000/s |
| Server to each client, every 3 seconds | One reliable message of 100 bytes | One reliable message of 100 bytes |
| Each client to server, every tick | One input message of 256 bytes | One input message of 256 bytes |

Each unreliable server message occupies one packet. Packet rates also
include protocol traffic, so they need not equal the message target exactly.
There are no large messages in this workload. Use the effective settings
printed by `harden_ready`; preset expansion can override a command-line
value. In particular, do not infer that reliable traffic is absent merely
because an input option was zero.

The game owns its thread pool. The load tool uses enkiTS with
`--space-send wide --space-update split --space-net task --space-split top`.
`--game-threads` includes the main thread. Loaded rows use `--game-load 0.5`
and `--world-ms 10`; unloaded rows set both to zero. These are synthetic work
requests, not a game simulation. Dedicated network workers are additional
CPU cost; main-thread network time alone does not account for them.

For repeatable runs, retain warmup, sample and drain durations and use the
same analysis window. Report the number of repetitions. Delivery covers the
drained traffic, while timing and rate may use a steady sample window; do
not divide a whole-run counter by only the sample duration. The measurement
criterion used a 99.9% player-delivery bound, with offered rate and frame
rate checked separately. “1000 connected” does not mean zero packet loss.

At an assumed 1304 wire bytes per packet, 900,000 packets/s is approximately
9.39 Gb/s. This is a conversion from packet rate, not a measured byte counter
or a claim that 10 Gb/s of application data reached the players. Record actual
wire counters separately when available.

## macOS results and limits

The current msgx record uses a Mac Studio M3 Ultra (32 cores), its Aquantia
10GBASE-T interface, and a Linux client machine on the same switch. It uses
1000 players in 12 client threads, three transmit workers and 28 game
threads, with a 20-second warmup and 60-second sample. Worker sweeps use
30-second samples. The exact macOS version and full build configuration are
not included in the supplied summary, so these rows do not establish a
platform-wide or production-build qualification.

| Workload | Client addresses | Packets/s | Refused starts over run | State-to-send mean / max (ms) | Minimum frames/s / overruns |
| --- | ---: | ---: | ---: | --- | --- |
| Average, game work | 1 | 480,106 | 0 | 9.60 / 17.16 | 59 / 7 |
| Average, no game work | 1 | 480,344 | 0 | 20.99 / 28.88 | 60 / 0 |
| Peak, game work | 1 | 712,942 | 15,401,906 | 10.62 / 26.11 | 59 / 61 |
| Peak, no game work, address comparison | 1 | 634,373 | 21,496,126 | 20.28 / 43.59 | 60 / 0 |
| Peak, no game work, address comparison | 50 | 808,063 | 7,599,998 | 21.50 / 28.60 | 60 / 0 |
| Peak, game work, address comparison | 1 | 620,042 | 21,574,003 | 11.07 / 63.09 | 44 / 407 |
| Peak, game work, address comparison | 50 | 760,715 | 10,283,417 | 11.61 / 36.68 | 48 / 351 |

These are selected rows, not best-case capacity guarantees. Across the
address-comparison samples without the separately identified interfering test
job, unloaded peak averaged 640,936 packets/s at one address (three samples)
and 803,543 at 50 addresses (two samples). Loaded peak averaged 635,694 at one
address (two samples) and 760,715 at 50 (one sample). Background processes
still used several cores; “without the interfering job” does not mean idle.

At 50 addresses, two, three and four transmit workers recorded 692,366,
756,731 and 813,633 packets/s respectively in the 30-second sweep. All still
refused starts and missed peak. The game-thread count changed with the worker
count, and background activity remained. These samples do not establish four
workers as a general default; the shipped default is three.

The reported delivery checks passed for traffic sent, approximately
99.97–99.99%, while **every peak row missed the offered-rate target**.
State-to-send runs from frame-state construction to the send call; it is not
receive-handoff delay or network transit time. Source-address diversity can
change contention, but does not promise a throughput increase on another
driver or topology. Use the setup and profiling advice in [macOS](MACOS.md).

## Network thread settings

Each native server transport has its own configuration field and environment
setting. Zero selects the transport default, and the environment overrides
the config field.

| Transport | Config field | Environment variable | Default |
| --- | --- | --- | ---: |
| XDP | `num_xdp_threads` | `ROCKETNET_SERVER_NUM_XDP_THREADS` | 1 |
| GSO | `num_gso_workers` | `ROCKETNET_SERVER_NUM_GSO_WORKERS` | 1 |
| io_uring | `num_io_uring_workers` | `ROCKETNET_SERVER_NUM_IO_URING_WORKERS` | 2 |
| RIO | `num_rio_lanes` | `ROCKETNET_SERVER_NUM_RIO_LANES` | 5 |
| IOCP receive | `num_iocp_completion_workers` | `ROCKETNET_SERVER_NUM_IOCP_COMPLETION_WORKERS` | 2 |
| IOCP send | `num_iocp_send_threads` | `ROCKETNET_SERVER_NUM_IOCP_SEND_THREADS` | 2 |
| msgx | `num_msgx_transmit_workers` | `ROCKETNET_SERVER_NUM_MSGX_TRANSMIT_WORKERS` | 3 |

Windows needs a specific bind address for multiple RIO lanes or IOCP
completion workers; an unspecified count on a wildcard bind falls back to
one. Inspect the granted transport and thread layout at startup. See
[Windows](WINDOWS.md), [Linux](LINUX.md) and [Deployment](DEPLOYING.md).

## Run and assess the current build

Build matching server, client and reconciliation tools from the exact commit
being qualified. Use [Building](BUILDING.md) and [Load testing](LOAD.md) for
the commands and prerequisites. Use a fresh output directory and record the
build mode; Release optimization and `ROCKETNET_DEV` are independent.

For `scripts/harden-pair`, pass the transport's count within `--server-args`:

| Transport | Server arguments for the default count |
| --- | --- |
| XDP | `--num-xdp-threads 1` |
| GSO | `--num-gso-workers 1` |
| io_uring | `--num-io-uring-workers 2` |
| RIO | `--num-rio-lanes 5` |
| IOCP | `--num-iocp-completion-workers 2 --num-iocp-send-threads 2` |
| msgx | `--num-msgx-transmit-workers 3` |

The controller's `--xdp-queues` option affects XDP only. It is not a shared
worker-count switch. Provision XDP queues and interrupts for the requested
count. Verify Windows RSS against the worker processors actually logged.
Read back requested placement and socket settings instead of assuming the OS
granted them. Retain and restore machine settings changed for a measurement.

Keep each role's log, reconciliation result, command line, source commit,
build configuration, transport grant, affinity/RSS read-back and host
counters. Inspect these together:

- `harden_ready`: actual workload, transport, buffers and worker setup.
- `harden_tick`: connected players, sent rate, refused starts and frames.
- `harden_frame`: timing, latency, join waits and overruns.
- `harden_frame_run`: whole-run p99, p99.9 and maximum; do not replace them
  with a mean of per-second percentiles.
- `harden_threads`: game-thread network cost and remaining-time accounting.
- `harden_players`: delivery and loss in each direction after draining.
- Reconciliation: named drops and unexplained gaps, including the chain
  verdict separately from the player-delivery verdict.

A failed chain is not a clean qualification merely because player delivery
passed. Retry counters are not automatically lost packets, and successful
send calls are not receipts. Keep accepted refusals, actual loss and
unexplained accounting gaps visible.

Profile only after a repeatable unprofiled row. Separate main, game and
network-worker cost, retain kernel work, and record profiler overhead. Sampled
CPU time, elapsed call time, state-to-send and receive delay answer different
questions. Report their units and definitions rather than combining them into
one latency figure.

## Evidence scope

The current summaries were supplied on November 13, 2026. AWS io_uring's
record identifies server source `4b9a1591a` plus measurement-harness changes;
the Windows receive-fix series identifies `8c58f1946` plus its harness. The
msgx record identifies server `25fca1774` and address-comparison clients
`aa9d85c50`; the other msgx client rows use `25fca1774`. These identifiers bound
the reported results to the measured builds. Several supplied aggregate
rows omit the full server/client/harness commit pair. Capture that pair and
the raw receipts when qualifying the final integrated build.
