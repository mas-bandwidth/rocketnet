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

For the baseline Linux rows, CPU 0 is reserved for the OS, CPUs 1–2 for the two
network threads, and CPU 3 for the main thread. The game threads follow on
separate CPUs. They write each client's packets as jobs over contiguous
ranges of clients. The test uses
enkiTS for these jobs; RocketNet does not create the game threads.

| Server transport | Server | Client machine and network | Thread layout |
| --- | --- | --- | --- |
| XDP | Latitude m4.metal.large, AMD EPYC 9254, 24 cores | Second m4.metal.large, 48 logical CPUs; Intel X710 10 GbE | Two XDP threads on CPUs 1–2; main thread on CPU 3; 20 game threads on CPUs 4–23; all 21 pool threads at real-time priority |
| io_uring | AWS c6in.8xlarge, 32 vCPUs, x86, Linux 7.0 | AWS c7gn.8xlarge, 32 vCPUs, arm64; same placement, private network | Two network threads on CPUs 1–2; main thread on CPU 3; 28 game threads on CPUs 4–31, real-time priority |
| GSO | AWS c8gn.8xlarge, Graviton4, arm64, 32 vCPUs; Ubuntu 26.04, GCC 15.2 | AWS c6gn.16xlarge, 64 vCPUs; same placement, private network | Two network threads on CPUs 1–2, or one on CPU 1 in the scaling run; main thread on CPU 3; 28 game threads on CPUs 4–31; no real-time priority |
| IOCP | AWS m7i.16xlarge, 64 vCPUs, Windows Server 2025; MSVC Release | AWS c8gn.8xlarge, 32 vCPUs, Linux; private network | OS on CPUs 0–1; NIC receive steering on CPUs 2 and 4; two receive threads on CPUs 3 and 5; main thread on CPU 6; 57 game threads on CPUs 7–63; eight unpinned send threads |
| RIO | AWS m7i.16xlarge, Windows Server 2025 | Pending | Eight network lanes in the passing peak row; CPU placement and game pool pending |
| macOS | Owned Macs; exact models pending | Pending | Pending |

For io_uring, CPU 0 also handles NIC interrupts. The library pins each network
thread to the number of OS CPUs plus its queue index: CPUs 1–2 in these runs.
XDP also pins its own network threads. GSO does the same in `ebdce1ca4`:
queue q pins itself to CPU `num_os_cpus + q`. The new GSO loaded-peak
measurements use library-owned pinning; earlier GSO rows retain their
original build provenance below. XDP and io_uring clients run 32 unpinned threads;
GSO uses 32 client threads, with affinity confirmation pending.

The profiled XDP server ran kernel `7.0.0-43`, booted with `nosmt irqaffinity=0
isolcpus=1-2 nohz_full=1-2`. These settings are part of that measured
environment; retain them when comparing its profile with another run.

IOCP uses ten network threads: two receive/completion threads, each with its
own socket, and eight send threads. The baseline Linux rows use two network
threads each; GSO also holds peak with one, as detailed below.
IOCP has no send-thread affinity setting in this build; the run
owner reports Windows scheduling them among CPUs 6–63, shared with the main
and game threads. IOCP clients use 16 threads. Measurements with fewer IOCP
network threads, client affinity, and server scheduling priority are pending.

## Results

### No simulated game-thread work

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
| GSO | Peak | 900,000 | 60.7 | 100% | 100% |
| GSO | Average | 480,000 | 60.5 | 100% | 100% |
| IOCP | Peak | 900,300 | 60.63 | 100% | 99.999% |
| IOCP | Average | 480,300 | 60.80 | 100% | 99.968% |
| RIO, eight lanes | Peak | 900,000 | 60.6 | 100% | 100% |

IOCP's average row lost 1,487 of approximately 4.58 million client inputs;
server-to-client delivery had no reported loss. Its peak row also had a small
input loss, reflected in the 99.999% delivery figure above.

Network work is reported in milliseconds per frame. The main-thread average
is elapsed time in its network calls, including waits inside those calls.
Game-thread min/mean/max compares the per-frame network work across game
threads; these are not frame-latency percentiles. The ratio is main-thread
average divided by mean game-thread network work. These times exclude the
dedicated network threads and do not describe the whole frame's duration.

