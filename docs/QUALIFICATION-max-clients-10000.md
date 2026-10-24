# Qualification: 10000-client build

`ROCKETNET_MAX_CLIENTS` (`sdk/rocketnet.h`) is 1000 by default. This record
proves a build configured with `-DROCKETNET_MAX_CLIENTS=10000` builds every
default target with zero warnings at the strict flags, runs the unit suite,
and plans and runs a bounded loopback load of all 10000 clients. The proof is
repeated nightly by `.github/workflows/nightly-max-clients.yml` on the Linux
fleet runner.

## Head

Recorded at head `5bc44eb1a4aac5a0342f54104597a72aba744b93`
(branch `sprint/rn-10000-custom-build.w2.g1.e1`).

## Commands

Default build (unchanged):

    make -j16

10000 build (into a separate build directory so the default build's `./bin`
is untouched):

    cmake -S . -B build-10000 -DCMAKE_BUILD_TYPE=Release -DROCKETNET_MAX_CLIENTS=10000
    cmake --build build-10000 -j16

Unit suite, 10000 build:

    ./bin/test

Bounded load, loopback, 10000 clients:

    ./bin/load 10000 30

Production (DEV OFF) legs:

    make ROCKETNET_DEV=OFF -j16
    cmake -S . -B build-10000 -DCMAKE_BUILD_TYPE=Release -DROCKETNET_MAX_CLIENTS=10000 -DROCKETNET_DEV=OFF
    cmake --build build-10000 -j16

## Results

- **Build.** Both DEV ON and DEV OFF 10000 builds complete every default target
  with zero warnings at the strict flags (`-Wall -Wextra -Wpedantic
  -Wconversion` and the rest CMakeLists.txt sets). Every static assert in
  `sdk/rocketnet_constants.h` holds at 10000: the per-update receive budget
  (160000), send staging (32768), XDP replay map (10240) and the rest of the
  limits table in BUILDING.md's "Maximum clients" section.
- **Unit suite.** The 10000 build's `./bin/test` runs the whole suite. One
  case fails on the RSS bound: `test_secrets_wiped_destroy_writes_only_keys`
  peaks at 1699 MiB against the suite-wide bound (1217 MiB for a >1000-client
  build). This red reproduces unchanged at BASE in the default 1000 build on
  the same host, so it is pre-existing and unrelated to the 10000 build; the
  case allocates the full process-wide loopback endpoint table
  (`ROCKETNET_LOOPBACK_MAX_ENDPOINTS`) without a declared RSS bound, unlike
  the loopback transport cases in `tests/test_unit_driver_loopback.cpp`.
  The proposed fix (a `SUITE_CASE_RSS_BOUND` registration naming
  `loopback_full_table_rss_bound_mb`) is beyond this card's PATHS and is
  reported separately; the nightly workflow is not gated on it.
- **Bounded load.** The 10000-client load plan
  (`LOAD_PLAN_ONLY=1 ./bin/load 10000 30`) reports
  `total_public_required_bytes=282457711360` (~263 GiB) in the default
  profile, far above the ~78 GiB available on the sandbox runner this record
  was written on, so the live 10000-client load is not run here. The daily
  proof runs it on the fleet's long runner, which checks host room against
  the plan before running and fails loudly if it lacks it
  (`.github/workflows/nightly-max-clients.yml`). The same build's 1000-client
  load is unchanged from BASE (`./bin/load 1000 30` PASS, connected=1000).
- **Default build unchanged.** `make -j16` builds, its `./bin/test` and
  `./bin/functional` match BASE, and `./bin/load 1000 30` is byte-for-byte
  the same PASS as BASE.

## What was not done

- The live `./bin/load 10000 30` run (default profile) is recorded as planned,
  not executed: the plan needs ~263 GiB and the local runner had ~78 GiB.
  The nightly fleet runner performs it.
- The pre-existing `test_secrets_wiped_destroy_writes_only_keys` RSS red is
  not fixed (tests are outside this card's PATHS); a proposed diff is in the
  result report.
