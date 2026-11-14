# AWS performance record: io_uring and GSO

Current state, measured 2026-11-13 between 22:30Z and 00:00Z on rocketnet v1.1.54. The tree is main e0f1a0fae with the load harness tools/v1.1.x/spec-verify merged in (tree 74e7be243, identical on both machines). These are reported measurements, transcribed for [Performance](../PERFORMANCE.md). Raw role logs are not published in this repository.


## Machines

| Role | Instance | CPU | Network | Private address |
| --- | --- | --- | --- | --- |
| Server | c8gn.8xlarge | 32 vCPU Graviton4 (Neoverse-V2), 61 GiB | ENA, ens34, 8 combined queues, MTU 9001 | SERVER_IP |
| Clients (1000 players) | c7gn.16xlarge | 64 vCPU Graviton3 (Neoverse-V1) | ENA, ens5, 32 combined queues, MTU 9001 | CLIENT_IP |

- **Placement:** both in us-east-1c, in one cluster placement group. All traffic runs between the private addresses.
- **System:** Ubuntu 26.04.1 LTS, kernel 7.0.0-1016-aws, ENA driver from the kernel.
- **Build:** GCC 15.2.0, CMake 4.2.3, liburing 2.14 from Ubuntu's archive (server only). Release build:
  ```
  cmake -DCMAKE_BUILD_TYPE=Release -DROCKETNET_DEV=ON -DROCKETNET_BUILD_TOOLS=ON -DROCKETNET_BUILD_DEV_LIBRARIES=ON -DROCKETNET_IO_URING=ON
  ```
  0 warnings. harden_load links the development library (rocketnet_dev) in every configuration.

## Settings

- **Socket limits (server only):** `net.core.rmem_max=25000000` and `net.core.wmem_max=1000000`, as in docs/PERFORMANCE.md. The image defaults are 4194304/4194304, and the clients keep them.
  - Both transports log what they were granted: send 1000000, receive 25000000.
- **Left as the image has them:**
  - qdisc: mq with fq_codel on both machines.
  - RPS: off.
  - NIC queue counts: unchanged.
  - IRQ affinity: the image default.
  - memlock: 8192 KiB.
  - `kernel.io_uring_disabled`: 0.
  - `kernel.sched_rt_runtime_us`: 950000.
- **Server privilege:** unprivileged. The network workers and game threads run at default priority.
- **CPU layout:** CPU 0 for the OS, then the network workers from CPU 1, then the main thread, then the game threads to CPU 31. `--game-threads` counts the main thread.
  - The library pins its own workers (worker q on CPU 1 + q); /proc confirmed it.
  - io_uring, 2 workers: workers on CPUs 1–2, main on 3, `--game-threads 29 --pin-first-cpu 3`.
  - gso, 1 worker: worker on CPU 1, main on 2, `--game-threads 30 --pin-first-cpu 2`.
  - gso, 2 workers: workers on CPUs 1–2, main on 3, `--game-threads 29 --pin-first-cpu 3`.
- **Worker counts:** io_uring's default is 2 and gso's default is 1. Each row passes its count explicitly (`--num-io-uring-workers`, `--num-gso-workers`).
- **ENA allowance counters:** bw_in, bw_out, pps, conntrack and linklocal `_allowance_exceeded` from `ethtool -S`, read on both machines before and after every row. Every delta was 0 in every row.
- **Other checks, both machines, every row:**
  - qdisc drops: 0.
  - UDP RcvbufErrors and SndbufErrors: 0.

## Workload

- **Players and frames:** 1000 players at 60 Hz, server in the space game frame shape. The flags:
  ```
  --frame-shape space --space-send wide --space-update split --space-net task --space-split top
  ```
- **Loads:**
  - Peak: `--load peak`, 15 x 1200 B per client per update, 900.3k pkt/s offered.
  - Average: `--load average`, 480.3k pkt/s offered.
- **Game load:**
  - None: `--game-load 0 --world-ms 0`.
  - Half: `--game-load 0.5 --world-ms 10`.
- **Clients:** each sends 256 B inputs, stamped for the receive-delay measure (`--input-stamp 40001` on both ends).
- **Run timing:** joins take 15 s, then 60 s are measured. The figures come from the server's per-second lines for t = 25..55 s.
- **Throughput:** from `/sys/class/net/<dev>/statistics` over a 20 s window, at 38–58 s into the row. Gb/s counts the L2 frame bytes the kernel reports.
- **Worker CPU:** user + system per pinned network worker over the same window, in % of one core.

