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
| XDP | Latitude m4.metal.large, AMD EPYC 9254, 24 cores | Second m4.metal.large, 48 logical CPUs; Intel X710 10 GbE | Two XDP threads on CPUs 1–2; main thread on CPU 3; 20 game threads on CPUs 4–23; all 21 pool threads at real-time priority |
| io_uring | AWS c6in.8xlarge, 32 vCPUs, x86, Linux 7.0 | AWS c7gn.8xlarge, 32 vCPUs, arm64; same placement, private network | Two network threads on CPUs 1–2; main thread on CPU 3; 28 game threads on CPUs 4–31, real-time priority |
| GSO | AWS c8gn.8xlarge, Graviton4, arm64, 32 vCPUs; Ubuntu 26.04, GCC 15.2 | AWS c6gn.16xlarge, 64 vCPUs; same placement, private network | Two network threads on CPUs 1–2; main thread on CPU 3; 28 game threads on CPUs 4–31; no real-time priority |
| RIO / IOCP | AWS m7i.16xlarge, Windows Server 2025 | Pending | Pending |
| macOS | Owned Macs; exact models pending | Pending | Pending |

For io_uring, CPU 0 also handles NIC interrupts. The library pins each network
thread to the number of OS CPUs plus its queue index: CPUs 1–2 in these runs.
XDP also pins its own network threads. In the GSO rows, the load test pins
the network threads with `--net-first-cpu 1`; library-owned GSO pinning is
pending a new measurement. XDP and io_uring clients run 32 unpinned threads;
GSO uses 32 client threads, with affinity confirmation pending.

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
| GSO | Peak | 900,000 | 60.7 | 100% | 100% |
| GSO | Average | 480,000 | 60.5 | 100% | 100% |

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

The io_uring average-load ratio is calculated from the reported rounded means:
0.147 / 1.81. The other ratios were supplied with their measurement rows.
For XDP, the p99 column is the mean of the per-second p99s in the steady
window, not a p99 computed over all frames in that window.

