# test/

Test-only vendored dependencies. Nothing here ships in the sdk; the sdk
directory remains the complete drop-in set.

- `serialize.h` — mas-bandwidth/serialize 1.15, vendored so `bin/test_serialize`
  can bind `serialize::WriteStream` and `serialize::ReadStream` directly over
  packet buffers of the transport. Refresh it when the serialize pin moves.
- `wycheproof/` — C2SP/wycheproof known-answer vectors for the primitives
  rocketnet calls (ChaCha20-Poly1305, X25519, Ed25519), read by
  test/wycheproof.h. See test/wycheproof/README.md for the pin and hashes.

## Running the suite

`./bin/test` prints test names and nothing else, because the suite captures
the library's log into a ring and dumps it only when a check fails — set
`ROCKETNET_TEST_VERBOSE=1` to skip the capture and watch the log live.

## Peak memory per case

Both runners record each case process's peak resident set (`wait4`
`ru_maxrss` on POSIX, normalised from kilobytes on Linux and bytes on macOS;
`PeakWorkingSetSize` on Windows) and print it after the timing summary, largest
first. `./bin/functional` prints one line a case:

    <case> peak_rss_mb=<n> bound_mb=<b>

`./bin/test` prints its ten largest as `test_timing_rss_NN [SKIPPED <case> peak_rss_mb=<n> bound_mb=<b>]`, the names-only spelling its checker accepts.
A case whose peak is over the bound fails the suite with the same
`<case> peak_rss_mb=<n> bound_mb=<b>` text on its `FAIL:` line. The bound is
`ROCKETNET_TEST_MAX_CASE_RSS_MB`, 698 MiB in `test/suite_runner.h` (the larger
of 256 MiB and 1.5 times the largest measured case, never over 1 GiB); the
environment variable of the same name overrides it with a positive value. A sanitizer build
prints the peaks and enforces no bound unless that variable sets one, because
sanitizer shadow memory is not the case's own footprint.
