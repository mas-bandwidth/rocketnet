# Preview-3 qualification record

Preview-3 is qualified by recorded manual runs, as the preview-2 recut was:
native unit and functional suites on Linux, macOS and Windows at the candidate
head, with exact commands, plus the LAN proof and soak records already in this
repository. Every value below is copied from a run log or from the record it
links; a result carried from another commit names that commit and the
difference of its inputs. Manual results are not GitHub CI runs. The gates
still owed when the tag is cut are listed under "Not qualified".

## Candidate head

- Suite head: `126ef062238c8f82a14a0a3a7ed97ba818cb2269`, the whole-tree
  format landing on `main`. The Linux, macOS and Windows suites below ran on a
  fresh tree at this commit.
- Candidate head: `13b82655`, this branch after merging `main` at
  `13b84d83e8d9dbed1f59e2b7005956a86a964bb5`. Its inputs differ from the
  suite head by four layout-only landings on `main` (`9e1be858`, `be297ec4`,
  `48041380`, `13b84d83`), each landed on identical object disassembly, the
  shipping version label in `sdk/rocketnet.h` (`"v1.0.0-preview-2"` to
  `"v1.0.0-preview-3"`, a string compiled only with `-DROCKETNET_DEV=OFF`),
  and documentation. The Linux and macOS suites were rerun at the candidate
  head and are recorded below; the Windows suites were not.
- Layout passes still landing on `main` after the candidate head change only
  whitespace and are proven by identical disassembly. The tag commit is
  reconciled against this record before the tag is cut
  ([RELEASING.md](RELEASING.md), "Cutting the release").

## Native suites

Release builds. The development build is the default (`ROCKETNET_DEV=ON`);
the production build (`ROCKETNET_DEV=OFF`) is built separately for the
zero-warning check. Test counts are the cases each suite printed; no case
failed and no case was rerun.

### Suite head `126ef062`

| Platform | Step | Command | Result | Last line | Seconds |
| --- | --- | --- | --- | --- | ---: |
| Linux x64, gcc 15.2.0 (Ubuntu 26.04.1) | build | `make -j4` | exit 0, 0 warnings | `[100%] Built target functional` | 86 |
| | unit | `./bin/test` | exit 0, 1294 cases | `*** ALL TESTS PASSED ***` | 22 |
| | functional | `./bin/functional` | exit 0, 1515 cases | `*** ALL TESTS PASSED ***` | 299 |
| | production build | `make clean && make ROCKETNET_DEV=OFF -j4` | exit 0, 0 warnings | `[100%] Built target functional` | 67 |
| | docs | `make docs && make docs-undocumented` | exit 0, 0 warnings | `Generated html documentation in 613ms` (227 pages) | 2 |
| macOS 27.2 arm64, Apple clang 21.0.0 | build | `make -j4` | exit 0, 0 warnings | `[100%] Built target functional` | 23 |
| | unit | `./bin/test` | exit 0, 1199 cases | `*** ALL TESTS PASSED ***` | 18 |
| | functional | `./bin/functional` | exit 0, 1391 cases | `*** ALL TESTS PASSED ***` | 201 |
| | production build | `make clean && make ROCKETNET_DEV=OFF -j4` | exit 0, 0 warnings | `[100%] Built target functional` | 18 |
| Windows 11 x64, MSVC 19.51.36260 | build | `cmake -B build-ON -A x64 -DROCKETNET_DEV=ON` then `cmake --build build-ON --config Release --parallel 4` | exit 0, 0 MSVC warnings | `functional.vcxproj -> ...\bin\Release\functional.exe` | not recorded |
| | unit | `bin\Release\test.exe` | exit 0, 1169 cases | `*** ALL TESTS PASSED ***` | 42 |
| | functional | `bin\Release\functional.exe` | exit 0, 1394 cases | `*** ALL TESTS PASSED ***` | at most 540 (see below) |
| | production build | `cmake -B build-OFF -A x64 -DROCKETNET_DEV=OFF` then `cmake --build build-OFF --config Release --parallel 4` | exit 0, 0 MSVC warnings | `Building Custom Rule .../CMakeLists.txt` | 287 |

The Windows build and unit suite ran in a shared slot of the bench's lock
(at most three jobs); the functional suite ran under the bench's exclusive
lock with nothing else running, inside a 540-second timeout. The 705 seconds
measured around it include waiting for the exclusive lock, so the suite's own
time is recorded only as under the timeout.