Commands. They are run by `scripts/harden-pair --row space --seconds 60 --clients 1000 --threads 32` from the harness branch; T is the shared end time.

Server (as ubuntu, `ulimit -n 65536`). The fixed part:

```
harden_load server --address SERVER_IP:40000 --transport <io_uring|gso> --max-clients 1000 \
  --end-at T --quiet-seconds 1.5 --dev ens34 --warm-seconds 20 --sample-seconds 60 \
  --base 1 --burst 15 --burst-clients 3 --reliable-every 1 --big-every 60 --big-bytes 3000 \
  --frame-shape space --game-threads <29|30> --pin-first-cpu <3|2> --num-<io-uring|gso>-workers <N> \
  --space-send wide --space-update split --space-net task --space-split top \
  --game-load <0|0.5> --world-ms <0|10> --load <peak|average> --input-stamp 40001 --stray-table harden_cell
```

The `--load` flag replaces the base/burst shape with the space load. The harden_ready line reads `base=15 load=peak` (or `average`) and `sizes=1200`.

Clients:

```
harden_load clients --address SERVER_IP:40000 --key K --clients 1000 --threads 32 --end-at T \
  --quiet-seconds 1.5 --dev ens5 --messages 1 --reliable-every 1 --join-rate 100 \
  --input-bytes 256 --input-stamp 40001 --stray-table harden_cell
```

The reconcile runs `harden_load reconcile` on the control machine.

## Pass rule and columns

A row PASSes when all of these hold:
- the rate is at least 99.9% of the offered rate;
- frames per second are at least 60.0;
- 0 starts are refused;
- 0 messages are lost in either direction;
- the reconcile chains PASS.

Overruns are reported but do not decide a row on their own.

Columns:
- **pkt/s:** the server's sent packets.
- **Gb/s:** the server's interface tx. The clients' interface rx was 9.10–9.11 Gb/s at peak and 4.85–4.86 Gb/s at average in every passing row.
- **Delay:** the receive delay of the clients' inputs at the server, from the input stamp, in ms at 60 Hz (one update is 16.7 ms).
- **late_1ms:** the share of inputs later than their update by more than 1 ms.
- **Main p99 and max:** the main thread's rocketnet work per frame.

## io_uring, 2 workers (default)

All 4 workloads pass: 18 of 18 runs.

| Run | Verdict | pkt/s | fps | Gb/s | Refused | Overruns | Lost s2c / c2s | Delay mean (ms) | Delay p99 (ms) | late_1ms | Main p99 (ms) | Main max (ms) | Worker CPU |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| peak, no game load, r1 | PASS | 900.3k | 60.63 | 9.21 | 0 | 0 | 0 / 0 | 8.65 | 16.33 | 0.015% | 0.288 | 0.386 | 34% + 32% |
| peak, no game load, r2 | PASS | 900.3k | 60.60 | 9.21 | 0 | 0 | 0 / 0 | 4.80 | 16.83 | 0.036% | 0.289 | 0.342 | 34% + 34% |
| peak, no game load, r3 | PASS | 900.3k | 60.57 | 9.21 | 0 | 0 | 0 / 0 | 9.55 | 16.17 | 0.012% | 0.289 | 0.349 | 32% + 34% |
| average, no game load, r1 | PASS | 480.3k | 60.57 | 4.91 | 0 | 0 | 0 / 0 | 9.77 | 16.67 | 0.071% | 0.262 | 0.390 | 22% + 22% |
| average, no game load, r2 | PASS | 480.3k | 60.60 | 4.91 | 0 | 0 | 0 / 0 | 9.10 | 16.17 | 0.008% | 0.274 | 0.397 | 22% + 22% |
| average, no game load, r3 | PASS | 480.3k | 60.67 | 4.91 | 0 | 0 | 0 / 0 | 4.30 | 11.00 | 0.009% | 0.269 | 0.377 | 24% + 22% |
| average, half game load, r1 | PASS | 480.3k | 60.63 | 4.91 | 0 | 0 | 0 / 0 | 8.43 | 18.83 | 0.063% | 0.329 | 0.374 | 22% + 22% |
| average, half game load, r2 | PASS | 480.3k | 60.63 | 4.91 | 0 | 0 | 0 / 0 | 10.13 | 18.67 | 0.068% | 0.332 | 0.432 | 22% + 21% |
| average, half game load, r3 | PASS | 480.3k | 60.53 | 4.91 | 0 | 0 | 0 / 0 | 10.72 | 17.67 | 0.034% | 0.282 | 0.383 | 22% + 21% |

