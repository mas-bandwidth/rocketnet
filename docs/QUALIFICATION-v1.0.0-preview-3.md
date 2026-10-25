# Preview-3 qualification record

This is the qualification record of the v1.0.0-preview-3 release candidate.
It is a candidate, not a release: no tag is cut. The tag is cut through
[RELEASING.md](RELEASING.md) steps 5 to 8 after the release owner has reviewed
`sdk/rocketnet.h`. Results are copied from the runs and records named here; a
result carried from another commit names that commit.

## Candidate head

The candidate head is `160a880b99468ecb3dd2094dad535b394946178b`: `main` at
`7d70806b` plus the shipping version label and documentation only. Every
native suite below ran at that head. The commit that adds this record and the
release notes changes documentation only (no compiled file), so the suites at
`160a880b` hold for it.

## Native unit and functional suites at the candidate head

Each suite ran in a Release build at `160a880b`, from a fresh clone. The
last line of each run is copied.

| platform | command | exit | last line |
| --- | --- | --- | --- |
| Linux x64, gcc 15.2, Ubuntu 26.04 | `make -j4` | 0 | `[100%] Built target functional` (0 warnings) |
| Linux x64, gcc 15.2 | `./bin/test` | 0 | `*** ALL TESTS PASSED ***` |
| Linux x64, gcc 15.2 | `./bin/functional` | 0 | `*** ALL TESTS PASSED ***` |
| Linux x64, gcc 15.2 | `make clean && make ROCKETNET_DEV=OFF -j4` | 0 | `[100%] Built target functional` (0 warnings) |
| Linux x64, clang 21.1 | `CC=clang CXX=clang++ make ROCKETNET_DEV=OFF -j4` | 0 | `[100%] Built target functional` (0 warnings) |
| Linux x64, clang 21.1 | `CC=clang CXX=clang++ make -j4` | 0 | `[100%] Built target functional` (0 warnings) |
| macOS arm64, Apple clang 21.0, macOS 27.2 | `make -j4` | 0 | `[100%] Built target functional` (0 warnings) |
| macOS arm64 | `./bin/test` | 0 | `*** ALL TESTS PASSED ***` |
| macOS arm64 | `./bin/functional` | 0 | `*** ALL TESTS PASSED ***` |
| macOS arm64 | `make clean && make ROCKETNET_DEV=OFF -j4` | 0 | `[100%] Built target functional` (0 warnings) |
| Windows x64, MSVC (Visual Studio 18), Windows 11 | `cmake -B build -A x64 -DROCKETNET_DEV=ON && cmake --build build --config Release --parallel 4` | 0 | `functional.vcxproj -> ...\bin\Release\functional.exe` (0 warnings, 0 errors) |
| Windows x64, MSVC | `cmake -B build -A x64 -DROCKETNET_DEV=OFF && cmake --build build --config Release --parallel 4` | 0 | `functional.vcxproj -> ...\bin\Release\functional.exe` (0 warnings, 0 errors) |
| Windows x64, MSVC | `bin/Release/test.exe` | 0 | `*** ALL TESTS PASSED ***` (31 s) |
| Windows x64, MSVC | `bin/Release/functional.exe` | 0 | `*** ALL TESTS PASSED ***` (430 s) |

The Windows unit suite ran with up to two other builds or unit suites on the
same host; the Windows functional suite ran with the host to itself.

## Release checks at the candidate head

| check | command | result |
| --- | --- | --- |
| Release document | `python3 tools/check_release_doc.py` | exit 0: `ok: docs/RELEASING.md names 1 version site(s): sdk/rocketnet.h` |
| version consistency | the `version-consistency` step of [release-check.yml](../.github/workflows/release-check.yml), run by hand | exit 0: `shipping: ROCKETNET_VERSION_FULL=v1.0.0-preview-3  MAJOR.MINOR.PATCH=1.0.0`, `shipping prerelease: preview-3`, `development: ROCKETNET_VERSION_FULL=dev  MAJOR.MINOR.PATCH=255.255.255` |
| version consistency, tag mode | the same step with `GITHUB_REF=refs/tags/v1.0.0-preview-3` | exit 0: `release tag: v1.0.0-preview-3` |
| unreleased wire changes | the `unreleased-wire-changes` step, run by hand | exit 0 with the expected warning: 46 wire-affecting commits past `v1.0.0-preview-2`, version already bumped to `v1.0.0-preview-3`. With the previous header label the same step exits 1 (`the shipping version is still v1.0.0-preview-2`) |
| API documentation | `make docs && make docs-undocumented` | exit 0: `Generated 226 pages`; the undocumented list names two parameters, `configuration` of `rocketnet_client_light_config_internal` and of `rocketnet_server_light_config_internal` |

