# Linux transport cells, fifty times in a row, and the functional suite under ASan

The record of one run of `scripts/transport-cells-repeat --runs 50` and one
run of `scripts/transport-cells-repeat --asan` on a Linux bench, for the
final-deliverable row "Linux: every transport cell fifty times in a row with
zero flakes, and the functional suite under ASan per transport". The cells
and the cases are read from ci.yml's `TRANSPORT_MATRIX_LINUX` step: in each
socket cell, IPv4 (`test_loopback_transport_udp`), IPv6
(`test_loopback_transport_ipv6`), an IPv4 client of a dual-stack server
(`test_transport_dual_stack_ipv4_client`) and the environment pin
(`test_transport_env_pin_is_honoured`).

- Date (`date -u`): Sat Oct 24 03:54:01 PM UTC 2026 (cells), the ASan pass ending 04:51:12 PM UTC
- Head: eedaa2cc (main, `land rn-reproducible-sources-all-platforms`); every run below is at this head
- Machine: `hostname` b1 (the fleet's amd-1); `uname -r` 7.0.0-38-generic; 8 processors; `/proc/sys/kernel/io_uring_disabled` 0
- NIC: none; every socket cell runs over the loopback interface, so no `ethtool` line applies
- Load: the bench is shared. The 50-run cells ran at load 6.7 to 10.6; the
  ASan suites ran while other cards' builds held the load at 35 to 40 on 8
  processors (37 at the end). The ASan results below are taken at that load.

## Commands

```
git log --oneline -1
eedaa2cc land rn-reproducible-sources-all-platforms (sprint stream rocketnet-final)
make -j4
[100%] Built target test
scripts/transport-cells-repeat --runs 50 --cell portable/portable --cell gso/portable --cell gso/gso
scripts/transport-cells-repeat --runs 50 --cell portable/gso --cell io_uring/portable --cell loopback/loopback
scripts/transport-cells-repeat --asan --runs 1 --jobs 4 --cell portable/portable --cell gso/portable --cell gso/gso --cell portable/gso --cell io_uring/portable --cell loopback/loopback
```

The six socket cells ran as two invocations of three cells each, so the
8-processor bench was not driven past its processor count by this run.
Together they are every socket cell `--list` names.

## The xdp cell

`xdp/portable` (`test_xdp_transport_max_size_both_ways_ipv4`,
`test_xdp_transport_max_size_both_ways_ipv6`): not made. This card was cut
to the Linux socket cells; the xdp cell belongs on the dedicated XDP machine
in a virtme-ng guest and is owed to its own card.

## Result: fifty runs per cell

```
CELL portable/portable test_loopback_transport_udp runs=50 pass=50 fail=0
CELL portable/portable test_loopback_transport_ipv6 runs=50 pass=50 fail=0
CELL portable/portable test_transport_dual_stack_ipv4_client runs=50 pass=50 fail=0
CELL portable/portable test_transport_env_pin_is_honoured runs=50 pass=50 fail=0
CELL gso/portable test_loopback_transport_udp runs=50 pass=50 fail=0
CELL gso/portable test_loopback_transport_ipv6 runs=50 pass=50 fail=0
CELL gso/portable test_transport_dual_stack_ipv4_client runs=50 pass=50 fail=0
CELL gso/portable test_transport_env_pin_is_honoured runs=50 pass=50 fail=0
CELL gso/gso test_loopback_transport_udp runs=50 pass=50 fail=0
CELL gso/gso test_loopback_transport_ipv6 runs=50 pass=50 fail=0
CELL gso/gso test_transport_dual_stack_ipv4_client runs=50 pass=50 fail=0
CELL gso/gso test_transport_env_pin_is_honoured runs=50 pass=50 fail=0
TRANSPORT-CELLS-REPEAT os=linux runs=50 asan=0 logs=.../run.W4NpYZ result=pass
CELL portable/gso test_loopback_transport_udp runs=50 pass=50 fail=0
CELL portable/gso test_loopback_transport_ipv6 runs=50 pass=50 fail=0
CELL portable/gso test_transport_dual_stack_ipv4_client runs=50 pass=50 fail=0
CELL portable/gso test_transport_env_pin_is_honoured runs=50 pass=50 fail=0
CELL io_uring/portable test_loopback_transport_udp runs=50 pass=50 fail=0
CELL io_uring/portable test_loopback_transport_ipv6 runs=50 pass=50 fail=0
CELL io_uring/portable test_transport_dual_stack_ipv4_client runs=50 pass=50 fail=0
CELL io_uring/portable test_transport_env_pin_is_honoured runs=50 pass=50 fail=0
CELL loopback/loopback test_loopback_transport_udp runs=50 pass=50 fail=0
CELL loopback/loopback test_loopback_transport_ipv6 runs=50 pass=50 fail=0
CELL loopback/loopback test_transport_dual_stack_ipv4_client runs=50 pass=50 fail=0
CELL loopback/loopback test_transport_env_pin_is_honoured runs=50 pass=50 fail=0
TRANSPORT-CELLS-REPEAT os=linux runs=50 asan=0 logs=.../run.B4Q7Cg result=pass
```

Every one of the 24 CELL lines is pass=50 fail=0 (1200 runs, no failure).

## Result: ASan (Release, `ROCKETNET_SANITIZE_ADDRESS=ON`)

Every socket cell's cases once on the ASan build: all 24 CELL lines
runs=1 pass=1 fail=0. The whole functional suite once per cell:

```
SUITE portable/portable runs=1 pass=1 fail=0
SUITE gso/portable runs=1 pass=1 fail=0
SUITE gso/gso runs=1 pass=1 fail=0
SUITE portable/gso runs=1 pass=1 fail=0
SUITE io_uring/portable runs=1 pass=0 fail=1 exit=1
SUITE loopback/loopback runs=1 pass=0 fail=1 exit=1
TRANSPORT-CELLS-REPEAT os=linux runs=1 asan=1 logs=.../run.5lHzVA result=fail
```

No AddressSanitizer or LeakSanitizer report in any log. The two failures,
each recorded once and not retried:

- `io_uring/portable`, case `test_lifecycle_transport` (the 136th case of
  the suite; every case before it passed), first log
  `io_uring.portable.suite.log`:

  ```
  test_lifecycle_transport
  check failed: test_lifecycle_transport exceeded 120s
  ```

- `loopback/loopback`, case
  `test_transport_acked_unfragmented_was_in_exactly_one_batch` (the 35th
  case; every case before it passed), first log
  `loopback.loopback.suite.log`:

  ```
  test_transport_acked_unfragmented_was_in_exactly_one_batch
  refusals moved on a run that should be clean:
    SEND_RING_FULL = 187
  check failed: ( rocketnet_counters_refusals_moved == 0 ), function rocketnet_test_transport_acked_unfragmented_was_in_exactly_one_batch, file functional/rocketnet_functional_tests.cpp, line 6884
  ```

A failed check ends the suite, so the cases after each failure did not run
under that cell's transports under ASan. Both failures are at main
(eedaa2cc), so they are at BASE, and both were taken at load 35 to 40 on an
8-processor bench; each is a defect for its own card (a functional case
that is not clean under load), never a reason to loosen its bound.

## Verdict

Hold. Every Linux socket cell fifty times in a row: pass, zero flakes
(1200 runs). The functional suite under ASan: green under portable/portable,
gso/portable, gso/gso and portable/gso; red under io_uring/portable
(`test_lifecycle_transport` exceeded 120s) and loopback/loopback
(`test_transport_acked_unfragmented_was_in_exactly_one_batch`,
SEND_RING_FULL = 187). The xdp cell is not made.