### Peak with half game load: 9 runs

The first three runs and six more, run later the same night.

| Run | Verdict | pkt/s | fps | Gb/s | Refused | Overruns | Lost s2c / c2s | Delay mean (ms) | Delay p99 (ms) | late_1ms | Main p99 (ms) | Main max (ms) | Worker CPU |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| r1 | PASS | 900.3k | 60.53 | 9.21 | 0 | 0 | 0 / 0 | 9.00 | 18.33 | 0.056% | 0.305 | 0.360 | 34% + 32% |
| r2 | PASS | 900.3k | 60.57 | 9.21 | 0 | 0 | 0 / 0 | 8.53 | 19.33 | 0.158% | 0.373 | 0.695 | 32% + 32% |
| r3 | PASS | 900.3k | 60.63 | 9.20 | 0 | 0 | 0 / 0 | 9.42 | 20.00 | 0.081% | 0.382 | 0.493 | 32% + 32% |
| s1 | PASS | 900.3k | 60.57 | 9.21 | 0 | 0 | 0 / 0 | 8.38 | 20.33 | 0.208% | 0.424 | 1.104 | 32% + 31% |
| s2 | PASS | 900.3k | 60.50 | 9.21 | 0 | 0 | 0 / 0 | 7.63 | 17.17 | 0.029% | 0.295 | 0.325 | 33% + 33% |
| s3 | PASS | 900.3k | 60.63 | 9.21 | 0 | 0 | 0 / 0 | 9.62 | 19.67 | 0.076% | 0.398 | 0.467 | 32% + 31% |
| s4 | PASS | 899.6k | 60.57 | 9.20 | 0 | 2 | 0 / 0 | 8.20 | 18.50 | 0.077% | 0.305 | 0.523 | 33% + 32% |
| s5 | PASS | 900.3k | 60.60 | 9.21 | 0 | 0 | 0 / 0 | 10.88 | 19.67 | 0.085% | 0.373 | 0.481 | 32% + 32% |
| s6 | PASS | 900.3k | 60.50 | 9.21 | 0 | 0 | 0 / 0 | 8.85 | 18.00 | 0.151% | 0.299 | 0.689 | 33% + 33% |

Over the 9 runs:
- Delay mean: 7.63–10.88 ms, average 8.95.
- Delay p99: 17.17–20.33 ms.
- late_1ms: 0.029–0.208%.
- Main p99: 0.295–0.424 ms, median 0.373.
- Main max: 0.325–1.104 ms.
- Refused: 0 in every run.

## gso, 1 worker (default)

At 1 worker, gso passes peak with no game load and average with no game load: 3 of 3 runs each. Average with half game load passes 2 of 3; run 1 ran 0.2% under the offered rate. Peak with half game load fails in every run: 6 of 6. It runs at 842–872k pkt/s and 56.7–58.6 fps, with 662–963 overruns.

