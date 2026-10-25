# macOS transport cells, fifty times in a row

The record of one run of `scripts/transport-cells-repeat --runs 50` on the
fleet's Apple-silicon Mac, the run the nightly job
`macos-transport-cells-repeat` (`.github/workflows/nightly.yml`) makes every
night on the `rocketnet-macos` runner. The cells and the cases are read from
ci.yml's `TRANSPORT_MATRIX_MACOS` step: in each cell, IPv4
(`test_loopback_transport_udp`), IPv6 (`test_loopback_transport_ipv6`), an
IPv4 client of a dual-stack server (`test_transport_dual_stack_ipv4_client`)
and the environment pin (`test_transport_env_pin_is_honoured`).

- Date (`date -u`): Sat Oct 24 04:23:50 UTC 2026
- Head: 13b7b291 (main at 8eb03874 plus the nightly job)
- Machine: `hostname` <macOS bench>; `uname -r` 27.2.0; macOS 27.2 (26B118), Apple silicon
- NIC: none; every cell runs over the loopback interface, so no `ethtool` line applies

## Commands

```
git log --oneline -1
13b7b291 nightly macOS job runs every transport cell fifty times
make -j8
[100%] Built target test
scripts/transport-cells-repeat --runs 50
./bin/test
```

## Result

```
CELL portable/portable test_loopback_transport_udp runs=50 pass=50 fail=0
CELL portable/portable test_loopback_transport_ipv6 runs=50 pass=50 fail=0
CELL portable/portable test_transport_dual_stack_ipv4_client runs=50 pass=50 fail=0
CELL portable/portable test_transport_env_pin_is_honoured runs=50 pass=50 fail=0
CELL portable/msgx test_loopback_transport_udp runs=50 pass=50 fail=0
CELL portable/msgx test_loopback_transport_ipv6 runs=50 pass=50 fail=0
CELL portable/msgx test_transport_dual_stack_ipv4_client runs=50 pass=50 fail=0
CELL portable/msgx test_transport_env_pin_is_honoured runs=50 pass=50 fail=0
CELL msgx/portable test_loopback_transport_udp runs=50 pass=50 fail=0
CELL msgx/portable test_loopback_transport_ipv6 runs=50 pass=50 fail=0
CELL msgx/portable test_transport_dual_stack_ipv4_client runs=50 pass=50 fail=0
CELL msgx/portable test_transport_env_pin_is_honoured runs=50 pass=50 fail=0
CELL msgx/msgx test_loopback_transport_udp runs=50 pass=50 fail=0
CELL msgx/msgx test_loopback_transport_ipv6 runs=50 pass=50 fail=0
CELL msgx/msgx test_transport_dual_stack_ipv4_client runs=50 pass=50 fail=0
CELL msgx/msgx test_transport_env_pin_is_honoured runs=50 pass=50 fail=0
CELL loopback/loopback test_loopback_transport_udp runs=50 pass=50 fail=0
CELL loopback/loopback test_loopback_transport_ipv6 runs=50 pass=50 fail=0
CELL loopback/loopback test_transport_dual_stack_ipv4_client runs=50 pass=50 fail=0
CELL loopback/loopback test_transport_env_pin_is_honoured runs=50 pass=50 fail=0
TRANSPORT-CELLS-REPEAT os=macos runs=50 asan=0 logs=<job dir>/rocketnet/build/transport-cells-repeat/run.RkFEZL result=pass

real	0m41.755s
user	0m8.562s
sys	0m16.897s
EXIT=0
```

Every one of the 20 CELL lines is pass=50 fail=0 (1000 runs, no failure),
including `test_transport_dual_stack_ipv4_client` on the portable client
(portable/portable and msgx/portable), the case that failed once,
intermittently, in a sharded functional run before. `./bin/test` on the
same build: exit 0, `*** ALL TESTS PASSED ***`.

## Verdict

Pass: every macOS transport cell fifty times in a row, zero flakes, at
13b7b291 on the macOS bench. The nightly job has not yet run on the runner; its
first night is not made yet.
