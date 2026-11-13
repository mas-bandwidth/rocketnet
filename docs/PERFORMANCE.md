# Performance

These measurements use work-in-progress builds, not a released version of RocketNet.

## Workload

Each run uses separate server and client machines, 1000 players, a 60 Hz tick,
and a nominal 60-second load. The measured windows are listed below; startup,
joining, and draining are outside the steady-window timing figures.

| Traffic | Peak | Average |
| --- | --- | --- |
| Server to each client, every tick | 15 unreliable messages of 1200 bytes | 8 unreliable messages of 1200 bytes |
| Target server packet rate | About 900,000 packets/s | About 480,000 packets/s |
| Server to each client, every 3 seconds | One reliable message of 100 bytes | One reliable message of 100 bytes |
| Each client to server, every tick | One input message of 256 bytes | One input message of 256 bytes |

Each unreliable server message occupies one packet. There are no large messages.
Peak server traffic is about 10 Gb/s; client input is about 140 kbps per player
including packet overhead. Packet rates include protocol traffic, so they need
not equal the target message rate exactly.

## Machines and threads

CPU 0 is reserved for the OS, CPUs 1–2 for the two network threads, and CPU 3
for the main thread. The game threads follow on separate CPUs. They write each
client's packets as jobs over contiguous ranges of clients. The test uses
enkiTS for these jobs; RocketNet does not create the game threads.

| Server transport | Server | Client machine and network | Thread layout |
| --- | --- | --- | --- |
| XDP | Latitude m4.metal.large, AMD EPYC 9254, 24 cores | Second m4.metal.large; Intel X710 10 GbE | Two XDP threads on CPUs 1–2; main thread on CPU 3; 20 game threads on CPUs 4–23 |
| io_uring | AWS c6in.8xlarge, 32 vCPUs, x86, Linux 7.0 | AWS c7gn.8xlarge, 32 vCPUs, arm64; same placement, private network | Two network threads on CPUs 1–2; main thread on CPU 3; 28 game threads on CPUs 4–31, real-time priority |
| GSO | AWS c8gn.8xlarge, Graviton4 | Pending | Pending |
| RIO / IOCP | AWS m7i.16xlarge, Windows Server 2025 | Pending | Pending |
| macOS | Owned Macs; exact models pending | Pending | Pending |

For io_uring, CPU 0 also handles NIC interrupts. The library pins each network
thread to the number of OS CPUs plus its queue index: CPUs 1–2 in these runs.
The client machine runs 32 unpinned client threads.

## Results

Delivery is the percentage of unreliable messages delivered out of delivered
plus lost, reported separately in each direction by `harden_load reconcile`.
It is not the percentage of players that connected. The rows below report
1000 players.

| Transport | Load | Server packets/s | Frames/s | Server → client delivery | Client → server delivery |
| --- | --- | ---: | ---: | ---: | ---: |
| XDP | Peak | 895,000 | 60.5 | 100% | 100% |
| XDP | Average | 480,000 | 60.5 | 100% | 100% |
| io_uring | Peak | 900,316 | 60.60 | 100% | 100% |
| io_uring | Average | 480,317 | 60.56 | 100% | 100% |

Network work is reported in milliseconds per frame. The main-thread average
is elapsed time in its network calls, including waits inside those calls.
Game-thread min/mean/max compares the per-frame network work across game
threads; these are not frame-latency percentiles. The ratio is main-thread
average divided by mean game-thread network work. These times exclude the
dedicated network threads and do not describe the whole frame's duration.

| Transport | Load | Main average | Main p99 | Main max | Game threads min / mean / max | Main / mean game thread |
| --- | --- | ---: | ---: | ---: | --- | ---: |
| XDP | Peak | 0.42 | 0.48 | 1.06 | 3.14 / 3.37 / 4.24 | 0.12 |
| XDP | Average | 0.40 | 0.47 | 0.61 | 1.72 / 1.94 / 2.81 | 0.21 |
| io_uring | Peak | 0.157 | 0.176 | 0.210 | 3.04 / 3.20 / 4.80 | 0.05 |
| io_uring | Average | 0.147 | 0.164 | 0.237 | 1.65 / 1.81 / 3.35 | ≈0.08 (derived) |