## LAN proof

1000 clients across the LAN against one server pinned to one transport, the
server alone on its machine, the clients in shards of at most 250 on other
machines. Rows are copied from [runs/lan-1000-proof.md](runs/lan-1000-proof.md)
and [runs/lan-1000-proof-xdp.md](runs/lan-1000-proof-xdp.md), which hold the
commands, host facts and receipts.

| platform | server transport | head | server NIC | seconds | connected throughout | timeouts | modes 0-7 | sizes | min per-client kbps | unreliable lost | verdict |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Linux | gso | `1ca47733` | 1000 Mb/s | 600 | yes | 0 | pass | small, fragmented pass | 278 | 0 | pass |
| Linux | io_uring | `1ca47733` | 1000 Mb/s | 600 | yes | 0 | pass | small, fragmented pass | 276 | 110,680 | pass |
| macOS | msgx | `813b4adb` | 1000 Mb/s | 600 | yes | 0 | pass | small, fragmented pass | 281 | 25,662 | pass |
| Linux | xdp (copy mode) | `534cce2e` | 10000 Mb/s | 300 | yes | 0 | pass | small, fragmented pass | 283 | 1,457 | pass |

The merger's `timing_verdict=FAIL` on every cell is the per-call timing
budget, which is not part of the proof verdict. The LAN cells ran at earlier
heads than the candidate; the records name each head and the difference of
their inputs.

## Per-transport soak records

Each cell ran for 1800 s on one host over loopback, with the link simulator
at 5% loss, 30 ms latency, 20 ms jitter and 2% duplication, under
`--assert-flat` (correctness only; no NIC is exercised).

| platform | cells | head | result | record |
| --- | --- | --- | --- | --- |
| Linux | portable, gso, gso-plain, io_uring, xdp | `adca4345` | all five PASS, 60 connects each | [runs/linux-soak-30min-per-transport.md](runs/linux-soak-30min-per-transport.md) |
| macOS | portable, msgx | `31ad7862` | both PASS, 60 connects each, 0 failures | [runs/macos-soak-per-transport.md](runs/macos-soak-per-transport.md) |
| Windows | portable | `74b8bb7f` | PASS, exit 0 | [runs/windows-soak-portable.md](runs/windows-soak-portable.md) |
| Windows | iocp, rio (portable client) | `3f900fa1` | both PASS, every payload received | [runs/windows-soak-native-server.md](runs/windows-soak-native-server.md) |

The fifty-run transport cell records are
[runs/linux-transport-cells-50x.md](runs/linux-transport-cells-50x.md),
[runs/macos-transport-cells-50x.md](runs/macos-transport-cells-50x.md),
[runs/windows-transport-family-50x.md](runs/windows-transport-family-50x.md)
and [runs/windows-transport-family-50x-native.md](runs/windows-transport-family-50x-native.md).

## TLA+ ledger

`tla/RUNS.tsv` at the candidate head has 409 cases: 393 meet their expected
outcome and 16 end at their 110 s bound (exit 124), which are bounded-timeout
debt and not passes.

## Owed at tag time

These release gates of [RELEASING.md](RELEASING.md) run on the release commit
when the tag is cut, and were not run for this candidate:

- the `space vendors the release` gate;
- the `space builds and soaks on the release` gate;
- the owner's review of `sdk/rocketnet.h`.

## Not qualified

- A Windows server under LAN load: the iocp and rio LAN cells were not made,
  because the test host has no firewall rule for the test port.
- 1000 Windows clients against an AF_XDP server, and the gso server ladder
  with Windows clients.
- AF_XDP zero copy and line rate: only copy mode was measured.
- The fifty-run repeat of the Linux AF_XDP cell and the Windows iocp client
  cell.
- The portable server under LAN load: the proof covers native server
  transports only.
- Parity with the previous networking stack at the release commit.
- The 100 microsecond per-call server update target, which still fails at
  higher client counts and is replaced by parity.
- macOS fuzzing; the Windows toolchain, examples and frame-budget runs; the
  instruction-count check.
- The macOS AddressSanitizer and UndefinedBehaviorSanitizer findings, whose
  fixes are not confirmed.
- One full big-endian system run.
- The CBMC proofs of the full-size packet readers, the ack-block reader, and
  the sequence buffers and replay window; the 16 bounded TLA+ cases.
- Mixed preview-2 and preview-3 clients and servers.
- A security review of the whole library.
- Two undocumented parameters named by `make docs-undocumented` above.

The known issues behind each row are in the
[release notes](RELEASE-NOTES-v1.0.0-preview-3.md#known-issues).
