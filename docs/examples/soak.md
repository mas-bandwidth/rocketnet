# Soak

`examples/soak.c` is the stress demo and profiling harness: up to 1000 clients
(default 1000) against one server over a simulated link that drops 1% of
packets each way, for N seconds of 60 Hz steps, checking every message's
integrity and order, and measuring server tick and client update timings,
packet/byte rates, and steady-state allocations. This is a demo and a
profiling instrument, not a load claim.

## What it shows

1. Install a counting allocator via `rocketnet_set_allocator` before
   `rocketnet_init`. It reports allocations per steady-state tick
   (`allocs/tick`) and peak bytes, and the run fails if any allocation is
   still live after teardown.
2. Connect `--clients <n>` (1..1000, default 1000) to one server in a single
   process over `loopback` by default (1000 real UDP sockets in one process on
   one machine need OS socket-buffer care; `loopback` measures the library, not
   the kernel). `--transport <name>` sets the transport on the server and every
   client, for example `portable`, `gso` on Linux, or `msgx` on macOS. `xdp`
   refuses here, because the server asks for an ephemeral port.
3. Step a simulated 60 Hz clock for `--seconds <n>` (1..120, default 3; use
   `--seconds 60` for full profiling) with the payload `--seed <n>` (default 1),
   exchanging 48-byte `ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED` state messages and,
   every sixth tick, 96-byte `ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT`
   event messages in both directions. Above 25 clients each client sends on
   one tick in `clients / 25`, so the message rate stays near 25 clients'
   worth while the connection count grows.
   Every message carries client id, sequence, and FNV-1a body checksum; any gap,
   backward sequence, or checksum mismatch prints `FAIL` naming client and sequence
   (`--self-test-corrupt` flips one byte of the first event before send to
   prove the check bites).
4. Record server tick and client per-peer update latency (`p50`, `p99`, `max`
   against the 100 µs per-client budget), packet/byte rates from public counters,
   and write `--out <file>` (default `soak-results.tsv`, one TSV row per metric
   with `clients`, `seconds`, `seed`, `transport`, `build`, `metric`, `value`).
5. Check the run: every event arrived and was acked, no corruption or order
   gap, no lost receipt, no budget drop, no unreleased message, no API misuse,
   every client disconnects cleanly, and nothing is left allocated. The
   `allocs/tick` figure is reported, not checked.

`--clients` outside 1..1000 or `--seconds` outside 1..120 exits 1 without
running.

## Run it and scale sweep

```sh
./bin/example_soak
./bin/example_soak --clients 1000 --seconds 60 --out soak-60s.tsv
```

A run prints one summary line, then `PASS`:

```
soak: clients=10 seconds=10 seed=1 transport=loopback rel=2000/2000 seq=11944/12000 srv_p50=66.2us srv_p99=120.8us cli_peer_p50=5.15us cli_peer_p99=9.27us (budget 100us) allocs/tick=0.00 peak_bytes=281816858
PASS
```

`rel` and `seq` are received over sent for events and state messages (state
messages lost on the lossy link are not resent). Timings depend on the
machine. A check that fails prints a `FAIL: ...` line and exits 1.

One 10-second sweep across 1, 10, 100, and 1000 clients (`--seconds 10 --seed 1`):

| clients | rel (rx/tx) | seq (rx/tx) | srv p50 (µs) | srv p99 (µs) | cli/peer p50 (µs) | cli/peer p99 (µs) | allocs/tick | peak_bytes |
|--------:|------------:|------------:|-------------:|-------------:|------------------:|------------------:|------------:|-----------:|
| 1 | 200/200 | 1194/1200 | 7.3 | 14.7 | 5.23 | 11.44 | 0.00 | 107,317,768 |
| 10 | 2000/2000 | 11941/12000 | 50.1 | 89.2 | 4.29 | 7.01 | 0.00 | 281,810,458 |
| 100 | 5000/5000 | 29843/30000 | 165.2 | 252.8 | 1.99 | 2.56 | 0.00 | 2,026,737,102 |
| 1000 | 5000/5000 | 29872/30000 | 4710.0 | 7000.6 | 4.00 | 6.10 | 0.00 | 19,476,003,670 |

## Profiling (`--pause`, `perf`, `xctrace`)

Build with symbols kept (`cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo`; the
repository's `Release` build also strips nothing by default). `--pause` prints
`PAUSED pid=<pid> (press Enter to start)` and waits for Enter on stdin so a
profiler can attach first; the pid is read from `/proc`, so it prints as 0
outside Linux:

```sh
# Linux perf + FlameGraph
perf record -F 999 -g --call-graph dwarf -- ./bin/example_soak --clients 1000 --seconds 60
perf script | stackcollapse-perf.pl | flamegraph.pl > soak-flamegraph.svg

# macOS Instruments (xctrace)
xctrace record --template 'Time Profiler' --output soak.trace --launch -- ./bin/example_soak --clients 1000 --seconds 60
```

## Public calls it uses

- `rocketnet_set_allocator`, `rocketnet_init`, `rocketnet_term`, `rocketnet_set_log_level`, `rocketnet_time`, `rocketnet_api_misuse_count`
- `rocketnet_server_dev_config_sized`, `rocketnet_server_create_sized`, `rocketnet_server_start`, `rocketnet_server_port`, `rocketnet_server_num_connected_clients`, `rocketnet_server_update`, `rocketnet_server_receive_message`, `rocketnet_server_release_message`, `rocketnet_server_receipts`, `rocketnet_server_start_message`, `rocketnet_server_finish_message`, `rocketnet_server_send_packets`, `rocketnet_server_counter`, `rocketnet_server_destroy`
- `rocketnet_client_default_config_sized`, `rocketnet_client_create_sized`, `rocketnet_client_connect`, `rocketnet_client_state`, `rocketnet_client_index`, `rocketnet_client_update`, `rocketnet_client_receive_message`, `rocketnet_client_release_message`, `rocketnet_client_receipts`, `rocketnet_client_start_message`, `rocketnet_client_finish_message`, `rocketnet_client_send_packets`, `rocketnet_client_disconnect`, `rocketnet_client_destroy`
- `rocketnet_write_dev_connect_token` (development library only)