### Candidate head `13b82655`

| Platform | Step | Command | Result | Last line | Seconds |
| --- | --- | --- | --- | --- | ---: |
| Linux x64, gcc 15.2.0 (Ubuntu 26.04.1) | build | `make -j4` | exit 0, 0 warnings | `[100%] Built target functional` | 87 |
| | unit | `./bin/test` | exit 0, 1294 cases | `*** ALL TESTS PASSED ***` | 22 |
| | functional | `./bin/functional` | exit 0, 1515 cases | `*** ALL TESTS PASSED ***` | 298 |
| | production build | `make clean && make ROCKETNET_DEV=OFF -j4` | exit 0, 0 warnings | `[100%] Built target functional` | 68 |
| macOS 27.2 arm64, Apple clang 21.0.0 | build | `make -j4` | exit 0, 0 warnings | `[100%] Built target functional` | 26 |
| | unit | `./bin/test` | exit 0, 1199 cases | `*** ALL TESTS PASSED ***` | 19 |
| | functional | `./bin/functional` | exit 0, 1391 cases | `*** ALL TESTS PASSED ***` | 204 |
| | production build | `make clean && make ROCKETNET_DEV=OFF -j4` | exit 0, 0 warnings | `[100%] Built target functional` | 18 |

## Recut at `81b6d3ef` (2026-10-28)

