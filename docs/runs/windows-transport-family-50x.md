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
rio server cells are recorded separately.

- Date (`date -u`): Sat Oct 24 20:10:52 UTC 2026 (exclusive lock taken); the run ended 20:12:08 UTC (76 s)
- Code head H: dbd4ff5fcf1aff826a3431ffac9abde1ae59c8d0 (main 31d4fbeb merged, plus the nightly job). The commit that adds this record is the only change after H.
- Machine: the fleet's native Windows 11 machine, x64 (`uname -s` under Git for Windows bash: MINGW64_NT-10.0-26200); MSVC (Visual Studio 18 Community), Release, ROCKETNET_DEV=ON (the default)
- Shell: Git for Windows bash
- Lock: the machine's exclusive lock (`winlock --exclusive`) for the run; the build ran beforehand in a shared slot; 0 warnings
- NIC: none; every cell runs over the loopback interface

## Commands

```
git clone https://github.com/mas-bandwidth/rocketnet.git && git checkout dbd4ff5fcf1aff826a3431ffac9abde1ae59c8d0
cmake -B build -A x64
cmake --build build --config Release --parallel 4
scripts/transport-cells-repeat --runs 50 --cell portable/portable --cell loopback/loopback
```

## Result

```
MINGW64_NT-10.0-26200
dbd4ff5fcf1aff826a3431ffac9abde1ae59c8d0
CELL portable/portable test_loopback_transport_udp runs=50 pass=50 fail=0
CELL portable/portable test_loopback_transport_ipv6 runs=50 pass=50 fail=0
CELL portable/portable test_transport_dual_stack_ipv4_client runs=50 pass=50 fail=0
CELL portable/portable test_transport_env_pin_is_honoured runs=50 pass=50 fail=0
CELL loopback/loopback test_loopback_transport_udp runs=50 pass=50 fail=0
CELL loopback/loopback test_loopback_transport_ipv6 runs=50 pass=50 fail=0
CELL loopback/loopback test_transport_dual_stack_ipv4_client runs=50 pass=50 fail=0
CELL loopback/loopback test_transport_env_pin_is_honoured runs=50 pass=50 fail=0
TRANSPORT-CELLS-REPEAT os=windows runs=50 asan=0 logs=<checkout>/build/transport-cells-repeat/run.kKk2S2 result=pass
cells-rc=0 elapsed=76s
```

(The logs path is shortened to `<checkout>`; nothing else in the output is
changed.)

Verdict: pass. Every one of the 8 CELL lines is pass=50 fail=0 (400 runs, no
failure) at H.
