# tests/

Test-only vendored dependencies. Nothing here ships in the sdk; the sdk
directory remains the complete drop-in set.

- `serialize.h` — mas-bandwidth/serialize 1.15, vendored so the serialize
  read of rocketnet.h (the C++ stream surface) is compiled and exercised by
  `bin/test_serialize`. Refresh it when the serialize pin moves.
- `wycheproof/` — C2SP/wycheproof known-answer vectors for the primitives
  rocketnet calls (ChaCha20-Poly1305, X25519, Ed25519), parsed by
  tests/wycheproof.h. See tests/wycheproof/README.md for the pin and hashes.

## Fixtures and where each case runs

A unit case reads no file and writes none. The fixed known-answer files the
unit cases check against, `connect_token_vectors.json` and the three
`wycheproof/*_test.json` files, are compiled into `./bin/test` at build time by
`cmake/EmbedTestFixtures.cmake` (portable CMake, `file(READ ... HEX)`) and read
from memory through `rocketnet_test_fixture`; editing one rebuilds the unit
binary. Their cases (`test_vectors_regenerate_byte_identical`,
`test_each_vector_verifies_and_opens`, `test_tampered_vector_refused` and the
`test_wycheproof_*` cases) are unit cases.

`connect_token_vectors.json` is written only by the tool
`./bin/connect_token_vectors <path>` (`tools/connect_token_vectors/main.cpp`),
which mints it from the same generator the unit case checks and writes only
the path it is given, through a temporary file renamed over it:

```
./bin/connect_token_vectors tests/connect_token_vectors.json
```

then rebuild, and `test_vectors_regenerate_byte_identical` checks the new copy.

The corpus-consistency cases walk directories that grow as findings land
(`pcapng/` with `pcapng/regress/`, and `fuzz/corpus/`), so they are functional
cases in `functional/corpus_consistency.cpp`, rows of
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

Functional cases run one at a time, even with `ROCKETNET_TEST_JOBS` set.
Use generous completion deadlines to catch hangs; do not assert wall-clock
latency or receive-gap thresholds. Unit tests use deterministic inputs and
simulated time for protocol deadlines, never host scheduling speed. See
[BUILDING.md](../BUILDING.md) for the 60-second target and 120-second cap.

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