The first `v1.0.0-preview-3` tag (tag object `4ebaa060`, commit `389baeef`)
was replaced before delivery under the recut authorization in
[RELEASING.md](RELEASING.md#authorized-preview-3-recut). Two memory findings
in CI led to it, and neither is in the library:

- The ThreadSanitizer loopback soak growing without bound was the harness
  defect fixed by `f9bd9d3f`, an ancestor of `389baeef`. Forty soak runs at
  `389baeef` on hetzner2 passed with peak resident
  memory between 597,888 and 598,908 KB.
- The MemorySanitizer gate exhausted amd runners (clang 21). Six cases that
  build a full 1000-slot server had MemorySanitizer poison the server's whole
  reserved block, and on clang 18 the same cases had died at create while the
  gate still reported success. #600 (`81b6d3ef`) gives those cases an
  allocator for large blocks in MemorySanitizer builds only, runs
  memory-bounded cases one at a time under MemorySanitizer, and fails the gate
  on any fatal MemorySanitizer error. The gate passed at `7087e36a`, the
  pull request head whose tree `81b6d3ef` squashes, on amd-2 (clang 21.1.8,
  20 GB scope, peak 5.99 GB, 191 s) and hetzner2 (clang 18.1.3, 91 s).

`81b6d3ef` changes only `tests/` and `tools/` against `389baeef`; the library
sources are identical. The native suites ran again on a fresh clone at
`81b6d3ef3b98869c3e83d56c9bc8d4b50cb02b23` with the commands of the
`471b3fbc` table below, on the same benches and toolchains. Case, skip and
warning counts equal the `471b3fbc` row on every platform. The full Windows
functional suite, which includes
`test_rio_transport_windows_server_create_destroy_1000`, passed. The Ubuntu 22.04 LTS VM results are carried from `471b3fbc`:
the library is unchanged and the test change touches only MemorySanitizer
builds and the order of memory-bounded cases under it.

| Platform | Step | Command | Result | Last line | Seconds |
| --- | --- | --- | --- | --- | ---: |
| Linux x64, gcc 13.3.0 (Ubuntu 24.04.5), hetzner2 | build | `make -j4` | exit 0, 0 warnings | `[100%] Built target functional` | 53 |
| | unit | `./bin/test` | exit 0, 1299 cases | `*** ALL TESTS PASSED ***` | 22 |
| | functional | `./bin/functional` | exit 0, 1521 cases, 21 skipped | `*** ALL TESTS PASSED ***` | 265 |
| | production build | `make clean && make ROCKETNET_DEV=OFF -j4` | exit 0, 0 warnings | `[100%] Built target functional` | 35 |
| | docs | `make docs && make docs-undocumented` | exit 0, 0 warnings | `Generated html documentation in 338ms` (229 pages) | 1 |
| macOS 27.2 arm64, Apple clang 21.0.0 | build | `make -j4` | exit 0, 0 warnings | `[100%] Built target functional` | 23 |
| | unit | `./bin/test` | exit 0, 1204 cases | `*** ALL TESTS PASSED ***` | 19 |
| | functional | `./bin/functional` | exit 0, 1397 cases, 1 skipped | `*** ALL TESTS PASSED ***` | 207 |
| | production build | `make clean && make ROCKETNET_DEV=OFF -j4` | exit 0, 0 warnings | `[100%] Built target functional` | 21 |
| Windows 11 x64, MSVC 19.51.36260 | build | `cmake -B build-ON -A x64 -DROCKETNET_DEV=ON` then `cmake --build build-ON --config Release --parallel 4` | exit 0, 0 MSVC warnings | `Building Custom Rule .../CMakeLists.txt` | 177 |
| | unit | `bin\Release\test.exe` | exit 0, 1174 cases | `*** ALL TESTS PASSED ***` | 43 |
| | functional | `bin\Release\functional.exe` | exit 0, 1400 cases, 1 skipped | `*** ALL TESTS PASSED ***` | 469 |
| | production build | `cmake -B build-OFF -A x64 -DROCKETNET_DEV=OFF` then `cmake --build build-OFF --config Release --parallel 4` | exit 0, 0 MSVC warnings | `Building Custom Rule .../CMakeLists.txt` | 145 |

The recut release commit differs from `81b6d3ef` only in this document and
RELEASING.md.

## Tag candidate `471b3fbc` (release/preview-3-gates)

- Candidate head: `471b3fbc70ce88a0c7da2a73ce1b3e60415159dd` on
  `release/preview-3-gates` (PR #599): `main` at `05233f3f` plus #580, #587,
  #588, #593, #597, #598 and #582, and two fixes made during this
  qualification:
  - `32fa0012`: `soak_sim.cpp` sets the server's `handshakes_per_update`.
    #593 made the handshake budget a server field; `soak_sim` builds its
    server by `memset` and left the budget at 0, so every request was
    dropped and `scripts/soak-sim-selftest` failed (platform selftests on
    `81e8b473`).
  - `471b3fbc`: a disposition in `tools/tidy_dispositions.txt` for the
    cppcheck `preprocessorErrorDirective` that #598's liburing version
    `#error` produces (static analysis on `81e8b473`).
- Every suite below ran on a fresh clone at `471b3fbc`, except the 22.04 XDP
  wire cases, which ran at `81e8b473`. The inputs differ between those two
  commits only in `soak_sim.cpp`, `tools/tidy_dispositions.txt` and the
  static-analysis triage document; none is built into `bin/functional`.
- The first tag's commit `389baeef` differs from `471b3fbc` only in this
  document and the release notes. By the rule for carried results above, the
  results below were carried to it from `471b3fbc`. The recut above
  supersedes that tag.

### Native suites at `471b3fbc`

Same commands as above. The Linux row ran on a different bench from the
earlier rows (the bench with the most free memory tonight), so its compiler
differs. Case counts are the `test_` lines each suite printed; a case
skipped by its predicate prints a `SKIPPED` line instead and is not counted.
The unit and functional counts rose by 5 and 6 cases on Linux and macOS
against the rows above: the pull requests merged since add cases.

| Platform | Step | Command | Result | Last line | Seconds |
| --- | --- | --- | --- | --- | ---: |
| Linux x64, gcc 13.3.0 (Ubuntu 24.04.5) | build | `make -j4` | exit 0, 0 warnings | `[100%] Built target functional` | 83 |
| | unit | `./bin/test` | exit 0, 1299 cases | `*** ALL TESTS PASSED ***` | 28 |
| | functional | `./bin/functional` | exit 0, 1521 cases, 21 skipped | `*** ALL TESTS PASSED ***` | 280 |
| | production build | `make clean && make ROCKETNET_DEV=OFF -j4` | exit 0, 0 warnings | `[100%] Built target functional` | 37 |
| | docs | `make docs && make docs-undocumented` | exit 0, 0 warnings | `Generated html documentation in 339ms` (229 pages) | under 1 |
| macOS 27.2 arm64, Apple clang 21.0.0 | build | `make -j4` | exit 0, 0 warnings | `[100%] Built target functional` | 23 |
| | unit | `./bin/test` | exit 0, 1204 cases | `*** ALL TESTS PASSED ***` | 20 |
| | functional | `./bin/functional` | exit 0, 1397 cases, 1 skipped | `*** ALL TESTS PASSED ***` | 204 |
| | production build | `make clean && make ROCKETNET_DEV=OFF -j4` | exit 0, 0 warnings | `[100%] Built target functional` | 19 |
| Windows 11 x64, MSVC 19.51.36260 | build | `cmake -B build-ON -A x64 -DROCKETNET_DEV=ON` then `cmake --build build-ON --config Release --parallel 4` | exit 0, 0 MSVC warnings | `Building Custom Rule .../CMakeLists.txt` | 177 |
| | unit | `bin\Release\test.exe` | exit 0, 1174 cases | `*** ALL TESTS PASSED ***` | 42 |
| | functional | `bin\Release\functional.exe` | exit 0, 1400 cases, 1 skipped | `*** ALL TESTS PASSED ***` | 470 |
| | production build | `cmake -B build-OFF -A x64 -DROCKETNET_DEV=OFF` then `cmake --build build-OFF --config Release --parallel 4` | exit 0, 0 MSVC warnings | `Building Custom Rule .../CMakeLists.txt` | 150 |

The Linux skips are the 19 XDP cases that need root and a veth pair, the
low-tick measurement case, and `test_transport_env_pin_is_honoured` (no
transport pin set); macOS and Windows skip only the last. On Windows the
build and unit suite held a shared slot of the bench's lock (at most three
jobs) and the functional suite its exclusive lock; the seconds are the
suite's own, measured inside the lock. An earlier Windows functional run at
`81e8b473` failed one case,
`test_rio_transport_windows_server_create_destroy_1000`: the process held
two threads more after the loop than before it, none of them the SDK's (its
ledger showed 1000 threads created and 1000 closed). The case then passed
three times in a row alone under the exclusive lock, and the whole suite
passed at `471b3fbc` as recorded. It is recorded here as an intermittent
count of operating-system threads, not fixed.

