# Soak

`examples/soak.c` is the stress demo and profiling harness: up to 1000 clients
against one server over a 1% lossy simulated link (development library) for N seconds at 60 Hz,
checking every message's integrity and order, and measuring server tick and
client update timings, packet/byte rates, and steady-state allocations. This is
a demo and a profiling instrument, not a load claim.

## What it shows

1. Pass a counting allocator to `rocketnet_init` that serves every SDK
   allocation from one arena. It reports `allocs/tick` during the
   steady-state soak (printed and written to the TSV, not a pass check), and
   the run fails if any SDK allocation is still live at teardown. The arena
   is `--arena-mib <n>` MiB; by default 2048 + 128 x clients MiB, allocated
   up front (on Linux reserved as address space, not resident bytes; on
   other platforms an ordinary allocation of that size). A run whose SDK
   allocations do not fit fails.
2. Connect `--clients <n>` (1..1000, default 1000) to one server in a single
   process over `loopback` by default. Both ends use the soak's own configs
   (the defaults with the soak's own budgets, buffers and windows set in
   `soak.c`), so a 1000-client run holds the server and all 1000 clients in one process
   and needs a large machine: `rocketnet_server_required_memory` and
   `rocketnet_client_required_memory` report what each takes in your build
   (see [Memory](../MEMORY.md)). Start with `--clients 32` on a smaller
   host. Loopback excludes kernel UDP/NIC cost;
   1000 real UDP clients in one process also need host socket-buffer planning. `--transport <name>` selects a candidate name/list for both endpoints; the
   endpoint environment overrides still take precedence. Use transports
   available on the host, and inspect the selected transport. XDP requires
   root and a suitable Linux NIC; see [Transports](../TRANSPORTS.md).
3. Run a deterministic 60 Hz step for `--seconds <n>` (default 3; use
   `--seconds 60` for a longer profile; the accepted duration is 1..120 seconds) with fixed `--seed <n>` (default 1),
   exchanging `ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED` state messages and
   `ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT` event messages in both directions.
   Every message carries client id, sequence, and FNV-1a body checksum. Missing
   reliable messages, backward sequence, or checksum mismatch fail the run;
   unreliable sequenced gaps are expected under loss
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

The older four-row scale table is retained in Git history. It lacked a pinned
revision, host, and raw receipt here, so it should not serve as a current
performance or memory baseline. Run the command above for a new TSV and record
its commit, build, host, transport, and workload alongside it. See
[Testing](../TESTING.md) for the maintained qualification evidence.

This workload deliberately reduces each client's send frequency at larger
counts: it uses `stride = max(1, clients / 25)`, rounded down, and selects clients by
`(client + tick) % stride == 0`. Thus a 1000-client run does not send state for
every client on every 60 Hz tick. The 100 microsecond figure is a timing target,
not a pass threshold or a guarantee.

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

<a id="public-calls-it-uses"></a>

Source: [`examples/soak.c`](../../examples/soak.c). See the
[integration guide](../USAGE.md) for shared lifecycle, threading, and message
ownership rules.
