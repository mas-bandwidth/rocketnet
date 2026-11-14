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
| XDP | Linux, AMD EPYC 9254 / Intel X710, receive-handoff fix, one or two XDP threads | Both counts pass all four workloads in three repetitions: about 900.3k packets/s at peak, zero refused starts or lost messages | Receive wait mean 4.3–11.6 ms across the rows; late_1ms 0.06–0.46%. Fix awaits hot-path integration and final-tree qualification |
| io_uring | AWS c8gn.8xlarge server / c7gn.16xlarge clients, two workers | All eighteen rows passed across four workloads, including nine loaded-peak samples; 899.6–900.3k packets/s at peak, 480.3k at average, zero refused starts or lost messages | Receive delay mean 4.30–10.88 ms, p99 11.00–20.33 ms; late-by-more-than-1-ms share 0.008–0.208% |
| GSO | Linux X710 pair, one worker, receive-handoff fix | All four workloads passed in three repetitions | Reported receive wait mean 5.9–12.3 ms, p99 13.8–19.3 ms; this does not establish the same capacity on the AWS rig |
| GSO | Same AWS pair, one worker | Loaded peak fails all six samples: 842.0–871.6k packets/s, 56.67–58.57 frames/s. Average with game work passes two of three; other workloads pass three of three. Zero refused starts and zero lost messages in all samples | Receive delay is fixed on main; the one-worker loaded-peak limit is rate/frame time. Two workers pass loaded peak three of three; other two-worker workloads were not measured |
| RIO | Windows m7i.16xlarge server, configured RSS, six lanes, receive-handoff fix | All four workloads pass in three repetitions with zero refused starts and zero lost messages | Receive wait mean 5.5–11.1 ms; loaded-peak late_1ms 1.05–2.72%, other workloads 0.02–0.34%. Five-lane default remains placement-sensitive |
| RIO | Windows pair 2, m7i.16xlarge server / c7gn.8xlarge players, specific bind, default five lanes | Peak without game work: 900.3k packets/s and zero refused starts in three of three; one run loses two client-to-server packets. Half game work: zero refused in two of three; the other records 75,322 refused starts at 897.7k packets/s | Input delay mean 6.8–12.2 ms, p99 20.5–52.2 ms across these six rows. See the individual P13 results below; this is a different rig from the six-lane record |
| IOCP | Windows m7i.16xlarge, two completion workers and two send threads, receive-handoff fix | Peak is MARGINAL: 898.4–900.3k packets/s, with loaded-peak 18,663–56,479 and unloaded-peak 1,558–8,999 refused starts per run. Both average workloads pass | Receive wait mean 4.6–10.6 ms; late_1ms 0.16–0.48%. Peak refusal limit remains |
| msgx | Mac Studio M3 Ultra, Aquantia 10GBASE-T, three transmit workers | Average reaches about 480k packets/s without refused starts. No measured peak row reaches 900k | Address topology and interface contention affect throughput; the macOS timing column below is state-to-send, not receive delay |

**Explicitly selected RIO with a specific bind address is the Windows choice
for this peak workload**, with verified RSS and worker placement. Production
`auto` still tries `iocp,rio,portable`; IOCP is the default and fallback.
IOCP's remaining peak refusals make its near-target
packet rate insufficient for the same qualification. Configure and verify
placement using [Windows](WINDOWS.md); a default wildcard bind is not a
substitute for the measured setup. Windows measurements used Windows Server
2025 and do not expand the Windows 11 support row in
[Platforms](PLATFORMS.md).

The input-stamp harness measures receive wait at 60 Hz. In the native-row
record, `late_1ms` is the share of inputs already waiting more than 1 ms
before the preceding receive call began. This detects an extra update of
transport delay; it is not a 1 ms end-to-end latency target. The AF_XDP,
io_uring and RIO results support the peak choices above with the measured
receive fixes and placement, while retaining the displayed late-input tail.

The [AWS record](runs/performance-aws.md) and
[X710/Windows record](runs/performance-native.md) give the current rows,
measured builds, commands and settings. Their pass rules differ: AWS requires
zero loss and passing reconciliation; the native record permits up to 0.1%
player loss and separately records chain failures. Do not turn a row's rate
PASS into a clean chain verdict. The complete integrated build still needs
its final measurement on both throughput and receive delay.

These figures also do not establish the less-than-100-microsecond main-call
target at 1000 players. A mean of per-second p99 readings is not the whole
run's p99. Keep whole-run percentiles, per-second samples and maxima distinct.

## Windows bind, fallback and adapter measurements