### Ubuntu 22.04 LTS at `471b3fbc`

The stock `jammy` amd64 cloud image (SHA256 `2099e63d...`, cached and checked
on the bench as the nightly LTS VM workflow does), booted under KVM with its
own kernel, 8 vCPUs and 24 GiB; stock packages: gcc 11.4.0, cmake 3.22.1,
`libbpf-dev` 0.5.0, `liburing-dev` 2.1, no `libxdp-dev`.

| Platform | Step | Command | Result | Last line | Seconds |
| --- | --- | --- | --- | --- | ---: |
| Ubuntu 22.04.5 x64 VM, gcc 11.4.0, kernel 5.15.0-198 | build | `make -j4` | exit 0, 0 warnings | `[100%] Built target functional` | 68 |
| | unit | `./bin/test` | exit 0, 1283 cases | `*** ALL TESTS PASSED ***` | 23 |
| | XDP adapter | `./bin/test_xdp_compat` | exit 0 | `Legacy XDP adapter contracts passed (no kernel operations)` | under 1 |
| | functional | `./bin/functional` | exit 0, 1479 cases, 41 skipped | `*** ALL TESTS PASSED ***` | 214 |

Configure printed `-- XDP: using legacy AF_XDP APIs from libbpf (Ubuntu
22.04)` and `-- liburing found without multishot receive
(IORING_RECV_MULTISHOT, liburing 2.3 or later): the io_uring server
transport is not built`. The AF_XDP server is built (`ROCKETNET_XDP=ON`,
`server_xdp` compiled and embedded, `test_xdp` built); the io_uring server
transport is not.

Why 22.04 prints fewer functional cases than the Linux row: the io_uring
server transport is not built there, so 42 io_uring cases do not run (22
are not compiled in; 20 more print `SKIPPED` on the predicate
`rocketnet_server_uring_available` or `rocketnet_test_posix_io_uring_offered`).
1521 - 42 = 1479. The other 21 skips are the same as on the Linux row. The
figure of 177 seen in an earlier 22.04 trial (176 in the trial run
40609380643 at `48080034`) is a different measure: it is the line count of
`./bin/functional --list`, the in-process registered
cases (178 at this head on 22.04, 204 on the 24.04 host), which leaves out
the cases that run inside the suite's helper executables (`test_serialize`,
`test_transport_control`, `test_xdp` and the unit-case memory check). That
trial ran before #598 and stopped at `test_server_port_zero_transport`
with `io_uring transport refused: built without liburing`, which #598 fixes.

