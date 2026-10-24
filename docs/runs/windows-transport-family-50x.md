# Windows transport cells, fifty times in a row

The record of one run of `scripts/transport-cells-repeat --runs 50` for the
portable-server and loopback cells on the fleet's native Windows machine, the
run the nightly job `windows-transport-cells-repeat`
(`.github/workflows/nightly.yml`) makes every night on the `rocketnet-windows`
runner. The cells and the cases are read from ci.yml's
`TRANSPORT_MATRIX_WINDOWS` step: in each cell, IPv4
(`test_loopback_transport_udp`), IPv6 (`test_loopback_transport_ipv6`), an
IPv4 client of a dual-stack server (`test_transport_dual_stack_ipv4_client`)
and the environment pin (`test_transport_env_pin_is_honoured`). The iocp and
rio server cells are rn-win-transport-family-50x-native's.

- Date (`date -u`): Sat Oct 24 12:48:59 UTC 2026 (queued for the exclusive lock; the run itself took 72 s)
- Head: e3290451 (main at 1a2f2c43 plus the nightly job)
- Machine: `hostname` captainamerica (captain); Windows 10.0.26200.9711, x64; MSVC (Visual Studio 18 Community), Release, ROCKETNET_DEV=ON (the default)
- Shell: Git for Windows bash (`uname -s` MINGW64_NT-10.0-26200)
- Lock: `winlock --exclusive --card rn-win-transport-family-50x` (ticket 1469); the build ran beforehand under a shared slot
- NIC: none; every cell runs over the loopback interface

## Commands

```
cmake -B build -A x64 -DROCKETNET_DEV=ON
cmake --build build --config Release --parallel 16
scripts/transport-cells-repeat --runs 50 --cell portable/portable --cell loopback/loopback
```

## Result

```
CELL portable/portable test_loopback_transport_udp runs=50 pass=50 fail=0
CELL portable/portable test_loopback_transport_ipv6 runs=50 pass=50 fail=0
CELL portable/portable test_transport_dual_stack_ipv4_client runs=50 pass=50 fail=0
CELL portable/portable test_transport_env_pin_is_honoured runs=50 pass=50 fail=0
CELL loopback/loopback test_loopback_transport_udp runs=50 pass=50 fail=0
CELL loopback/loopback test_loopback_transport_ipv6 runs=50 pass=50 fail=0
CELL loopback/loopback test_transport_dual_stack_ipv4_client runs=50 pass=50 fail=0
CELL loopback/loopback test_transport_env_pin_is_honoured runs=50 pass=50 fail=0
TRANSPORT-CELLS-REPEAT os=windows runs=50 asan=0 logs=/c/rowan-jobs/rn-backlog/rn-win-transport-family-50x.w2/build/transport-cells-repeat/run.FyTUqY result=pass
rc=0 elapsed=72s head=e3290451dcfbb0d9900b28ef1859510703c3ca71
```

Every one of the 8 CELL lines is pass=50 fail=0 (400 runs, no failure).

## Noted, not this card's

The same build's unit-test target (`test.exe`) did not compile at this head:
MSVC C4324 (structure padded due to alignment specifier) as an error at
`tests/test_unit_public_link_simulator.cpp(313)`. `functional.exe`, the only
binary this run and the nightly job use, built clean; the nightly job builds
the `functional` target alone.