| Rows | Build | Steady window | Simulated game work |
| --- | --- | --- | --- |
| XDP peak / average | [79888a398](https://github.com/mas-bandwidth/rocketnet/commit/79888a3980a42db26fa0d84c22b1df6f4e1ee3f9), measurement branch, server binary | t=25–55 s | `--world-ms 0 --game-load 0` |
| io_uring peak / average | [6db162421](https://github.com/mas-bandwidth/rocketnet/commit/6db1624212079133de226fe08cb8111bd70e352d), `tests/v1.2/goal-uring`, both machines | t=25–75 s, after joining and warm-up | `--world-ms 0 --game-load 0` |
| GSO peak / average | [d9110a19f](https://github.com/mas-bandwidth/rocketnet/commit/d9110a19fedf098a15068ce9a4cb9106f4ee603d), `tests/v1.2/goal-gso` | t=30–60 s | `--world-ms 10 --game-load 0` |

The io_uring rows use the earlier frame order with no simulated game work.
GSO starts its network task before the world update with `--space-split top`.
Its main thread performs 10 ms of simulated world work per frame while its
game threads carry only network work. XDP and io_uring have no simulated
world work in the rows above.
The GSO rates are means over its steady window. Its server requested a 25 MB
socket receive buffer and the OS granted 4 MB, limited by `net.core.rmem_max`.
There was no reported loss at this load; check the requested and granted
values in the transport log when configuring another machine.

Rows with game threads half busy with game work are pending for every transport:
the test's simulated world update was serial, corrected in `4fc1ae05b`.
Raw result logs are pending;
the timing percentiles are reported values, not recomputed here. Confirmation
of the io_uring and GSO percentile aggregation is pending. Different machines, builds, and
sampling windows make these workload results unsuitable for ranking transports
against one another.

| Measurement still pending | Status |
| --- | --- |
| XDP with game threads half busy with game work | Pending |
| io_uring with game threads half busy with game work | Pending |
| GSO with game threads half busy with game work | Pending |
| RIO peak and average | Pending |
| IOCP peak and average | Pending |
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
available at `./bin/harden_load`. Additional profiling build flags are pending.

The current `--game-threads N` flag counts the main thread plus N−1 game
threads. XDP uses 21; io_uring and GSO use 29. `--world-ms` sets simulated work
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

The supplied GSO commands differ as follows. Set `LOAD` to `peak` or
`average`, use its measured commit, and preserve the 10 ms world-work
setting. The archived launcher and its stop-time
arguments are pending; these are the binary workload arguments supplied
with the row:

```sh
./bin/harden_load server --address "$SERVER_IP:40000" \
  --transport gso --max-clients 1000 --dev "$SERVER_DEV" \
  --sample-seconds 60 --num-xdp-queues 2 \
  --frame-shape space --game-threads 29 --pin-first-cpu 3 --net-first-cpu 1 \
  --space-send wide --space-update split --space-net task \
  --world-ms 10 --space-split top --game-load 0 --load "$LOAD" > server.log 2>&1
```

After copying the server public key, run on the client machine:

```sh
./bin/harden_load clients --address "$SERVER_IP:40000" --key "$SERVER_KEY" \
  --clients 1000 --threads 32 --messages 1 --reliable-every 1 \
  --join-rate 100 --input-bytes 256 > clients.log 2>&1
```

Reconcile the logs as above. GSO uses the tool's default 60 Hz and 60-second
duration; the supplied command does not override its default pattern payload.

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
| `--num-xdp-queues 2` | Request two network queues; also controls io_uring and GSO queue counts. |
| `--frame-shape` | Select the test's job-based frame loop. |
| `--load` | Set the peak or average server traffic from the workload table. |
| `--game-threads` | Pool size including the main thread. |
| `--pin-first-cpu 3` | Pin the main thread to CPU 3 and successive game threads to successive CPUs. |
| `--net-first-cpu 1` | Have the load test pin GSO network threads starting at CPU 1 in this build. |
| `--world-ms` | Main-thread simulated world work in milliseconds; 0 disables it, 10 requests 10 ms. |
| `--game-load 0` | Disable simulated game jobs. |
| `--space-send wide` | Let the client-range jobs write their clients' messages directly. |
| `--space-update split` | Run per-client network updates as jobs over client ranges. |
| `--space-net task` | Run network update and send work in the scheduled network task. |
| `--space-split top` | Start the network task before the main thread's world update so they can overlap. |
| `--key` | Server public key printed by `harden_ready`. |
| `--threads 32` | Number of client-machine threads, independent of the server pool size. |
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
| `harden_frame main_rn_ms p99/max` | Per-interval main-thread network-time p99 and maximum. XDP's table averages the per-second p99 values. |
| `harden_threads game_net_ms min/mean/max` | Minimum, mean, and maximum across game threads of each thread's network work per frame. |
| `harden_threads ratio` | `main_net_ms` / mean `game_net_ms`. |
| `harden_thread_net_ms` | Per-thread network work; index 0 is the main thread's contribution to jobs. |

For example, the io_uring peak row delivers 900,316 packets/s at 60.60
frames/s. Its main thread spends 0.157 ms per frame on network work and its
game threads average 3.20 ms each: 0.157 / 3.20 is about 0.05. This does not
mean the entire frame takes 0.157 ms, or that all thread times should be added
to obtain elapsed frame time.

## Profiles

The measured Linux profiling procedure is pending: the exact build flags,
`perf record` event/frequency/duration and steady-window start, and the
per-thread report commands will be published with the XDP profile. The
launcher and row script are also awaiting their repository commit.

Each profile will rank functions by measured milliseconds per frame for its
thread and include a worked reading of the captured result. No profile command
is presented as having been run, and no function cost is estimated, until that
evidence arrives.