The P13 rows use Windows pair 2: an AWS m7i.16xlarge server with 64 logical
processors and adapter `Ethernet 2`, and **c7gn.8xlarge Linux players** with
32 vCPUs and device `ens5`, in the same subnet and cluster placement group in
us-east-1c. These players differ from the c8gn.8xlarge players in the linked
Windows receive-handoff record. Do not combine the two rigs' repetitions.

The measurement owner reports MSVC Release on the server and GCC Release on
the players, using tree `cfd75ce15` with the input-stamp harness. The tool
links `rocketnet_dev`; these are not production-library measurements.
Each row explicitly selects its transport list, so it does not measure or
change the production `auto` preference. Runs were recorded on November 14,
2026, from 00:23 to 00:39 UTC; the raw receipts are not published here.

Each run has 1000 players, 32 client threads and a 60-second duration.
Throughput, frames, refusals, overruns and main work below use the server
ticks in the **25–55 second window**. Loss and reconciliation are separate
reported run outcomes. “Harness verdict” preserves the owner's result; it
does not mean zero refusals, zero loss, or qualification under another
record's pass rule.

| Row | Bind / granted transport | Game work | Packets/s | Frames/s | Refused starts | Overruns | Main p99 / max (ms) | Loss server-to-client / client-to-server | Harness verdict |
| --- | --- | --- | ---: | ---: | ---: | ---: | --- | --- | --- |
| P13-A1 | specific / RIO, five lanes | none | 900.3k | 60.67 | 0 | 0 | 0.566 / 0.680 | 0 / 0 | PASS |
| P13-A2 | specific / RIO, five lanes | none | 900.3k | 60.67 | 0 | 0 | 0.568 / 0.778 | 0 / 0 | PASS |
| P13-A3 | specific / RIO, five lanes | none | 900.3k | 60.67 | 0 | 0 | 0.585 / 1.023 | 0 / 2 | FAIL: unexplained client-to-server chain loss |
| P13-G1 | specific / RIO, five lanes | half | 900.1k | 60.57 | 0 | 1 | 0.938 / 1.126 | 0 / 0 | PASS |
| P13-G2 | specific / RIO, five lanes | half | 897.7k | 60.63 | 75,322 | 2 | 0.878 / 1.291 | 0 / 0 | PASS, with the displayed refusals |
| P13-G3 | specific / RIO, five lanes | half | 900.3k | 60.63 | 0 | 0 | 0.907 / 1.033 | 0 / 0 | PASS |
| P13-F1 | specific / IOCP after RIO budget refusal | none | 899.9k | 60.53 | 14,840 | 0 | 0.783 / 1.336 | 0 / 0 | PASS, with the displayed refusals |
| P13-N1 | wildcard / RIO, one lane | none | 241.4k | 57.60 | 18,614,564 | 10 | 1.815 / 2.197 | 0 / 123,093 | FAIL |
| P13-N2 | wildcard / IOCP, one socket | none | 899.5k | 60.53 | 25,097 | 0 | 0.828 / 1.645 | 0 / 0 | PASS, with the displayed refusals |

P13-G2's refused starts arrived in bursts. The sample-window count is
75,322; the final run counter is 83,508, including a burst after the sample
window. Neither value is a per-second rate. Five of the six specifically
bound RIO rows have zero refused starts, but only two of the three unloaded
rows have clean reconciliation. Wildcard RIO does not sustain peak here.
IOCP's wildcard and budget-fallback rows approach the offered rate with
nonzero refusals; they are not clean zero-refusal qualifications.

Input delay is measured from the player's input stamp to the server update
that reads it. The owner supplies these values in milliseconds at 60 Hz,
converted from update units; this is distinct from main-thread work and the
`late_1ms` metric in the other record.

| Row | Delay mean / p50 / p99 / max (ms at 60 Hz) |
| --- | --- |
| P13-A1 | 6.8 / 5.2 / 22.8 / 44.5 |
| P13-A2 | 7.8 / 5.8 / 20.5 / 47.4 |
| P13-A3 | 8.4 / 7.2 / 22.5 / 40.0 |
| P13-G1 | 12.0 / 13.7 / 33.3 / 62.2 |
| P13-G2 | 12.2 / 9.5 / 52.2 / 106.1 |
| P13-G3 | 9.6 / 6.8 / 28.3 / 46.5 |
| P13-F1 | 10.3 / 11.5 / 16.5 / 37.7 |
| P13-N1 | 44.3 / 43.3 / 83.3 / 112.4 |
| P13-N2 | 10.1 / 10.0 / 16.7 / 35.6 |

### P13 settings and selection checks

All rows read back this RSS setting on the server:

```powershell
Set-NetAdapterRss -Name 'Ethernet 2' -BaseProcessorNumber 0 -MaxProcessorNumber 8 -MaxProcessors 5 -NumberOfReceiveQueues 5
```

