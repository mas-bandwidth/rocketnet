# Exact-head 1000-client frame-call evidence

This procedure collects evidence for the open final-deliverable requirement in
`docs/roadmap.sexp` and `docs/testing.sexp`: every main call in a Release frame
must take **less than 100 µs** at 1000 clients on each platform. The current
`load.c` reports only `server_update_p50_us` and `server_update_p99_us`, so the
runner below deliberately says `COLLECTED_NOT_QUALIFIED`, never budget PASS.
Do not substitute the older `tools/check_frame_budget.py`: it measures a
32-packet client microbenchmark, not the 1000-client API loop.

Run only after the platform owner has an allocated, quiet host and the core
functional gates are settled. Use a clean exact-head checkout and a Release
build made from that checkout on the measurement host. Keep the build log,
`CMakeCache.txt`, source head, binary SHA-256, raw output, and receipt together.
The runner checks head, clean tree, Release cache, CMake source, physical and
available memory, fd hard limit, output space, and 1-minute load before each
run. A refused guard is `INCOMPLETE`; do not raise or disable it to get data.
Output must live outside the source tree. The preflight intentionally requires
14 GiB physical and 12 GiB available memory because historical macOS loads
reported roughly 10.5 GiB RSS. Revisit the minimum only with a documented
memory budget and a capacity owner, not a green-result adjustment.

Example after the platform owner builds `load` in Release:

```sh
src=/path/to/exact-head/rocketnet
build=/path/to/exact-head/release-build
head=$(git -C "$src" rev-parse HEAD)
python3 "$src/tools/profile_1000_load.py" \
  --mode timings --source "$src" --build-dir "$build" \
  --binary "$src/bin/load" --expected-head "$head" \
  --server-transport portable --client-transport portable \
  --seconds 60 --output /path/to/evidence/portable-timings
```

Run each supported server transport as a separate cell with three runs. The
runner keeps each raw load output and checks all five correctness criteria,
1000 connected clients, the granted transport, and fd/thread return to
baseline. It retains each run's server-update p99 and any future `frame_*`
fields. Do **not** average away a failing run; a qualifying gate must enforce
the strict per-call threshold on each run, keep counts and raw distributions,
and cover every main call. The existing `load.c` cannot make that claim.

For macOS sampled CPU attribution, use a separate run. Sampling adds overhead
and its wall-clock times must never be used for the latency gate:

```sh
python3 "$src/tools/profile_1000_load.py" \
  --mode samples --source "$src" --build-dir "$build" \
  --binary "$src/bin/load" --expected-head "$head" \
  --server-transport portable --client-transport portable \
  --seconds 60 --output /path/to/evidence/portable-samples
xcrun xctrace export --input /path/to/evidence/portable-samples/time-profiler.trace \
  --toc --output /path/to/evidence/portable-samples/trace-toc.xml
```

`xcrun xctrace help record` and `help export` on the local Apple toolchain
confirm `--template 'Time Profiler'`, `--launch --`, `--target-stdout`,
`--env`, `--time-limit`, and `export --toc`. Retain the `.trace` bundle and
table of contents. Inspect the **main thread** of the load process during the
steady 1000-client interval, annotate inclusive and self samples by
`rocketnet_server_update`, receive/protocol/ACK/message subpaths, and
`rocketnet_server_send_packets`; also record the transport worker threads
separately. Compare portable and msgx on the same host, exact head, offered
load and interval. Samples tell where CPU was spent, while separate direct
timing tells whether main calls meet the budget. Report unattributed and
blocked/wait time rather than assigning it to a guessed function.

## Nonoverlapping `load.c` timing proposal for its owner

The current traffic loop is at `load.c:820-1000` in intake head `4f7aebd`.
Its existing `server_update` timer is at 925-935. Do not change the 100-client
slice, 10 ms pace, offered messages, packet budgets, threshold, or population.

1. Add a fixed-capacity, per-call sample collector near the existing
   `update_us` allocation. Allocate once before traffic; never allocate in a
   timed region. Record `rocketnet_time()` immediately around each call and
   append a microsecond sample and count. Preserve full precision (double or
   integer nanoseconds) until percentile reporting. Fail explicitly if the
   buffer fills; do not silently drop tail samples.
2. Wrap calls at traffic-loop lines 832 (`client_update`), 835
   (`client_receive_messages`, including the final NULL call), 859
   (`client_receipts`), 874/878 (`client_start_message`/`finish_message`),
   905 (`client_send_packets`), 928 (`server_update`, reuse the existing
   measurement), 939 (`server_receive_messages`, including NULL), and 999
   (`server_send_packets`). Also wrap `client_state`, `client_index`, and
   `server_client_connected` queries actually used each frame, or explicitly
   enumerate and justify their exclusion. This is a loopback harness, so
   server and client timings must be reported separately.
3. Emit `frame_<api>_count=<n>` and `frame_<api>_p99_us=<value>` in the final
   single `load: connected=... verdict=...` summary, with the same value on
   PASS and FAIL paths. Use one documented p99 rank convention, report maxima
   alongside p99, and retain raw samples or a lossless trace for review. A
   count of zero means **unmeasured**, not a passing zero latency. The
   timing collector must not alter message/receipt consumption.
4. The public five-step loop also includes server/client release and abort
   calls in some integrations. If not exercised by this load, cover them in a
   separate 1000-client fixture before asserting **every** main call. Create,
   stop/disconnect and destroy have distinct lifecycle budgets and are not
   smuggled into the per-frame claim.

The script's `reported_frame_call_p99_us` field will retain any new summary
fields. It intentionally leaves `every_main_call_budget=UNMEASURED` until the
final gate defines and enforces the complete API-call set. Add a controlled
slow-call plant and prove the gate fails before relying on it. Keep raw
per-call samples, source/binary identity, exact commands, exit codes, host
capacity, profiler trace, and criteria with each platform receipt.
