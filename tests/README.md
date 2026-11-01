# tests/

Tests, fixtures, workload harnesses and test-only vendored dependencies.
Nothing here ships in the sdk; the sdk directory remains the complete drop-in
set. Commands below run from the repository root unless stated otherwise.

| Directory | Purpose |
| --- | --- |
| `functional/` | Functional tests and platform/transport integration cases |
| `fuzz/` | Fuzz harnesses, dictionaries and committed seed corpora |
| `oss-fuzz/` | Prepared OSS-Fuzz build integration; not submitted |
| `client_server/` | Client/server workload programs |
| `load/`, `soak/` | Load and soak workloads |
| `pcapng/` | Packet captures and regression fixtures |

## Vendored test dependencies

- `serialize.h` — mas-bandwidth/serialize 1.15, vendored so the serialize
  read of rocketnet.h (the C++ stream surface) is compiled and exercised by
  `bin/test_serialize`. Refresh it when the serialize pin moves.
- `wycheproof/` — C2SP/wycheproof known-answer vectors for the primitives
  rocketnet calls (ChaCha20-Poly1305, X25519, Ed25519), parsed by
  tests/wycheproof.h. See tests/wycheproof/README.md for the pin and hashes.

## Fixtures and where each case runs

A unit case reads no file and writes none. The fixed known-answer files the
unit cases check against, `standard/vectors/connect_token_v1.json` and the three
`wycheproof/*_test.json` files, are compiled into `./bin/test` at build time by
`cmake/EmbedTestFixtures.cmake` (portable CMake, `file(READ ... HEX)`) and read
from memory through `rocketnet_test_fixture`; editing one rebuilds the unit
binary. Their cases (`test_vectors_regenerate_byte_identical`,
`test_each_vector_verifies_and_opens`, `test_tampered_vector_refused` and the
`test_wycheproof_*` cases) are unit cases.

`standard/vectors/connect_token_v1.json` is written only by the tool
`./bin/connect_token_vectors <path>` (`tools/connect_token_vectors/main.cpp`),
which mints it from the same generator the unit case checks and writes only
the path it is given, through a temporary file renamed over it:

```
./bin/connect_token_vectors standard/vectors/connect_token_v1.json
```

then rebuild, and `test_vectors_regenerate_byte_identical` checks the new copy.

The corpus-consistency cases walk directories that grow as findings land
(`pcapng/` with `pcapng/regress/`, and `tests/fuzz/corpus/`), so they are functional
cases in `tests/functional/corpus_consistency.cpp`, rows of
`rocketnet_functional_tests[]`: `test_pcapng_corpus_replay`,
`test_pcapng_corpus_witness_unread_packet_goes_red`,
`test_pcapng_corpus_covers_control_packet_types` and
`test_fuzz_prefilter_seed_names_match_outcomes`. Run one with
`./bin/functional --run <name>`.

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
`tests/functional/rocketnet_functional_tests.cpp` and run by `./bin/functional`,
not `./bin/test`.

Functional cases run in parallel, one job per core (`ROCKETNET_TEST_JOBS` to
change it); a case that cannot share the host is fixed or moved to the
nightly tier (`ROCKETNET_TEST_NIGHTLY=1`).
Use generous completion deadlines to catch hangs; do not assert wall-clock
latency or receive-gap thresholds. Unit tests use deterministic inputs and
simulated time for protocol deadlines, never host scheduling speed. See
[BUILDING.md](../docs/BUILDING.md) for the 60-second target and 120-second cap.

Unit cases do not read or change the process environment. Every SDK reader
takes an environment source as an argument (`sdk/rocketnet_env.h`), the way it
takes time: the public entry points pass the process environment, and a unit
case that tests environment parsing, validation or precedence declares the
in-memory table in `test_env_source.h`, which it owns, and passes it to the
reader or to a `_with_env` create. The runner (`tests/test.cpp`), not a case,
clears every name the SDK reads before each case, so a public create in a
unit case reads none of them; `tools/check_test_environment.py` runs the suite
with hostile values set, transport pins included, and
`tools/check_unit_purity.py` refuses any process-environment read or change in
the unit sources, their `tests/*.h` helpers and the DEV fixtures they call.
The functional environment-wiring cases change and restore real process
variables and observe each SDK reader.

The process thread-count cases `test_server_stats_creates_no_thread` and
`test_public_step_driven_client_connects_without_a_thread` read the process's
OS thread count from `/proc/self/task`, so they are functional cases in
`./bin/functional`, not unit cases. Their bodies are in
`tests/functional/process_thread_counts.cpp`. On platforms without that file the
count is -1 and the thread assertion pins nothing, so it is never unit logic.

## Peak memory per case

Both runners record each case process's peak resident set (`wait4`
`ru_maxrss` on POSIX, normalised from kilobytes on Linux and bytes on macOS;
`PeakWorkingSetSize` on Windows). With `ROCKETNET_TEST_REPORT=1` both
runners print, after the results, a `slowest cases:` block and a `peak rss:`
block of the ten largest (`suite_print_report`). The unit suite reports an
over-bound case as `<case> peak_rss_mb=<n> bound_mb=<b>` without changing its
verdict. The functional suite enforces the bound and fails with the same values.

`test_unit_case_memory` is a functional sibling row that runs the complete unit
binary in enforcement mode. The unit binary therefore supplies the case list,
the suite-wide fallback, every case-specific declaration and every measured
peak directly; there is no second bound table. The outer functional row keeps
the 120-second limit but does not compare the unit controller's aggregate child
usage with one case's bound. Its inner runner retains the 120-second per-case
Release limit, including the existing Debug and sanitizer scaling, and enforces
each unit case separately.

The suite-wide bound is
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
cannot silently disable functional enforcement.

A case whose plan is mostly never touched (a thousand connected clients
reserve tens of GiB the case does not fill) declares its bound from the pages
it touches instead: a byte allowance for what it builds plus a count of pages
scaled by `suite_page_bytes()`, since a sparse touch holds a whole page
resident and a page is 16 KiB on Apple silicon and 4 KiB on x86_64
(`no_worker_rss_bound_mb` in `test_unit_server_stats_no_worker.cpp`). The
bound is a measured scale, not a loosened constant: it grows with the page
and nothing else.

`./bin/test --run <case>` runs one case without the bound, and a plain
`./bin/test` run only reports it; the functional `test_unit_case_memory` row
(`./bin/test --enforce-rss`) is the run that checks each case's peak against it.