The XDP cases that put the AF_XDP server on a wire need root, a network
namespace and a veth pair, and skip in the run above as they do on every
host. They were run separately in the same image at `81e8b473`, as root
with `ROCKETNET_TEST_XDP_VETH=1`, after setting the kernel command line the
XDP pre-flight requires (`irqaffinity=0 isolcpus=1-3 nohz_full=1-3`) and
rebooting, each with `sudo ROCKETNET_TEST_XDP_VETH=1 ./bin/functional --run
<case>`. These passed: `test_xdp_transport_keep_alive_before_first_payloads`
(IPv4, IPv6), `test_xdp_transport_posix_init_term_1000`,
`..._server_create_destroy_1000`, `..._client_create_destroy_1000`,
`..._server_restart_same_port_100`, `..._connect_disconnect_1000`,
`..._destroy_under_traffic_100`, `test_posix_hard_killed_client_xdp_*`,
`test_posix_send_to_closed_port_xdp_*`, `test_posix_client_server_gone_xdp_*`
(each IPv4 and IPv6), `test_session_reconnect_isolation_xdp`, and
`sudo ./bin/test_xdp` (6 cases). `test_xdp_transport_max_size_both_ways`
(IPv4, IPv6) failed at first with the server receiving every packet (`xdp
redirected = 44`, `transport packets_received = 44`) but its packet capture
empty: the image's AppArmor profile for tcpdump denied tcpdump reading back
its own capture file (`apparmor="DENIED" operation="open"
profile="tcpdump" name="/tmp/rocketnet_xdp_veth_..."`). With that profile
unloaded (`apparmor_parser -R /etc/apparmor.d/usr.bin.tcpdump`), both
passed. The two `test_xdp_transport_linux_receive_weather` cases were not
run: their predicate requires a virtme-ng guest. The guest's virtio NIC is
not a deployment NIC; zero copy was refused and the server bound in copy
mode, and performance was not measured.

### CI at `471b3fbc`

All 30 jobs that ran of the CI, static analysis, msan, CBMC, TLC records,
docs and Release Check workflows on PR #599 passed at `471b3fbc`. The three
tag-only Release Check jobs were skipped, as on any branch push. The six legs
below were cancelled: they cannot finish inside the three-minute per-commit
budget and are waived for this candidate. The native suites recorded above
cover them.

| Waived CI leg | Covered by |
| --- | --- |
| windows / Release (a) | Windows rows above |
| windows / Release (b) | Windows rows above |
| ubuntu-24.04 / Debug | Linux rows above |
| ubuntu-24.04 / Release (b) | Linux rows above |
| macos-15 / Debug | macOS rows above |
| macos-15 / Release | macOS rows above |

The docs workflow is run again on the commit that adds this section.

## LAN proof

1000 clients against one server pinned to one transport, clients on other
machines, every reliability mode checked. Copied from
[runs/lan-1000-proof.md](runs/lan-1000-proof.md), which holds the commands,
host facts and merger lines.

| Platform | Server transport | Head | Clients | Seconds | Connected throughout | Timeouts | Min per-client kbps | Verdict |
| --- | --- | --- | ---: | ---: | --- | ---: | ---: | --- |
| Linux | gso | `1ca47733` | 1000 | 600 | yes | 0 | 278 | pass |
| Linux | io_uring | `1ca47733` | 1000 | 600 | yes | 0 | 276 | pass |
| macOS | msgx | `813b4adb` | 1000 | 600 | yes | 0 | 281 | pass |
| Linux | xdp (copy mode) | `534cce2e` | 1000 | 300 | yes | 0 | 283 | pass |

These cells ran at earlier heads than the candidate; the record gives the
diff of their inputs between heads. Their `timing_verdict=FAIL` is the
per-call budget, which is not part of the proof verdict. The Windows iocp and
rio cells were not made (see "Not qualified").

## Soak records

Each transport soaked for 1800 seconds over loopback on one host with the
link simulator at 5% loss, 30 ms latency, 20 ms jitter and 2% duplication.

