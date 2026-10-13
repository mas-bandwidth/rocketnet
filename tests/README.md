# test/

Test-only vendored dependencies. Nothing here ships in the sdk; the sdk
directory remains the complete drop-in set.

- `serialize.h` — mas-bandwidth/serialize 1.15, vendored so the serialize
  read of rocketnet.h (the C++ stream surface) is compiled and exercised by
  `bin/test_serialize`. Refresh it when the serialize pin moves.

## Running the suite

`./bin/test` prints test names and nothing else, because the suite captures
the library's log into a ring and dumps it only when a check fails — set
`ROCKETNET_TEST_VERBOSE=1` to skip the capture and watch the log live.