| Transport | Load | Main average | Main p99 | Main max | Game threads min / mean / max | Main / mean game thread |
| --- | --- | ---: | ---: | ---: | --- | ---: |
| XDP | Peak | 0.417 | 0.484 | 1.062 | 3.14 / 3.37 / 4.24 | 0.12 |
| XDP | Average | 0.398 | 0.467 | 0.606 | 1.72 / 1.94 / 2.81 | 0.21 |
| io_uring | Peak | 0.157 | 0.176 | 0.210 | 3.04 / 3.20 / 4.80 | 0.05 |
| io_uring | Average | 0.147 | 0.164 | 0.237 | 1.65 / 1.81 / 3.35 | ≈0.08 (derived) |
| GSO | Peak | 0.26 | 0.27 | 0.29 | 2.52 / 2.72 / 3.16 | 0.095 |
| GSO | Average | 0.24 | 0.25 | 0.27 | 1.29 / 1.48 / 1.89 | 0.16 |
| IOCP | Peak | Pending | 0.178 | 0.274 | 2.97 / 3.11 / 6.35 | 0.05 |
| IOCP | Average | 0.148 | 0.175 | 0.272 | 1.58 / 1.71 / 5.12 | 0.087 |
| RIO, eight lanes | Peak | 0.46 | 0.57 | 0.79 | 3.1 / 3.3 / 5.4 | 0.14 |

The io_uring average-load ratio is calculated from the reported rounded means:
0.147 / 1.81. The other ratios were supplied with their measurement rows.
For the XDP rows above, the p99 column is the mean of the per-second p99s
in the steady window, not a p99 computed over all frames in that window.

