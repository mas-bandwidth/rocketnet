# Soak

`examples/soak.c` is the stress demo and profiling harness: up to 1000 clients
against one server over a 1% lossy simulated link (development library) for N seconds at 60 Hz,
checking every message's integrity and order, and measuring server tick and
client update timings, packet/byte rates, and steady-state allocations. This is
a demo and a profiling instrument, not a load claim.

## What it shows

1. Pass a counting allocator to `rocketnet_init` to verify `allocs/tick == 0` during the steady-state soak and zero leaks at teardown.
2. Connect `--clients <n>` (1..1000, default 1000) to one server in a single
   process over `loopback` by default (1000 real UDP sockets in one process on
   one machine need OS socket-buffer care; `loopback` measures the library, not
   the kernel). `--transport <name>` (or `ROCKETNET_SERVER_TRANSPORT`) selects
   `portable`, `gso`, `msgx`, or `xdp` (`xdp` requires root and its own machine).
3. Run a deterministic 60 Hz step for `--seconds <n>` (default 3; use
   `--seconds 60` for full profiling) with fixed `--seed <n>` (default 1),
   exchanging `ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED` state messages and
   `ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT` event messages in both directions.
   Every message carries client id, sequence, and FNV-1a body checksum; any gap,
   backward sequence, or checksum mismatch prints `FAIL` naming client and sequence
   (`--self-test-corrupt` flips one byte before send to prove the check bites).
4. Record server tick and client update latency per client (`p50`, `p99`, `max`
   against the 100 µs per-client budget), packet/byte rates from public counters,
   and write `--out <file>` (default `soak-results.tsv`, one TSV row per metric
   with `clients`, `seconds`, `seed`, `transport`, `build`, `metric`, `value`).

## Run it and scale sweep

```sh
./bin/example_soak
./bin/example_soak --clients 1000 --seconds 60 --out soak-60s.tsv
```

10-second sweep across 1, 10, 100, and 1000 clients (`--seconds 10 --seed 1`):

| clients | rel (rx/tx) | seq (rx/tx) | srv p50 (µs) | srv p99 (µs) | cli/client p50 (µs) | cli/client p99 (µs) | allocs/tick | peak_bytes |
|--------:|------------:|------------:|-------------:|-------------:|------------------:|------------------:|------------:|-----------:|
| 1 | 200/200 | 1194/1200 | 7.3 | 14.7 | 5.23 | 11.44 | 0.00 | 107,317,768 |
| 10 | 2000/2000 | 11941/12000 | 50.1 | 89.2 | 4.29 | 7.01 | 0.00 | 281,810,458 |
| 100 | 5000/5000 | 29843/30000 | 165.2 | 252.8 | 1.99 | 2.56 | 0.00 | 2,026,737,102 |
| 1000 | 5000/5000 | 29872/30000 | 4710.0 | 7000.6 | 4.00 | 6.10 | 0.00 | 19,476,003,670 |

## Profiling (`--pause`, `perf`, `xctrace`)

Build with symbols kept (`cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo`; the
repository's `Release` build also strips nothing by default). `--pause` prints
`PAUSED pid=<pid>` and waits for Enter on stdin so a profiler can attach first:

```sh
# Linux perf + FlameGraph
perf record -F 999 -g --call-graph dwarf -- ./bin/example_soak --clients 1000 --seconds 60
perf script | stackcollapse-perf.pl | flamegraph.pl > soak-flamegraph.svg

# macOS Instruments (xctrace)
xctrace record --template 'Time Profiler' --output soak.trace --launch -- ./bin/example_soak --clients 1000 --seconds 60
```

## Public calls it uses

- `rocketnet_init`, `rocketnet_term`, `rocketnet_log_level`
- `rocketnet_server_default_config`, `rocketnet_server_create`,
  `rocketnet_server_start`, `rocketnet_server_destroy`,
  `rocketnet_server_update`, `rocketnet_server_send_packets`,
  `rocketnet_server_ipv4_address`,
  `rocketnet_server_public_key`, `rocketnet_server_num_connected_clients`,
  `rocketnet_server_start_message`, `rocketnet_server_finish_message`,
  `rocketnet_server_receive_messages`, `rocketnet_server_receipts`,
  `rocketnet_server_stats`
- `rocketnet_client_default_config`, `rocketnet_client_create`,
  `rocketnet_client_connect`, `rocketnet_client_destroy`,
  `rocketnet_client_update`, `rocketnet_client_send_packets`,
  `rocketnet_client_disconnect`, `rocketnet_client_state`,
  `rocketnet_client_index`, `rocketnet_client_start_message`,
  `rocketnet_client_finish_message`, `rocketnet_client_receive_messages`,
  `rocketnet_client_receipts`
- `rocketnet_client_network_simulator`, `rocketnet_server_network_simulator`
- `rocketnet_create_dev_connect_token` and
  `ROCKETNET_DEV_BACKEND_PUBLIC_KEY` (development library only)
- `rocketnet_status_name`, `rocketnet_time`