The io_uring average-load ratio is calculated from the reported rounded means:
0.147 / 1.81. The other ratios were supplied with their measurement rows.

| Rows | Build | Steady window | Simulated game work |
| --- | --- | --- | --- |
| XDP peak / average | Exact measurement commit pending | t=25–55 s | Exact command settings pending |
| io_uring peak / average | [6db162421](https://github.com/mas-bandwidth/rocketnet/commit/6db1624212079133de226fe08cb8111bd70e352d), `tests/v1.2/goal-uring`, both machines | t=25–75 s, after joining and warm-up | `--world-ms 0 --game-load 0` |

The io_uring rows use the earlier frame order with no simulated game work.
Game-load rows for every transport are pending a rerun: the test's simulated
world update was serial, corrected in `4fc1ae05b`. Raw result logs are pending;
the timing percentiles
are reported values, not recomputed here. Different machines, builds, and
sampling windows make these workload results unsuitable for ranking transports
against one another.

| Measurement still pending | Status |
| --- | --- |
| XDP with game threads half busy with game work | Pending |
| io_uring with game threads half busy with game work | Pending |
| GSO peak and average | Pending |
| RIO peak and average | Pending |
| IOCP peak and average | Pending |
| macOS peak and average | Pending |

## Repeating a run

The tool is `tests/load/harden_load.cpp`. Build `harden_load` on both machines
using the [build instructions](BUILDING.md), at the commit of the row being
repeated. Use a server address reachable from the client machine.

The current `--game-threads N` flag counts the main thread plus N−1 game
threads. XDP uses 21 and io_uring uses 29. `--world-ms` sets simulated work
on the main thread; `--game-load 0.5` requests game work equal to half a frame
per game thread. Neither is a network timing measurement.

The following direct invocations preserve the io_uring peak row's recorded
workload arguments at [6db162421](https://github.com/mas-bandwidth/rocketnet/blob/6db1624212079133de226fe08cb8111bd70e352d/tests/load/harden_load.cpp).
For the average row, change `--load peak` to `--load average`.
Substitute addresses and interface names. Supply the same Unix timestamp
`END_AT` to both roles, allowing about 25 seconds for joining and warm-up
before the 60-second load. Start the server first:

```sh
./bin/harden_load server --address "$SERVER_IP:40000" \
  --transport io_uring --max-clients 1000 --hz 60 \
  --seconds 60 --end-at "$END_AT" --dev "$SERVER_DEV" \
  --warm-seconds 20 --sample-seconds 60 --payload copy --num-xdp-queues 2 \
  --frame-shape space --load peak --game-threads 29 \
  --pin-first-cpu 3 --world-ms 0 --game-load 0 \
  --space-send wide --space-update split --space-net task > server.log 2>&1
```

Copy the public key from the server's `harden_ready` line into `SERVER_KEY`.
On the client machine, using the same `END_AT`:

```sh
./bin/harden_load clients --address "$SERVER_IP:40000" --key "$SERVER_KEY" \
  --clients 1000 --threads 32 --hz 60 --messages 1 \
  --reliable-every 1 --join-rate 100 --payload copy \
  --input-bytes 256 --seconds 60 --end-at "$END_AT" \
  --dev "$CLIENT_DEV" > clients.log 2>&1
```

After both roles finish, collect the logs on one machine:

```sh
./bin/harden_load reconcile --logs server.log,clients.log
```

Record the granted transport and thread affinity, not just requested settings.
Use `harden_tick` for packet rate and frame count per reporting interval, `harden_threads` for network
work and the ratio, and `harden_frame` for main-thread p99/max. Retain the
steady-window samples and the final delivery report. These runs used
`scripts/harden-pair` with a bench-local wrapper that appended `SF_SERVER_ARGS`;
the checked-in script alone does not accept the frame and game-thread flags.
The commands above invoke the binary directly. Exact XDP commands and the
archived launcher are pending. Corrected-order runs select `--space-split top`;
that option is available in this build but is absent from the recorded
arguments above. Do not substitute a different frame order when repeating a row.

## Profiles

Per-thread profiles are pending. Each profile will list functions in descending
order of measured milliseconds per frame, together with its build and sampling
window. No function costs are estimated here.
