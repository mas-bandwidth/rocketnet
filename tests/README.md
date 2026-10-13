# tests/

The test sources. Nothing here ships in the sdk; the sdk directory remains the
complete drop-in set. The targets are defined in the top-level
`CMakeLists.txt` and the binaries land in `bin/` beside the libraries, so every
run command below is from the repository root.

- `test.cpp`, `test_channels.cpp` — the unit suite, `bin/test` (its test
  bodies are in `sdk/rocketnet_tests.cpp`).
- `test_lifecycle.cpp`, `test_driver.cpp`, `test_transport_control.cpp`,
  `test_serialize.cpp`, `test_sodium.c`, and per platform `test_xdp.cpp`
  (Linux), `test_msgx.cpp` (macOS), `test_iocp.cpp` (Windows) — one binary
  each, `bin/<name>`; the functional suite runs the socket ones as its own
  tests.
- `soak.c`, `soak_sim.cpp` — the soaks, `bin/soak` and `bin/soak_sim`.
- `driver_extension/` — the external driver example, built and run by
  `tools/check_driver_extension.py`.
- `serialize.h` — mas-bandwidth/serialize 1.15, vendored so the serialize
  read of rocketnet.h (the C++ stream surface) is compiled and exercised by
  `bin/test_serialize`. Refresh it when the serialize pin moves.
- `shared/` — test code used by more than one test:
  - `suite_runner.h` — the process-per-test runner behind `bin/test` and
    `bin/functional`.
  - `route_checks.h` — the route-matching checks, shared by `bin/test_driver`
    and the big-endian `tools/bigendian/wire_check.cpp`.

## Running the suite

`./bin/test` prints test names and nothing else, because the suite captures
the library's log into a ring and dumps it only when a check fails — set
`ROCKETNET_TEST_VERBOSE=1` to skip the capture and watch the log live.