| Run | Verdict | pkt/s | fps | Gb/s | Refused | Overruns | Lost s2c / c2s | Delay mean (ms) | Delay p99 (ms) | late_1ms | Main p99 (ms) | Main max (ms) | Worker CPU |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| peak, no game load, r1 | PASS | 900.3k | 60.40 | 9.21 | 0 | 0 | 0 / 0 | 6.75 | 19.50 | 0.015% | 0.353 | 0.450 | 61% |
| peak, no game load, r2 | PASS | 900.3k | 60.57 | 9.21 | 0 | 0 | 0 / 0 | 7.92 | 15.67 | 0.084% | 0.269 | 0.398 | 59% |
| peak, no game load, r3 | PASS | 900.3k | 60.57 | 9.21 | 0 | 0 | 0 / 0 | 7.67 | 18.33 | 0.053% | 0.326 | 0.450 | 61% |
| peak, half game load, r1 | FAIL (rate, fps) | 869.2k | 58.37 | 8.89 | 0 | 683 | 0 / 0 | 8.87 | 20.17 | 0.271% | 0.329 | 0.557 | 58% |
| peak, half game load, r2 | FAIL (rate, fps) | 849.8k | 57.13 | 8.70 | 0 | 963 | 0 / 0 | 9.05 | 20.17 | 0.295% | 0.352 | 0.601 | 56% |
| peak, half game load, r3 | FAIL (rate, fps) | 857.3k | 57.60 | 8.77 | 0 | 762 | 0 / 0 | 9.10 | 20.83 | 0.232% | 0.378 | 0.504 | 57% |
| peak, half game load, r4 | FAIL (rate, fps) | 844.7k | 56.77 | 8.61 | 0 | 879 | 0 / 0 | 9.22 | 21.83 | 0.262% | 0.392 | 0.579 | 56% |
| peak, half game load, r5 | FAIL (rate, fps) | 842.0k | 56.67 | 8.61 | 0 | 930 | 0 / 0 | 9.22 | 21.00 | 0.208% | 0.361 | 0.493 | 55% |
| peak, half game load, r6 | FAIL (rate, fps) | 871.6k | 58.57 | 8.93 | 0 | 662 | 0 / 0 | 8.68 | 20.17 | 0.200% | 0.350 | 0.547 | 58% |
| average, no game load, r1 | PASS | 480.3k | 60.47 | 4.91 | 0 | 0 | 0 / 0 | 10.58 | 17.50 | 0.070% | 0.284 | 0.365 | 36% |
| average, no game load, r2 | PASS | 480.3k | 60.57 | 4.91 | 0 | 0 | 0 / 0 | 7.02 | 18.00 | 0.042% | 0.334 | 0.477 | 41% |
| average, no game load, r3 | PASS | 480.3k | 60.47 | 4.91 | 0 | 0 | 0 / 0 | 6.03 | 13.50 | 0.010% | 0.261 | 0.352 | 41% |
| average, half game load, r1 | FAIL (rate) | 479.3k | 60.43 | 4.91 | 0 | 5 | 0 / 0 | 8.37 | 18.83 | 0.074% | 0.338 | 0.499 | 38% |
| average, half game load, r2 | PASS | 480.3k | 60.37 | 4.91 | 0 | 12 | 0 / 0 | 9.40 | 18.33 | 0.106% | 0.314 | 0.349 | 37% |
| average, half game load, r3 | PASS | 480.3k | 60.50 | 4.91 | 0 | 0 | 0 / 0 | 8.53 | 17.67 | 0.031% | 0.265 | 0.377 | 39% |

The six loaded-peak runs come from two builds of the same tree, r1–r3 and r4–r6. Both builds link the same development library.

## gso, 2 workers

At 2 workers, peak with half game load passes 3 of 3. This is the workload that fails at 1 worker. The other three workloads were not run at 2 workers.

| Run | Verdict | pkt/s | fps | Gb/s | Refused | Overruns | Lost s2c / c2s | Delay mean (ms) | Delay p99 (ms) | late_1ms | Main p99 (ms) | Main max (ms) | Worker CPU |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| peak, half game load, r1 | PASS | 900.3k | 60.47 | 9.21 | 0 | 0 | 0 / 0 | 9.93 | 19.67 | 0.176% | 0.358 | 0.834 | 31% + 31% |
| peak, half game load, r2 | PASS | 900.3k | 60.67 | 9.21 | 0 | 0 | 0 / 0 | 5.68 | 18.00 | 0.039% | 0.292 | 0.360 | 32% + 34% |
| peak, half game load, r3 | PASS | 900.1k | 60.60 | 9.21 | 0 | 1 | 0 / 0 | 9.12 | 20.00 | 0.263% | 0.404 | 1.191 | 31% + 30% |

## Summary

| Transport | Workers | What passes | What fails |
| --- | --- | --- | --- |
| io_uring | 2 (default) | All 4 workloads, 18 of 18 runs | Nothing |
| gso | 1 (default) | Peak, no game load (3 of 3); average, no game load (3 of 3); average, half game load (2 of 3) | Peak with half game load (0 of 6); one average, half game load run 0.2% under rate |
| gso | 2 | Peak with half game load, 3 of 3 (the only workload run) | Nothing |

The measurement record includes server and client logs, reconciliation output,
NIC, qdisc, UDP and thread snapshots held by the measurement owner. Those raw
receipts are not published here. This table is a reported result record, not
an independent rerun of the build. `SERVER_IP`, `CLIENT_IP`, `T` and `K` in
the command templates denote the two private addresses, shared end time and
server-advertised key.