| Platform | Cells | Head | Result | Record |
| --- | --- | --- | --- | --- |
| Linux | portable, gso, gso-plain, io_uring, xdp | `adca4345` | every cell PASS, 60 connects each | [runs/linux-soak-30min-per-transport.md](runs/linux-soak-30min-per-transport.md) |
| Linux | xdp server-to-client loss | see record | loss explained in part; see "Not qualified" | [runs/linux-soak-impaired-xdp-loss.md](runs/linux-soak-impaired-xdp-loss.md) |
| macOS | portable, msgx | `31ad7862` | both PASS, 60 connects each, 0 failures | [runs/macos-soak-per-transport.md](runs/macos-soak-per-transport.md) |
| Windows | portable | `74b8bb7f` | PASS, `--assert-flat` | [runs/windows-soak-portable.md](runs/windows-soak-portable.md) |
| Windows | iocp, rio | `3f900fa1` | both PASS, `--assert-flat` | [runs/windows-soak-native-server.md](runs/windows-soak-native-server.md) |

Transport cells repeated fifty times are recorded in
[runs/linux-transport-cells-50x.md](runs/linux-transport-cells-50x.md),
[runs/macos-transport-cells-50x.md](runs/macos-transport-cells-50x.md),
[runs/windows-transport-family-50x.md](runs/windows-transport-family-50x.md)
and
[runs/windows-transport-family-50x-native.md](runs/windows-transport-family-50x-native.md),
each at its own head.

## Documentation and version gates

At the candidate head, run by hand:

- `python3 tools/check_release_doc.py`: exit 0,
  `ok: docs/RELEASING.md names 1 version site(s): sdk/rocketnet.h`.
- The `version-consistency` step of
  [release-check.yml](../.github/workflows/release-check.yml): exit 0;
  shipping `ROCKETNET_VERSION_FULL=v1.0.0-preview-3  MAJOR.MINOR.PATCH=1.0.0`,
  prerelease `preview-3`; development `dev` with `255.255.255`. With
  `GITHUB_REF=refs/tags/v1.0.0-preview-3` it also passes the tag check.
- The `unreleased-wire-changes` comparison: the newest tag is
  `v1.0.0-preview-2`, wire-affecting commits follow it, and the shipping
  version is no longer the tag version, so the job warns that the tag is owed
  instead of failing.
- `make docs && make docs-undocumented`: exit 0 on Linux at the suite head.

## Not qualified

Owed at tag time, or not part of this preview. None of these is reported as a
pass.

- **Review of the public header** `sdk/rocketnet.h` by the release owner,
  before the tag.
- **Tag commit reconciliation**: the merged `main` commit compared with the
  inputs recorded here, and the version, wire-change and tag checks run on it
  (RELEASING.md steps 5 and 7).
- **Space vendor gate**: the integrating application re-vendored at the
  release commit and the `space vendors the release` job passing on it
  (RELEASING.md step 6). Not run.
- **Space soak gate**: the `space builds and soaks on the release` job on the
  same dispatch. Not run.
- **Windows suites at the candidate head `13b82655`**: the Windows unit and
  functional suites ran at the suite head `126ef062` and at the tag candidate
  `471b3fbc`, not at `13b82655`.
- **Windows docs gate**: `make docs` was run on Linux only.
- **Default CI gates** of RELEASING.md ("Default CI requirements"): not
  recorded here; this preview uses manual qualification as preview-2 did.
- **Windows server under LAN load**: the iocp and rio LAN cells were not made.
- **Windows clients against Linux servers**: 1000 Windows clients against an
  AF_XDP server were not retested; the gso server ladder with Windows clients
  is incomplete.
- **Transport cells not yet repeated fifty times**: the Linux AF_XDP cell and
  a Windows iocp client cell.
- **AF_XDP zero copy and line rate**: only copy mode is recorded.
- **Linux AF_XDP soak loss**: the cell passed while losing about twice the
  configured share of server-to-client payloads; the clean re-measurement of
  the receive-buffer cause is owed.
- **Performance**: parity at the release commit is not recorded here; the
  per-call budget check still fails at larger client counts.
- **Sanitizers, fuzzing and toolchain rows**: macOS fuzzing, the Windows
  toolchain, examples and frame-budget runs, the instruction-count check, and
  confirmation of the macOS AddressSanitizer and UndefinedBehaviorSanitizer
  findings' fixes.
- **Proofs and models**: sixteen TLA+ cases end at their time bound in
  `tla/RUNS.tsv`; some CBMC proofs and the full big-endian system run are
  owed.
- **Security**: a complete review of the whole library.

The [release notes](RELEASE-NOTES-v1.0.0-preview-3.md) give the known limits
for users of this preview.
