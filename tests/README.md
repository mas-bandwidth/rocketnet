# tests/

Test-only vendored dependencies. Nothing here ships in the sdk; the sdk
directory remains the complete drop-in set.

- `serialize.h` — mas-bandwidth/serialize 1.15, vendored so the serialize
  read of rocketnet.h (the C++ stream surface) is compiled and exercised by
  `bin/test_serialize`. Refresh it when the serialize pin moves.
- `wycheproof/` — C2SP/wycheproof known-answer vectors for the primitives
  rocketnet calls (ChaCha20-Poly1305, X25519, Ed25519), read by
  tests/wycheproof.h. See tests/wycheproof/README.md for the pin and hashes.

## Running the suite

`./bin/test` and `./bin/functional` print one line per test, its name, in
suite order, then `*** ALL TESTS PASSED ***`, as netcode's tests do. Every
name is `test_` then lowercase snake_case, and the suite fails before it runs
anything when a registered name is not (`suite_name_is_test_snake_case` in
`tests/suite_runner.h`). A failing test prints everything it wrote, including
its `check failed:` line, and the suite exits 1 at once. Each case's output is
captured and dropped when it passes; set `ROCKETNET_TEST_VERBOSE=1` to skip
the capture and watch the log live. A test that a platform does not build is
not registered there, so it is neither run nor printed.

The repository text audits that used to be unit cases — the client/server
"says messages, not packets" scan (`test_client_server_say_messages_not_packets`),
the environment-name documentation check (`test_config_env_every_name_read_is_documented`),
the counter-named-by-a-test check (`test_counters_every_counter_is_named_by_a_test`),
the one-family-per-outcome check (`test_counters_every_outcome_in_at_most_one_family`)
and the XDP counter-row check (`test_xdp_transport_host_every_counter_has_a_case`) —
run in the static source-audit binary `./bin/source_audit`, not `./bin/test`. It
is a source-policy gate: it reads source, header and doc files through
`ROCKETNET_SOURCE_DIR`, opens no socket, starts no thread, and CI runs it on
a Linux leg beside `tools/check_client_server_words.py`.

Shared-library export cases are functional because they load a separately
built library through the OS loader: `test_api_polish_dev_token_string`,
`test_dev_connect_token_timeout_is_the_named_constant`, and
`test_dev_token_minting_absent_in_release`. They are registered in
`functional/rocketnet_functional_tests.cpp` and run by `./bin/functional`,
not `./bin/test`.

Functional cases run one at a time, even with `ROCKETNET_TEST_JOBS` set.
Use generous completion deadlines to catch hangs; do not assert wall-clock
latency or receive-gap thresholds. Unit tests use deterministic inputs and
simulated time for protocol deadlines, never host scheduling speed. See
[BUILDING.md](../BUILDING.md) for the 60-second target and 120-second cap.

Unit cases do not read or change the process environment. SDK environment
parsing and validation use the in-memory table in `test_env_source.h`, which
is available only in the unit hook library. The functional environment-wiring
cases change and restore real process variables and observe each SDK reader.

The process thread-count cases `test_server_stats_creates_no_thread` and
`test_public_step_driven_client_connects_without_a_thread` read the process's
OS thread count from `/proc/self/task`, so they are functional cases in
`./bin/functional`, not unit cases. Their bodies are in
`functional/process_thread_counts.cpp`. On platforms without that file the
count is -1 and the thread assertion pins nothing, so it is never unit logic.

## Peak memory per case

Both runners record each case process's peak resident set (`wait4`
`ru_maxrss` on POSIX, normalised from kilobytes on Linux and bytes on macOS;
`PeakWorkingSetSize` on Windows). With `ROCKETNET_TEST_REPORT=1` both
runners print, after the results, a `slowest cases:` block and a `peak rss:`
block of the ten largest (`suite_print_report`). A case whose peak is over the
bound fails the suite with `check failed: <case> peak_rss_mb=<n> bound_mb=<b>`. The bound is
`ROCKETNET_TEST_MAX_CASE_RSS_MB`, 698 MiB in `tests/suite_runner.h` (the larger
of 256 MiB and 1.5 times the largest measured case, never over 1 GiB); the
environment variable of the same name overrides it with a positive value. A sanitizer build
prints the peaks and enforces no bound unless that variable sets one, because
sanitizer shadow memory is not the case's own footprint.

A case that intentionally constructs a full-capacity server can declare a
larger case-specific bound beside its registration:

```cpp
static long my_case_rss_bound_mb()
{
    return suite_rss_bound_mb( planned_server_bytes, 64 );
}
SUITE_CASE_RSS_BOUND( my_case, my_case_rss_bound_mb );
```

The function computes its bound from the SDK memory plan for the exact config
the case uses, then adds a stated process margin with `suite_rss_bound_mb`.
Without `ROCKETNET_TEST_MAX_CASE_RSS_MB`, the runner uses that declared bound
for the named case and the suite-wide bound for every other case. A positive
environment override replaces the suite-wide bound and lifts a declared bound
under it, but never lowers a declared bound: a Windows Debug job's override
does not take a full-table case below its plan. Sanitizer builds continue to
report without enforcing a bound unless the environment sets one. A declared
plan of zero falls back to the suite-wide bound, so a refused memory plan
cannot silently disable the check.