| Rows | Build | Steady window | Simulated game work |
| --- | --- | --- | --- |
| XDP peak / average | [79888a398](https://github.com/mas-bandwidth/rocketnet/commit/79888a3980a42db26fa0d84c22b1df6f4e1ee3f9), measurement branch, server binary | t=25–55 s | `--world-ms 0 --game-load 0` |
| io_uring peak / average | [6db162421](https://github.com/mas-bandwidth/rocketnet/commit/6db1624212079133de226fe08cb8111bd70e352d), `tests/v1.2/goal-uring`, both machines | t=25–75 s, after joining and warm-up | `--world-ms 0 --game-load 0` |
| GSO peak / average | [d9110a19f](https://github.com/mas-bandwidth/rocketnet/commit/d9110a19fedf098a15068ce9a4cb9106f4ee603d), `tests/v1.2/goal-gso` | t=30–60 s | `--world-ms 10 --game-load 0` |
| IOCP peak / average | [110416b4c](https://github.com/mas-bandwidth/rocketnet/commit/110416b4cec7670c97d43c752b25d454b3b90725), `tests/v1.2/goal-iocp-nofold` | Pending | `--world-ms 0 --game-load 0` |
| RIO peak, eight lanes | Pending; earlier frame order | Pending | No game work reported; exact world-work setting pending |

The io_uring rows use the earlier frame order with no simulated game work.
GSO starts its network task before the world update with `--space-split top`.
Its main thread performs 10 ms of simulated world work per frame while its
game threads carry only network work. XDP and io_uring have no simulated
world work in the rows above.
The GSO rates are means over its steady window. Its server requested a 25 MB
socket receive buffer and the OS granted 4 MB, limited by `net.core.rmem_max`.
There was no reported loss at this load; check the requested and granted
values in the transport log when configuring another machine.

Raw result logs are pending;
the timing percentiles are reported values, not recomputed here. Confirmation
of the io_uring, GSO, IOCP, and RIO percentile aggregation is pending. Different machines, builds, and
sampling windows make these workload results unsuitable for ranking transports
against one another.

### Game threads half busy with game work

These rows use `--game-load 0.5 --world-ms 10 --space-split top`. The game
jobs request work equal to half a 60 Hz frame per game thread, while the main
thread performs 10 ms of simulated world work. The job scheduler distributes
that work across the pool; these settings do not prove identical utilization
on every thread. The builds below correct the earlier serial world-work
stand-in: game jobs run alongside the main thread's world work.

The network task operates on the preceding world state while the next world
update runs. This adds state-to-send delay: approximately one frame in the
ordering, measured around 10 ms here. The latency figures below measure the
time from the world state to the test's send call, not network transit time
or a separately measured increase over a baseline. Input age is measured
from the server's network update completing until the main thread takes its
inputs for the world update.

Hardware, pool size, and thread placement match each transport's rows above.
All six rows report 1000 players and 100% delivery in both directions.

| Transport | Load | Server packets/s | Frames/s | Main average | Main p99 | Main max | Game threads min / mean / max | Main / mean game thread |
| --- | --- | ---: | ---: | ---: | ---: | ---: | --- | ---: |
| XDP | Peak | 900,000 | 60.4 | 0.424 | 0.620 | 0.696 | 2.68 / 3.12 / 4.05 | ≈0.14 (derived) |
| XDP | Average | 480,000 | 60.6 | 0.384 | 0.490 | 0.626 | 0.97 / 1.70 / 4.63 | ≈0.23 (derived) |
| io_uring | Peak | 900,318 | 60.40 | 0.158 | 0.205 | 0.286 | 2.37 / 2.76 / 3.36 | 0.057 |
| io_uring | Average | 480,316 | 60.64 | 0.154 | 0.168 | 0.234 | 1.35 / 1.64 / 2.03 | 0.094 |
| GSO | Peak | 900,000 | 60.6 | 0.26 | 0.32 | 0.35 | 2.30 / 2.68 / 3.12 | 0.098 |
| GSO | Average | 480,000 | 60.5 | 0.24 | 0.30 | 0.32 | 1.14 / 1.47 / 2.0 | 0.165 |

Network times remain milliseconds per frame. The two XDP ratios are derived
from the reported rounded means: 0.424 / 3.12 and 0.384 / 1.70. The other
ratios were supplied with the rows. Percentile aggregation for these loaded
rows is awaiting confirmation.

The earlier builds' `leftover_core_ms` field is excluded because it omitted
game-thread network work and overstated the time available. Corrected XDP
reruns are recorded separately below.

| Rows | Build | Steady window |
| --- | --- | --- |
| XDP peak / average | [186a4f364](https://github.com/mas-bandwidth/rocketnet/commit/186a4f3646632119d64633e9e9404cd93e61c4ee), measurement branch | t=25–55 s |
| io_uring peak / average | [c154d7971](https://github.com/mas-bandwidth/rocketnet/commit/c154d7971ab3e6e57c12c4e2012fe9442d7534bb), `tests/v1.2/goal-uring` | t=25–75 s |
| GSO peak, two network threads | [ebdce1ca4](https://github.com/mas-bandwidth/rocketnet/commit/ebdce1ca4ab6c083f7b4887b3ea8cc0ee16d2e4a), `tests/v1.2/goal-gso`, library-owned pinning | Pending confirmation |
| GSO average | [aad649f62](https://github.com/mas-bandwidth/rocketnet/commit/aad649f624690c21ec34a8eba07da6903f3b4ef3), `tests/v1.2/goal-gso` | Pending confirmation |

| Transport | Load | State-to-send latency mean / max (ms) | Input age mean / max (ms) | Main join wait mean (ms) | World update mean (ms) |
| --- | --- | --- | --- | ---: | ---: |
| XDP | Peak | 10.1 / 13.5 | 4.7 / 8.0 | 0.003 | 11.84 |
| XDP | Average | 10.5 / 15.5 | 6.2 / 8.1 | 0.003 | 10.44 |
| io_uring | Peak | 9.9 / Pending | 5.2 / Pending | Pending | Pending |
| io_uring | Average | 9.1 / Pending | 6.8 / Pending | Pending | Pending |

The XDP loaded rows reported no frame overruns. In GSO's earlier loaded peak
run at `aad649f62`, the world update took 11.4 ms and the network task finished in
11.2 ms; these operations overlap. Its reported state-to-send delay was
about 10 ms; exact per-row latency and input-age summaries are pending.

#### XDP rerun with corrected remaining-time accounting

The calculation was fixed in [03a29f651](https://github.com/mas-bandwidth/rocketnet/commit/03a29f651a83c47aac92134c0bc321c9ddfbb352)
and in measurement-branch [f46d820ba](https://github.com/mas-bandwidth/rocketnet/commit/f46d820ba851bb09db3a488c7bcd9ff36b8ad2c0).
The run owner supplied these reruns with game threads half busy and the
corrected frame order. Both report 100% delivery in each direction and no
frame overruns. Packet rates, steady windows, and confirmation of which
fixed binary produced the rows are pending.

| XDP load | Frames/s | Main mean / p99 / max (ms) | Game threads min / mean / max (ms) | Pool network work (core-ms/frame) | Mean game-thread time left (ms/frame) |
| --- | ---: | --- | --- | ---: | ---: |
| Peak | 60.6 | 0.422 / 0.481 / 0.659 | 2.64 / 3.09 / 4.06 | 62.5 | 5.23 |
| Average | 60.5 | 0.403 / 0.526 / 0.651 | 0.92 / 1.68 / 4.43 | 33.9 | 6.66 |

The remaining-time column is calculated as `16.67 ms − measured game work
per game thread − mean network work per game thread`, using the unrounded
measurements. It is an average frame-budget estimate, not a guaranteed
contiguous idle interval on every thread. The pool network total sums
instrumented main/game-thread work; it excludes the two dedicated XDP
threads and is not the wall duration of a frame.

For the reported old-order comparison, starting network work after the world
update reached 797,000 packets/s at 53.7 frames/s with 1,611 late frames at
peak, and 467,000 packets/s at 58.9 frames/s at average load.

IOCP's results with game work remain separate because its peak run does not
yet hold the target. The average row reaches the target with the input loss
shown below. Exact loaded build, world-work setting, frame order, and sampling
window are pending.

| IOCP load | Server packets/s | Frames/s | Server → client delivery | Client → server delivery | Peak/average rate and frame target |
| --- | ---: | ---: | ---: | ---: | --- |
| Peak | 854,600 | 57.5 | Pending | Pending | Not yet passing; 372 of 4,759 frames late |
| Average | 480,300 | 60.60 | 100% | 99.946% | Reached; input loss remains |

The loaded IOCP average main-thread network times are 0.145 / 0.191 / 0.255 ms
(mean / p99 / max); game-thread min / mean / max is 0.91 / 1.25 / 3.93 ms.
Loaded peak timings and a corrected rerun are pending.

### Network threads needed at peak

These are the smallest passing counts measured so far on each stated machine
and workload. Counts still being tested are not established minima. Reaching
the rate and frame target does not imply zero loss; delivery is reported
separately.

| Transport | Smallest passing count measured | Workload and remaining checks |
| --- | ---: | --- |
| GSO | 1 | Peak with game threads half busy; 100% delivery both ways |
| XDP | 2 | Peak with and without game work; one-thread run pending |
| io_uring | 2 | Peak with and without game work; one-thread run pending |
| RIO | 8 | Peak without game work; four lanes miss target; five through seven pending |
| IOCP | 10 | Peak without game work, with small input loss; fewer-thread runs pending; loaded peak not yet passing |

The GSO loaded-peak comparison uses library-owned network-thread pinning:

| GSO network threads | Server packets/s | Frames/s | Delivery, both directions | Main mean / p99 / max (ms) | Game threads min / mean / max (ms) |
| --- | ---: | ---: | --- | --- | --- |
| 1, CPU 1 | 900,000 | 60.6 | 100% | 0.25 / 0.32 / 0.32 | 2.30 / 2.63 / 3.08 |
| 2, CPUs 1–2 | 900,000 | 60.6 | 100% | 0.26 / 0.32 / 0.35 | 2.30 / 2.68 / 3.12 |

Both rows use `ebdce1ca4` on the same machines. The one-thread row retains
the main thread on CPU 3 and game threads on CPUs 4–31; its single network
thread runs on CPU 1. Sampling-window confirmation remains pending.

RIO's lane sweep used the earlier frame order without game work:

| RIO lanes | Server packets/s | Frames/s | Server → client delivery | Client → server delivery | Peak target |
| --- | ---: | ---: | ---: | ---: | --- |
| 2 | 222,000 | 16.4 | Pending | 86.5% | Missed |
| 4 | 629,000 | 44.7 | 100% | 100% | Missed |
| 8 | 900,000 | 60.6 | 100% | 100% | Reached |

The observed aggregate rates improve with more lanes; a fixed per-lane
capacity is not established by these rows. Exact RIO commands, build, client
machine, and loaded rows remain pending.

| Measurement still pending | Status |
| --- | --- |
| RIO exact peak reproduction details, average and loaded rows, and five-to-seven-lane measurements | Pending |
| IOCP peak main-thread mean, sampling window, and fewer-network-thread measurements | Pending |
| IOCP loaded peak | Not yet passing; corrected rerun pending |
| IOCP loaded reproduction details | Pending |
| macOS peak and average | Pending |

## Run it yourself

The tool is `tests/load/harden_load.cpp`. Build `harden_load` on both machines
using the [build instructions](BUILDING.md), at the commit of the row being
repeated. Use a server address reachable from the client machine.

The target requires `ROCKETNET_BUILD_TOOLS=ON` and
`ROCKETNET_BUILD_DEV_LIBRARIES=ON`; it links the development library. XDP
requires `ROCKETNET_XDP=ON`, a clang with BPF support, and the AF_XDP
dependencies in [Building](BUILDING.md). io_uring requires
`ROCKETNET_IO_URING=ON` and liburing. Check the CMake output: a requested
transport can be compiled out if its dependencies are absent.

The supplied Linux build recipe is below. Run it from a checkout at the
row's measured commit, setting `OUTPUT_DIR` to an absolute directory and
`BUILD_JOBS` to the desired build parallelism. Development-library, XDP,
and io_uring options above default to on in these builds.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DROCKETNET_DEV=ON -DROCKETNET_BUILD_TOOLS=ON \
  -DCMAKE_RUNTIME_OUTPUT_DIRECTORY="$OUTPUT_DIR/bin"
cmake --build build --target harden_load -j "$BUILD_JOBS"
```

`-S` selects the source directory and `-B` the build directory. `Release`
selects optimization; `ROCKETNET_DEV` enables development helpers and
`ROCKETNET_BUILD_TOOLS` includes the load tool. The runtime output option
chooses the binary directory; `--target` builds `harden_load`, and `-j`
sets compiler parallelism. The run commands below assume that binary is
available at `./bin/harden_load`. The XDP profiling build's flags are recorded
in the profiling section below.

The current `--game-threads N` flag counts the main thread plus N−1 game
threads. XDP uses 21; io_uring and GSO use 29; IOCP uses 58.
`--world-ms` sets simulated work
on the main thread; `--game-load 0.5` requests game work equal to half a frame
per game thread. Neither is a network timing measurement.

Choose the matching XDP or io_uring row settings for the direct commands below:

| Row | `TRANSPORT` | `POOL_THREADS` | `PAYLOAD` | `LOAD` |
| --- | --- | ---: | --- | --- |
| XDP peak | `xdp` | 21 | `pattern` | `peak` |
| XDP average | `xdp` | 21 | `pattern` | `average` |
| io_uring peak | `io_uring` | 29 | `copy` | `peak` |
| io_uring average | `io_uring` | 29 | `copy` | `average` |

The commands preserve the workload arguments copied from the measurement
cells. Set the variables above and substitute addresses and interface names.
Supply the same Unix timestamp
`END_AT` to both roles, allowing about 25 seconds for joining and warm-up
before the 60-second load. Start the server first:

```sh
./bin/harden_load server --address "$SERVER_IP:40000" \
  --transport "$TRANSPORT" --max-clients 1000 --hz 60 \
  --seconds 60 --end-at "$END_AT" --dev "$SERVER_DEV" \
  --warm-seconds 20 --sample-seconds 60 --payload "$PAYLOAD" --num-xdp-queues 2 \
  --frame-shape space --load "$LOAD" --game-threads "$POOL_THREADS" \
  --pin-first-cpu 3 --world-ms 0 --game-load 0 \
  --space-send wide --space-update split --space-net task > server.log 2>&1
```

Copy the public key from the server's `harden_ready` line into `SERVER_KEY`.
On the client machine, using the same `END_AT`:

```sh
./bin/harden_load clients --address "$SERVER_IP:40000" --key "$SERVER_KEY" \
  --clients 1000 --threads 32 --hz 60 --messages 1 \
  --reliable-every 1 --join-rate 100 --payload "$PAYLOAD" \
  --input-bytes 256 --seconds 60 --end-at "$END_AT" \
  --dev "$CLIENT_DEV" > clients.log 2>&1
```

After both roles finish, collect the logs on one machine:

```sh
./bin/harden_load reconcile --logs server.log,clients.log
```

The io_uring run used `scripts/harden-pair` with a bench-local wrapper that
appended `SF_SERVER_ARGS`. XDP used a bench-local launcher. Their archived
launcher and documented row script are pending; the checked-in script alone
does not accept the frame and game-thread flags. The commands above invoke
the binary directly. The recorded arguments above omit `--space-split top`.
Do not substitute a different frame order when repeating a row.

For GSO's new loaded peak rows, use `ebdce1ca4`, set `NETWORK_THREADS` to
1 or 2, and preserve the 10 ms world-work setting. The library pins its network threads;
the command does not need a load-test network-pin flag.
The archived launcher and its stop-time
arguments are pending; these are the binary workload arguments supplied
with the row:

```sh
./bin/harden_load server --address "$SERVER_IP:40000" \
  --transport gso --max-clients 1000 --dev "$SERVER_DEV" \
  --sample-seconds 60 --num-xdp-queues "$NETWORK_THREADS" \
  --frame-shape space --game-threads 29 --pin-first-cpu 3 \
  --space-send wide --space-update split --space-net task \
  --world-ms 10 --space-split top --game-load 0.5 --load peak > server.log 2>&1
```

After copying the server public key, run on the client machine:

```sh
./bin/harden_load clients --address "$SERVER_IP:40000" --key "$SERVER_KEY" \
  --clients 1000 --threads 32 --messages 1 --reliable-every 1 \
  --join-rate 100 --input-bytes 256 > clients.log 2>&1
```

Reconcile the logs as above. GSO uses the tool's default 60 Hz and 60-second
duration; the supplied command does not override its default pattern payload.

The historical GSO rows use their original commits: `d9110a19f` without
game work and `aad649f62` for loaded average. To repeat those rows, retain
their recorded `--net-first-cpu 1`, set `--game-load` to 0 or 0.5 respectively,
and select the recorded peak/average load. They have not been relabelled as
measurements of the newer build. Set `NETWORK_THREADS=2` for those historical
rows.

For IOCP, use the Windows server build at `110416b4c` and a Linux client.
The exact MSVC build invocation, NIC receive-steering setup, receive-thread
affinity setup, and archived launcher are pending. The supplied binary
workload commands are below. Set `LOAD` to `peak` or `average` and bind the
server to its specific IPv4 address. In PowerShell on the server:

```powershell
.\bin\harden_load.exe server --address "${SERVER_IP}:40000" --transport iocp --max-clients 1000 --dev "$SERVER_DEV" --warm-seconds 20 --sample-seconds 60 --payload copy --frame-shape space --load "$LOAD" --game-threads 58 --pin-first-cpu 6 --game-load 0 --world-ms 0 --space-send wide --space-update split --space-net task --num-xdp-queues 2 > server.log 2>&1
```

Copy the server public key, then run on the Linux client:

```sh
./bin/harden_load clients --address "$SERVER_IP:40000" --key "$SERVER_KEY" \
  --clients 1000 --threads 16 --messages 1 --reliable-every 1 \
  --join-rate 100 --payload copy --input-bytes 256 > clients.log 2>&1
```

Reconcile both logs on the Linux machine as above. These arguments retain
the default 60 Hz and 60-second duration; the shared stop-time settings used
by the original launcher are pending.

To repeat an XDP or io_uring row with game threads half busy, build its loaded-row commit
from the table above. Retain that transport's machine layout, payload,
client arguments, and peak/average load selection. In the server command,
replace `--game-load 0` with `--game-load 0.5`, set `--world-ms 10`, and
add `--space-split top` if absent. Use these settings only with the listed
loaded-row builds; the earlier builds have a different world-work stand-in.

### Command flags

| Flag | Meaning |
| --- | --- |
| `--address` | Server bind address for `server`; destination address for `clients`. |
| `--transport` | Requested server transport; confirm the granted transport in `harden_ready`. |
| `--max-clients` / `--clients` | Server capacity / number of client players. |
| `--hz 60` | Tick rate on both ends. |
| `--seconds 60` | Nominal duration; the shared `--end-at` controls the actual stop time when supplied. |
| `--end-at` | Common Unix timestamp at which the roles stop generating traffic, then drain. |
| `--dev` | Network interface used for host counters. |
| `--warm-seconds 20` | Delay before the server's first long-run sample; separate from the reported steady window. |
| `--sample-seconds 60` | Interval between long-run server samples. |
| `--payload` | `pattern` generates a verifiable body; `copy` copies a prebuilt body. |
| `--num-xdp-queues` | Requested network queue count for XDP, io_uring, and GSO: 2 for baseline rows, 1 for GSO's one-thread run; also retained in the supplied IOCP command, whose actual thread counts are listed above. |
| `--frame-shape` | Select the test's job-based frame loop. |
| `--load` | Set the peak or average server traffic from the workload table. |
| `--game-threads` | Pool size including the main thread. |
| `--pin-first-cpu` | Pin the main thread to this CPU and successive game threads to successive CPUs: 3 for Linux rows, 6 for IOCP. |
| `--net-first-cpu 1` | Historical GSO load-test pinning for the older measured builds; omitted for library-owned pinning in `ebdce1ca4`. |
| `--world-ms` | Main-thread simulated world work in milliseconds; 0 disables it, 10 requests 10 ms. |
| `--game-load` | 0 disables simulated game jobs; 0.5 requests half a frame of game work per game thread. |
| `--space-send wide` | Let the client-range jobs write their clients' messages directly. |
| `--space-update split` | Run per-client network updates as jobs over client ranges. |
| `--space-net task` | Run network update and send work in the scheduled network task. |
| `--space-split top` | Start the network task before the main thread's world update so they can overlap. |
| `--key` | Server public key printed by `harden_ready`. |
| `--threads` | Number of client-machine threads, independent of the server pool: 32 for Linux server rows, 16 for IOCP. |
| `--messages 1` | One client input message per player per tick. |
| `--reliable-every 1` | Preserved client command setting; the server's reliable cadence is set separately by `--load`. |
| `--join-rate 100` | Connect approximately 100 players per second. |
| `--input-bytes 256` | Size of each client input message. |
| `--logs` | Comma-separated role logs read by `reconcile`. |

### Check placement and read the results

For XDP, the measured pool log reports `game_threads=21 first_cpu=3 pinned=21
realtime=21`: the main thread plus 20 game threads successfully pinned and
entered real-time scheduling. The library pins the two network threads to
CPUs 1–2. For io_uring, check for `io_uring worker 0 pinned to cpu 1` and
`io_uring worker 1 pinned to cpu 2`; a pin or priority refusal is logged.
Retain these startup lines with the result rather than assuming requested
affinity was granted.

| Result field | How to read it |
| --- | --- |
| `harden_tick t` | Seconds since the server run started. Select the row's stated steady window. |
| `harden_tick pkts_ps` | Change in server packets sent divided by the reporting interval. |
| `harden_tick frames` | Frames in that interval; divide by elapsed seconds between reports for frames/s. |
| `harden_tick conn` | Connected players at the report. |
| `harden_players connects` | Successful connection count; read with stable/unplanned ends and error counts. |
| `harden_players` delivery percentages | Unreliable delivered / (delivered + lost), separately for each direction, after draining. |
| `harden_threads main_net_ms` | Mean elapsed main-thread network work per frame in that reporting interval. |
| `harden_frame main_rn_ms p99/max` | Per-interval main-thread network-time p99 and maximum. XDP's rows without game work average the per-second p99 values. |
| `harden_frame latency_ms avg/max` | Time from the world state used for a frame's packets to its send call; not wire latency. |
| `harden_frame input_age_ms avg/max` | Time from the network update completing to the main thread taking its inputs for the world update. |
| `harden_frame join_ms avg/max` | Time the main thread waits for the network task to finish. |
| `harden_frame world_ms avg/max` | Elapsed world-update time, including waiting for its game jobs; may exceed the nominal 10 ms. |
| `harden_threads game_net_ms min/mean/max` | Minimum, mean, and maximum across game threads of each thread's network work per frame. |
| `harden_threads ratio` | `main_net_ms` / mean `game_net_ms`. |
| `harden_threads leftover_ms` | On the corrected XDP builds only: frame budget minus measured game work per game thread minus mean network work per game thread. |
| `harden_frame rn_core_ms` | On the corrected XDP builds: summed instrumented network work across the main/game pool, excluding dedicated network threads. |
| `harden_thread_net_ms` | Per-thread network work; index 0 is the main thread's contribution to jobs. |

For example, the io_uring peak row delivers 900,316 packets/s at 60.60
frames/s. Its main thread spends 0.157 ms per frame on network work and its
game threads average 3.20 ms each: 0.157 / 3.20 is about 0.05. This does not
mean the entire frame takes 0.157 ms, or that all thread times should be added
to obtain elapsed frame time.

## Profiles

### Record and inspect a Linux profile

The XDP profiles below used the same machines and thread layout as its rows
without game work. The profiled binary was [186a4f364](https://github.com/mas-bandwidth/rocketnet/commit/186a4f3646632119d64633e9e9404cd93e61c4ee),
on the measurement branch, built as target `harden_load` with
`ROCKETNET_DEV=ON`, Release `-O3 -DNDEBUG`, no `-g`, and no frame pointers.
The profiled peak and average rows had no game work and used the default
earlier frame order. This is a separate capture from the earlier throughput
rows at `79888a398`.

The server was sampled for eight seconds, from t=40 to t=48 of the
60-second run, using perf `7.0.14` and event `cpu/cycles/P`. The record
command omitted `-e`; the worker identified the selected event with
`perf evlist`. Although recording requested call graphs, this build does
not support reliable unwinding: use the self-time report below. The launcher
and row script still await their repository commit.

Start the chosen server and client row. On the server machine, the recorded
process-selection command was:

```sh
pid=$(pgrep -x harden_load | tail -1)
```

Use the server process ID if several load processes are running. The main
thread's ID equals this process ID. The run identified the remaining threads
by their names and last CPU from `/proc/$pid/task/*`. This helper performs
that inspection and prints the thread ID, name, and last CPU:

```sh
python3 - "$pid" <<'PY'
from pathlib import Path
import sys

pid = int(sys.argv[1])
print("TID\tNAME\tLAST_CPU")
for task in sorted(Path(f"/proc/{pid}/task").iterdir(), key=lambda p: int(p.name)):
    try:
        stat = (task / "stat").read_text()
        # Fields after the final ')' start at field 3; processor is field 39.
        fields = stat.rsplit(")", 1)[1].split()
        name = (task / "comm").read_text().strip()
        print(f"{task.name}\t{name}\t{fields[36]}")
    except FileNotFoundError:
        continue  # The thread exited during inspection.
PY
```

For this XDP layout, identify the main thread on CPU 3, two XDP threads on
CPUs 1–2, and 20 game threads on CPUs 4–23. Field 39 is the last CPU used,
so this snapshot complements the startup affinity logs; it does not prove
an affinity mask. See the [Linux stat-field reference](https://man7.org/linux/man-pages/man5/proc_pid_stat.5.html).

When `harden_tick` reaches t=40, record the following eight seconds. Use a
fresh output directory for each row so the capture and report stay together.
These are the supplied recording and reporting commands:

```sh
sudo perf record -F 999 -g -p "$pid" -o perf.data -- sleep 8
sudo perf report -f -i perf.data --no-children --sort pid,symbol -n --stdio -g none > perf.rep
```

The recording requests 999 samples/s (`-F`), call graphs (`-g`), attachment
to the server (`-p`), and the named output file (`-o`); `sleep 8` bounds the
capture. See [perf record](https://man7.org/linux/man-pages/man1/perf-record.1.html).

The report reads that file (`-i`), allows it despite an ownership mismatch
(`-f`), shows self samples (`--no-children`) and sample counts (`-n`), and
writes text (`--stdio`) without call graphs (`-g none`). In this report,
the `pid` sort key identifies a task's thread ID; `symbol` identifies the
function. There is no separate `tid` sort key in the measured perf version.
See [perf report](https://man7.org/linux/man-pages/man1/perf-report.1.html).

Match report thread IDs to the saved thread listing. Sum a function's sample
counts over the threads in the desired class, then estimate its milliseconds
per frame using the run owner's conversion:

```text
class ms/frame ≈ samples / 999 × 1000 / (frames_per_second × 8)
mean game-thread ms/frame ≈ class ms/frame / 20
mean XDP-thread ms/frame ≈ class ms/frame / 2
```

Use the frame rate from the capture interval. Sort functions by the resulting
self-time estimate within each class. These are sampling estimates assuming
the requested frequency, not exact timers; they need not match elapsed-call
measurements, which include waits. Keep kernel entries, marked `[k]`, and
the unlisted tail when considering a thread's total.

### XDP profile results

At peak, the profiled server sent about 895,000 packets/s; at average load,
about 480,000 packets/s. Estimated totals are per thread, per frame:

| Thread class | Peak (ms) | Average load (ms) |
| --- | ---: | ---: |
| Main thread | 0.33 | 0.33 |
| Mean game thread, 20 threads | 3.32 | 1.96 |
| Mean XDP thread, two threads | 16.5 | 16.5 |

The two XDP threads busy-poll: each occupies a full core at both measured
loads, and this polling design keeps them busy even when traffic is low.
Their cost is additional to the main and game threads' work.

The leading game-thread self times are below. `rn_bitmap_walk_bits_desc`
appears in acknowledgment application.

| Function | Peak ms/frame per game thread | Average ms/frame per game thread |
| --- | ---: | ---: |
| `harden_write_message` (load test) | 0.746 | 0.404 |
| `chacha20_encrypt_bytes__avx2` | 0.650 | 0.353 |
| `rocketnet_server_transport_start_packet` | 0.310 | 0.177 |
| `poly1305_blocks__sse2` | 0.292 | 0.161 |
| `rocketnet_ack_block_build` | 0.205 | 0.113 |
| `rocketnet_next_packet_lease_id` | 0.153 | 0.085 |
| `rocketnet_server_transport_queue_budget_now` | 0.112 | 0.073 |
| `rocketnet_server_socket_send_room` | 0.063 | 0.034 |
| `rocketnet_fill_phase_fill_end` | 0.055 | 0.033 |
| `send_room_queue` | 0.045 | Not supplied |
| `rn_bitmap_walk_bits_desc` | 0.042 | 0.030 |
| `rocketnet_server_transport_finish_packet` | 0.031 | Not supplied |

The main-thread leaders are:

| Function | Peak ms/frame | Average ms/frame |
| --- | ---: | ---: |
| `harden_read_message` (load test) | 0.194 | 0.213 |
| `rn_message_object_receive_messages` | 0.058 | 0.050 |
| `harden_server_main` (load test) | 0.025 | 0.021 |

The remaining peak main-thread functions each contribute less than 0.01 ms.
The peak XDP-thread leaders, averaged over its two threads, are:

| Function | Peak ms/frame per XDP thread |
| --- | ---: |
| `entry_SYSRETQ_unsafe_stack` [k] | 2.4 |
| `do_sys_poll` [k] | 1.9 |
| `rocketnet_server_socket_xdp_thread_function` | 1.54 |
| `native_sched_clock` [k] | 1.3 |
| `do_syscall_64` [k] | 1.3 |
| `__ct_user_exit` [k] | 0.52 |
| `__sys_sendto` [k] | 0.35 |
| `i40e_xsk_wakeup` [k] | 0.32 |
| `xsk_poll` [k] | 0.27 |

These lists omit smaller entries; their sums are not the thread totals.
As a worked reading, peak ChaCha20 and Poly1305 self time is
0.650 + 0.292 = 0.942 ms per game thread per frame, about 28% of its
3.32 ms total. That is the largest listed library cost when those encryption
and authentication functions are grouped. The load test's own message-body
write costs another 0.746 ms; it is application workload, not library time.
The main thread similarly spends 0.194 ms in the test's input-copying code.

Profiles for the other transports and rows with game work remain pending.
