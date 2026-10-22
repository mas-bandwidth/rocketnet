# test/

Test-only vendored dependencies. Nothing here ships in the sdk; the sdk
directory remains the complete drop-in set.

- `serialize.h` — mas-bandwidth/serialize 1.15, vendored so the serialize
  read of rocketnet.h (the C++ stream surface) is compiled and exercised by
  `bin/test_serialize`. Refresh it when the serialize pin moves.
- `wycheproof/` — C2SP/wycheproof known-answer vectors for the primitives
  rocketnet calls (ChaCha20-Poly1305, X25519, Ed25519), read by
  test/wycheproof.h. See test/wycheproof/README.md for the pin and hashes.

## Running the suite

`./bin/test` and `./bin/functional` print one line per test, its name, in
suite order, then `*** ALL TESTS PASSED ***`, as netcode's tests do. Every
name is `test_` then lowercase snake_case, and the suite fails before it runs
anything when a registered name is not (`suite_name_is_test_snake_case` in
`test/suite_runner.h`). A failing test prints everything it wrote, including
its `check failed:` line, and the suite exits 1 at once. Each case's output is
captured and dropped when it passes; set `ROCKETNET_TEST_VERBOSE=1` to skip
the capture and watch the log live. A test that a platform does not build is
not registered there, so it is neither run nor printed.

`./bin/functional` has a nightly tier: the cases named in
`rocketnet_functional_nightly_cases` (`functional/rocketnet_functional_tests.cpp`)
run only with `ROCKETNET_TEST_NIGHTLY=1`, which the nightly workflow sets.
They are cases whose length is their design, such as a real token timeout
waited out or a thousand real-socket cycles, and each area keeps one
representative in the per-commit suite. `--run <case>` runs any case, nightly
or not.

## Peak memory per case

Every case process is held to its plan (`test/plan_rss.h`). The SDK
allocates every byte through the allocator `rocketnet_init` is given, and
it allocates exactly its memory plan (`tools/check_memory.py` checks the
plans against allocator observations). Each suite process passes
`plan_rss_malloc` and `plan_rss_free`, which count the bytes the SDK holds
and keep the peak. A case that passes an allocator of its own chains it
under the counter: `rocketnet_init( context, plan_rss_chain( my_malloc,
my_free ), plan_rss_free )` (the counter passes the case's allocator the
exact sizes the SDK asks for). When the case ends, the process's peak
resident set must be at most the plan's peak plus `PLAN_RSS_SLACK_MB`, a
fixed allowance for what no plan counts (the executable, the runtimes,
stacks and the test's own bookkeeping). Otherwise the case fails with
`check failed: <case> peak_rss_mb=<n> bound_mb=<b> (plan peak <bytes> + <slack> MiB slack)`.
Memory the SDK does not account for, or a leak outside the SDK, fails
whatever size of server the case legitimately builds. Blocks of 1 MiB or
more are mapped from the OS and unmapped when freed. Otherwise the C
allocator's cache of freed large blocks would make a process that builds a
300-client server and then a 1000-client one peak at the sum of both.

Both runners record each case's peak resident set (`wait4` `ru_maxrss` on
POSIX, normalised from kilobytes on Linux and bytes on macOS;
`PeakWorkingSetSize` on Windows). With `ROCKETNET_TEST_REPORT=1` both
runners print, after the results, a `slowest cases:` block and a `peak rss:`
block of the ten largest (`suite_print_report`). `ROCKETNET_TEST_MAX_CASE_RSS_MB`,
when set, replaces the plan bound with one bound the runner applies to every
case. A sanitizer build checks no bound unless that variable sets one,
because sanitizer shadow memory is not the case's own footprint.

A case can also declare a bound of its own beside its registration, which
the runner enforces as well:

```cpp
static long my_case_rss_bound_mb()
{
    return suite_rss_bound_mb( planned_server_bytes, 64 );
}
SUITE_CASE_RSS_BOUND( my_case, my_case_rss_bound_mb );
```

The function computes its bound from the SDK memory plan for the exact config
the case uses, then adds a stated process margin with `suite_rss_bound_mb`.
A positive `ROCKETNET_TEST_MAX_CASE_RSS_MB` replaces it, as it replaces the
plan bound. A declared plan of zero declares no bound; the case's own plan
bound still applies.
