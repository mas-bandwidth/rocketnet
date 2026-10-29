# Exact-head 1000-client frame-call evidence

This procedure collects evidence for the open final-deliverable requirement in
`docs/roadmap.sexp` and `docs/testing.sexp`: every main call in a Release frame
must take **less than 100 µs** at 1000 clients on each platform. `tests/load/load.c`
reports counts, exact maxima and threshold misses, and histogram percentile
upper bounds for the calls it exercises during traffic and drain. Coverage
still excludes other public calls and setup/teardown, so the runner says
`COLLECTED_NOT_QUALIFIED`, never an every-main-call budget PASS.
Do not substitute the older `tools/check_frame_budget.py`: it measures a
32-packet client microbenchmark, not the 1000-client API loop.

Run only after the platform owner has an allocated, quiet host and the core
functional gates are settled. Use a clean exact-head checkout. The runner
creates a **fresh Release build inside its new evidence directory**, builds
only the `load` target with at most four jobs, and executes that exact output.
This binds binary provenance to the checked source rather than accepting an
arbitrary executable. Keep configure/build logs, `CMakeCache.txt`, source
head, binary SHA-256, raw output, and receipt together. The runner checks
physical and available memory, fd hard limit, output space, and 1-minute
load before configure and before each run. A refused guard is `INCOMPLETE`;
do not raise or disable it to get data.
Output must live outside the source tree. The preflight intentionally requires
14 GiB physical and 12 GiB available memory because historical macOS loads
reported roughly 10.5 GiB RSS. Revisit the minimum only with a documented
memory budget and a capacity owner, not a green-result adjustment.

Example on the allocated platform host:

```sh
src=/path/to/exact-head/rocketnet
head=$(git -C "$src" rev-parse HEAD)
python3 "$src/tools/profile_1000_load.py" \
  --mode timings --source "$src" --expected-head "$head" \
  --server-transport portable --client-transport portable \
  --seconds 60 --output /path/to/evidence/portable-timings
```

Run each supported server transport as a separate cell with three runs. The
runner keeps each raw load output and checks all five correctness criteria,
1000 connected clients, the granted transport, and fd/thread return to
baseline. All four baseline fd/thread fields must exist as nonnegative
integers. It retains each run's legacy server-update p99 and any `frame_*`
histogram fields, listing missing call names and exact at-or-above-budget
counts. A present category with count zero and zero percentile/max/miss fields
is retained as `UNMEASURED`; it is distinct from an absent category. Missing
fields or nonzero values for a zero-sample category make the receipt
`INCOMPLETE`. Neither state qualifies the every-main-call budget. Do **not**
average away a failing run; a qualifying gate must enforce
the strict per-call threshold on each run, keep counts and raw distributions,
and cover every main call. The existing `tests/load/load.c` cannot make that claim.

For macOS sampled CPU attribution, use a separate run. Sampling adds overhead
and its wall-clock times must never be used for the latency gate:

```sh
python3 "$src/tools/profile_1000_load.py" \
  --mode samples --source "$src" --expected-head "$head" \
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

## Measurement scope and remaining qualification

The integrated collector preserves the 100-client slice, 10 ms pace,
offered messages and acceptance criteria. The following requirements define
its measurements and the remaining work before a complete budget gate.

1. Add bounded fixed histograms near the existing `update_us` accounting.
   Allocate all buckets before traffic; never allocate in a timed region.
   Record `rocketnet_time()` immediately around each call, increment the
   correct bin, exact invocation count, exact maximum and exact count of calls
   at or above 100 µs. Do not silently drop or overwrite observations. Label
   histogram percentiles as **upper bounds** at their stated bucket
   resolution; report overflow explicitly.
2. Measure `client_update`, `client_receive_messages` (including the final
   NULL call), `client_receipts`, `client_start_message`/`finish_message`,
   `client_send_packets`, `server_update`, `server_receive_messages`
   (including NULL), and `server_send_packets`. Also measure `client_state`, `client_index`, and
   `server_client_connected` queries actually used each frame, or explicitly
   enumerate and justify their exclusion. Both endpoints run in one process, so
   server and client timings must be reported separately.
3. Emit `frame_<api>_count=<n>`, `_p50_upper_us`, `_p99_upper_us`, `_max_us`
   and `_overbudget_count` in the final single
   `load: connected=... verdict=...` summary, with the same value on PASS
   and FAIL paths. The currently agreed call set is `client_update`,
   `client_send_packets`, `client_state`, `client_index`,
   `client_receive_messages`, `client_receipts`, `client_start_message`,
   `client_finish_message`, `server_update`, `server_send_packets`,
   `server_client_connected`, `server_num_connected_clients`,
   `server_receive_messages`, `server_start_message`, and
   `server_finish_message`. Use one documented p99 rank convention and retain raw
   histogram buckets. A count of zero means **unmeasured**, not a passing
   zero latency. The timing collector must not alter message/receipt
   consumption, and calibrated clock overhead is never subtracted from the
   reported call latencies.
4. The public five-step loop also includes server/client release and abort
   calls in some integrations. If not exercised by this load, cover them in a
   separate 1000-client fixture before asserting **every** main call. Create,
   stop/disconnect and destroy have distinct lifecycle budgets and are not
   smuggled into the per-frame claim.

The script's `reported_frame_calls` field will retain any new summary
histograms. The `overbudget_count` is exact for elapsed time **at or above**
100 µs; any positive count is a strict-threshold miss even if the p99 upper
bound is below 100 µs. It intentionally leaves
`every_main_call_budget=UNMEASURED` until the
final gate defines and enforces the complete API-call set. Add a controlled
slow-call plant and prove the gate fails before relying on it. Keep raw
per-call histograms, source/binary identity, exact commands, exit codes, host
capacity, profiler trace, and criteria with each platform receipt.