The `scripts/harden-pair` recipe uses `--row space --seconds 60 --clients
1000 --threads 32`, `--client-dev ens5`, and client arguments `--input-bytes
256 --input-stamp 40001`. Server arguments are `--frame-shape space
--space-send wide --space-update split --space-net task --space-split top
--load peak --input-stamp 40001`, plus the following settings:

| Granted transport | Game threads / first CPU | Game-work arguments |
| --- | --- | --- |
| RIO | `--game-threads 54 --pin-first-cpu 10` | none: `--game-load 0 --world-ms 0`; half: `--game-load 0.5 --world-ms 10` |
| IOCP | `--game-threads 56 --pin-first-cpu 8` | none: `--game-load 0 --world-ms 0` |

Set `ROCKETNET_SERVER_BIND_IPV4_ADDRESS` to the server's private address for
the bound rows; omit it for the wildcard rows. RIO rows and the fallback row
use `ROCKETNET_SERVER_TRANSPORT=rio,iocp,portable`; the wildcard IOCP row uses
`iocp`. The harness's `--transport` names the transport required to be
granted, including `iocp` for the fallback row. Read the startup grant.

In P13-F1, `ROCKETNET_SERVER_MEMORY_BUDGET_BYTES=700000000` rejects RIO's
904,379,943-byte allocator plan; IOCP's 534,051,687-byte plan fits. The
resulting IOCP layout has two completion workers, two send threads and eight
send lanes of 4096 slots. This demonstrates fallback on a valid budget
refusal, not recovery from an invalid configuration. Wildcard RIO logs one
lane, one worker and one socket, with a 491,349,431-byte plan.

### Adapter observations

These supplied adapter observations describe measured configurations; they
are not additional repetitions of P13 and do not establish causation between
interrupt placement and a throughput shortfall. Read back both RSS settings
and actual per-processor activity, as described in [Windows](WINDOWS.md).

| Configuration or operation | Reported observation |
| --- | --- |
| Five-lane RIO at about 900k packets/s, RSS on processors 0, 2, 4, 6, 8 | Socket processors spend 38–47% in DPCs, about 40k–47k DPCs/s each. Worker processors 1, 3, 5, 7, 9 spend 0.0% in DPCs and at most 0.7% in interrupts |
| Six-lane RIO, RSS range 0–10, six processors and receive queues, measured interrupt overlap | Worker processors 7 and 9 spend 21.6–23.1% in interrupts, about 48k–50k interrupts/s each; processor 11 spends about 15%. Three runs record about 685k–740k packets/s and 165k–225k refused starts/s from ramp to finish. Per-processor counters were captured in two of the three |
| Same six-lane shortfall configuration | About 9,900 `RIO_SEND_WAITS` by 80 seconds and 59–79% lane-worker thread time. The interrupt and throughput observations occur together; this does not identify the cause |
| Normal-priority busy loops on all RIO worker processors for three seconds | No measured change in the supplied rows. The implementation requests `THREAD_PRIORITY_TIME_CRITICAL`; the requested priority alone does not establish what the OS granted |
| Forced three-second stall using realtime-class busy loops on every RIO worker processor | The stall costs those three seconds and about one further second; reported recovery reaches 900k packets/s with zero refused starts. This is an observation, not a recovery-time guarantee |
| IOCP development override `ROCKETNET_TEST_IOCP_LAYOUT=2,4/3,5`, RSS on processors 2 and 4 | 54–57% DPC time on those processors. Two supplied runs with this layout report 738 and 7,405 refused starts; the layout is not a zero-refusal result or a production override |
| Applying `Set-NetAdapterRss` | The adapter reset interrupts its SSH connection for about 15 seconds in the supplied observations; do not treat that duration as a bound |

The six-lane shortfall rows and the six-lane passing receive-handoff rows in
the qualification table have different measurement conditions. Lane count
alone does not determine capacity. These observations support verifying the
actual worker/interrupt placement rather than assuming an RSS request proves
it. They do not change the production transport order.

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
not divide a whole-run counter by only the sample duration. The AWS rows require at least 99.9% of the offered rate, at least 60 frames/s,
zero refused starts, zero lost messages and passing reconciliation chains.
Other records state their own acceptance criteria. “1000 connected” does not
mean zero packet loss.

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

The current summaries were supplied on November 13–14, 2026. The AWS
record identifies main `e0f1a0fae` with the measurement harness merged as
`74e7be243`, identical on server and clients;
the Windows receive-fix series identifies `8c58f1946` plus its harness. The
msgx record identifies server `25fca1774` and address-comparison clients
`aa9d85c50`; the other msgx client rows use `25fca1774`. These identifiers bound
the reported results to the measured builds. The linked row records identify the measurement recipes. Capture full
server/client/harness commit identities and raw receipts when qualifying the
final integrated build.
