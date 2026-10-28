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
- **Windows suites at the candidate head**: the Windows unit and functional
  suites ran at the suite head `126ef062` only.
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
- **Security**: a review of the whole library, and the replayed-response and
  cookie-rewind issues listed in the [release notes](RELEASE-NOTES-v1.0.0-preview-3.md).

The [release notes](RELEASE-NOTES-v1.0.0-preview-3.md) give the known limits
for users of this preview.
