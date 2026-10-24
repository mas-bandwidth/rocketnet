# Supported platforms

RocketNet's support claim is a claim about CI, not about what the source might
compile on. A platform is supported when a leg in the per-commit workflow
builds it and runs the suite on it for every pull request and every push to
`main`. A platform that only a scheduled workflow runs is nightly. A platform
that no workflow runs is unsupported, even if the code happens to build there.

The table below is the whole claim. Each row names the workflow file and the
runner label that prove it. `tools/check_platform_policy.py` reads the table
and refuses a supported or nightly row whose label is not in the workflow it
names. See [docs/LINUX.md](LINUX.md) for the Linux build, transport ladder, kernel/provisioning requirements, and host verification guide, and [docs/WINDOWS.md](WINDOWS.md) for the Windows build, transports, host requirements and verification.

## The table

| Operating system | Architecture | Compiler | Transports | Tier | Workflow | Runner label |
| --- | --- | --- | --- | --- | --- | --- |
| Ubuntu 24.04 (Linux, self-hosted fleet) | x86_64 | GCC 13 | portable, gso, io_uring, xdp | supported | .github/workflows/ci.yml | self-hosted, Linux, X64, fleet |
| Ubuntu 24.04 (Linux, cross/QEMU) | s390x | s390x-linux-gnu-gcc | portable | supported | .github/workflows/ci.yml | self-hosted, Linux, X64, fleet |
| [macOS 27 (self-hosted fleet)](MACOS.md) | arm64 | Apple Clang | portable, msgx | supported | .github/workflows/ci.yml | self-hosted, macOS, ARM64, rocketnet-macos |
| [Windows 11 (self-hosted fleet)](WINDOWS.md) | x86_64 | MSVC | portable, iocp, rio | supported | .github/workflows/ci.yml | self-hosted, Windows, X64, rocketnet-windows |
| Ubuntu 24.04 (Linux, self-hosted fleet) | x86_64 | GCC | portable, gso, xdp | nightly | .github/workflows/nightly-leaks.yml | self-hosted, Linux, X64, fleet |
| Ubuntu 24.04 (Linux, self-hosted fleet) | x86_64 | Clang | portable | nightly | .github/workflows/fuzz-nightly.yml | self-hosted, Linux, X64, fleet |
| Ubuntu 24.04 (Linux, full-system guest) | s390x | GCC | portable | nightly | .github/workflows/nightly-bigendian-system.yml | self-hosted, Linux, X64, fleet |
| [macOS 27 (self-hosted fleet)](MACOS.md) | arm64 | Apple Clang | portable, msgx | nightly | .github/workflows/nightly-soak.yml | self-hosted, macOS, ARM64, rocketnet-macos |
| [macOS 27 (self-hosted fleet)](MACOS.md) | arm64 | Apple Clang | portable, msgx | nightly | .github/workflows/nightly-frame-budget.yml | self-hosted, macOS, ARM64, rocketnet-macos |
| [macOS 27 (self-hosted fleet)](MACOS.md) | arm64 | Apple Clang | portable, msgx | nightly | .github/workflows/nightly-examples.yml | self-hosted, macOS, ARM64, rocketnet-macos |
| [macOS 27 (self-hosted fleet)](MACOS.md) | x86_64 | Apple Clang | portable, msgx | nightly | .github/workflows/nightly-examples.yml | self-hosted, macOS, ARM64, rocketnet-macos |
| [Windows 11 (self-hosted fleet)](WINDOWS.md) | x86_64 | MSVC | portable, iocp | nightly | .github/workflows/nightly-soak.yml | self-hosted, Windows, X64, rocketnet-windows |
| [Windows 11 (self-hosted fleet)](WINDOWS.md) | x86_64 | MSVC | portable, iocp | nightly | .github/workflows/nightly.yml | self-hosted, Windows, X64, rocketnet-windows |
| [Windows 11 (self-hosted fleet)](WINDOWS.md) | x86_64 | clang-cl (LLVM toolset) | portable | nightly | .github/workflows/nightly.yml | self-hosted, Windows, X64, rocketnet-windows |
| Linux | arm64 | none | none | unsupported | - | - |
| Windows | arm64 | MSVC (cross, build-checked) | none | unsupported | - | - |

clang-cl x64 is built and tested nightly: the `windows-toolchains` job in
`nightly.yml` builds the static and shared libraries, `test.exe` and
`functional.exe` with the LLVM toolset, runs both suites, and runs the DLL
consumer, package and export checks against that build. Windows arm64 is
build-checked and untested: the same job cross-builds the static and shared
libraries with MSVC for ARM64, checks that `rocketnet.dll` is an ARM64 image
and checks its exports. Nothing runs on arm64, so its row stays unsupported.

The macOS rows link to [MACOS.md](MACOS.md), the guide for building, pinning
and verifying on a Mac, and the Windows rows to [WINDOWS.md](WINDOWS.md), the
same guide for Windows. The transports column names the backends the row actually
exercises or proves in CI. The registry (`rocketnet_drivers.inc`) also lists
portable and loopback on every platform, gso, io_uring and xdp on Linux, msgx on
macOS, and iocp and rio on Windows. A transport that the row does not name is not part of that row's claim.

## Policy rules

### What supported promises

A supported row promises three things, and only these three:

1. Every commit's CI includes a leg that builds the library with the compiler
   in the row and runs the unit and functional suite, or the platform-specific
   subset the row describes, on the operating system and architecture in the
   row.
2. A red leg on that platform is a merge blocker. It is not a nightly finding
   that may wait for morning.
3. The workflow file and runner label in the row are the ones that run it, and
   the label is present in that file.

It does not promise another compiler, another architecture, another runner
image, privileged XDP attach, line-rate measurements, or a transport the row does
not name. In particular, the per-commit `big-endian` leg in `ci.yml`
cross-compiles the library and suite for s390x and runs the wire and sodium
checks under qemu-user; the socket-crossing suite runs on s390x only in the
nightly full-system guest. The nightly row is how that suite is claimed.

### Adding a platform

1. Register a runner with a stable label and add a leg to `ci.yml` that builds
   and runs the suite on it. If the platform is not fit for the per-commit
   budget, add the leg to a nightly workflow instead and it becomes nightly,
   not supported.
2. Add one row to the table in the same change. The row names the operating
   system, architecture, compiler, transports, tier, workflow file and runner
   label.
3. Run `python3 tools/check_platform_policy.py`. A row whose label is absent
   from the named workflow fails the check.

A platform is not added by landing code that compiles on it, by adding it to a
comment, or by naming it in a release note. It is added by the CI leg and the
row together.

### Dropping a platform

Remove the CI leg and its row in the same change. A row may not outlive the
leg that proves it. To demote a platform, move its leg from `ci.yml` to a
nightly workflow and change the row's tier, workflow and label in the same
change. To withdraw support entirely, delete the leg and the row; the
unsupported rows in the table record architectures for which no leg exists.

### A claim with no leg is refused

A supported or nightly row must name both a workflow file and a runner label.
An empty value, `-`, or a label that does not occur in the named workflow is a
failed check, not a documentation note. `tools/check_platform_policy.py` exits
1 and names the row. This is deliberate: a support claim without a leg is the
one thing the policy exists to prevent.

## Workflows that do not add a platform row

These workflows run on a platform already in the table and add checks, not a
new support claim:

- `release-check.yml` runs its version-consistency job on
  `self-hosted, Linux, X64, fleet` per commit and its wire-change job on pushes to `main`. It
  checks release metadata, not the library build.
- `cbmc.yml` runs arithmetic proofs on `self-hosted, Linux, X64, fleet` for pull requests
  that touch `sdk/` or `proofs/cbmc/`.
- `static.yml`, `tlc.yml`, `msan.yml` and `nightly-leaks.yml` run on the
  self-hosted `[self-hosted, Linux, X64, fleet]` runners. They are analysis
  and leak gates on the supported Linux x86_64 platform.
- `nightly-soak.yml` runs its soak job on the
  `self-hosted, Linux, X64, fleet, long` label, its soak-macos job on the
  `self-hosted, macOS, ARM64, rocketnet-macos` label, its soak-windows job on
  the `self-hosted, Windows, X64, rocketnet-windows` label, and its
  sanitized-soak job on the `self-hosted, Linux, X64, fleet` label.
- `nightly-frame-budget.yml` has its Linux leg on
  `self-hosted, Linux, X64, rocketnet-quiet`, macOS leg on
  `self-hosted, macOS, ARM64, rocketnet-macos`, and Windows leg
  disabled (`if: false`; its label is `self-hosted, Windows, X64, rocketnet-windows`).
- `nightly-examples.yml` has its Linux leg on
  `self-hosted, Linux, X64, fleet`, macOS leg on
  `self-hosted, macOS, ARM64, rocketnet-macos`, and Windows leg
  disabled (`if: false`; its label is `self-hosted, Windows, X64, rocketnet-windows`).
- `nightly.yml`'s `windows-transport-extension` job is
  disabled (`if: false`; its label is `self-hosted, Windows, X64, rocketnet-windows`).
- `ci.yml`'s `platform-selftests` job runs on the
  `self-hosted, Linux, X64, fleet` and
  `self-hosted, macOS, ARM64, rocketnet-macos` labels.
- `fuzz-nightly.yml` runs on the `self-hosted, Linux, X64, fleet` label, and
  its fuzz-macos job on `self-hosted, macOS, ARM64, rocketnet-macos`.

## The Windows verification runs

The table above is the support claim. The table below is not: it records the
Windows Debug, AddressSanitizer and Application Verifier runs, which are made
on the native Windows runner (`self-hosted, Windows, X64, rocketnet-windows`),
one run at a time, and are owed to the current API whenever the source moves
under them. A row that says "not made" is a row that was not run, not a row
that passed. `BUILDING.md` names the commands.

Each run below was made once on the Windows runner (Windows 11 build
26200, cl 19.51.36260), with nothing else running, at rocketnet commit
`79834cfa`, on 2026-10-23. "No pin" clears `ROCKETNET_SERVER_TRANSPORT`; a pin
is set with cmd.exe `set`, and each pinned `functional.exe` run printed
`test_transport_env_pin_is_honoured`. The Debug runs, with or without
AddressSanitizer, set `ROCKETNET_TEST_MAX_CASE_RSS_MB=2048`, the Debug figure
`nightly.yml`'s `windows / Debug` job uses; the Release AddressSanitizer runs
keep the 698 MiB bound in code.

| Group | Configuration | Run | Result | Time | Date |
| --- | --- | --- | --- | --- | --- |
| Debug | Debug | `test.exe` | pass | 32 s | 2026-10-23 |
| Debug | Debug | `test_lifecycle.exe` | pass | 1 s | 2026-10-23 |
| Debug | Debug | `test_iocp.exe` | pass | 24 s | 2026-10-23 |
| Debug | Debug | `functional.exe`, no pin | pass | 53 s | 2026-10-23 |
| Debug | Debug | `functional.exe`, `iocp` | pass | 54 s | 2026-10-23 |
| Debug | Debug | `functional.exe`, `rio` | pass | 58 s | 2026-10-23 |
| AddressSanitizer | Release | `test.exe` | pass, 0 reports | 66 s | 2026-10-23 |
| AddressSanitizer | Release | `test_lifecycle.exe` | pass, 0 reports | 1 s | 2026-10-23 |
| AddressSanitizer | Release | `test_iocp.exe` | pass, 0 reports | 30 s | 2026-10-23 |
| AddressSanitizer | Release | `functional.exe`, no pin | pass, 0 reports | 55 s | 2026-10-23 |
| AddressSanitizer | Release | `functional.exe`, `iocp` | pass, 0 reports | 57 s | 2026-10-23 |
| AddressSanitizer | Release | `functional.exe`, `rio` | pass, 0 reports | 58 s | 2026-10-23 |
| AddressSanitizer | Debug | `test.exe` | pass, 0 reports | 107 s | 2026-10-23 |
| AddressSanitizer | Debug | `test_lifecycle.exe` | pass, 0 reports | 1 s | 2026-10-23 |
| AddressSanitizer | Debug | `test_iocp.exe` | pass, 0 reports | 31 s | 2026-10-23 |
| AddressSanitizer | Debug | `functional.exe`, no pin | pass, 0 reports | 57 s | 2026-10-23 |
| AddressSanitizer | Debug | `functional.exe`, `iocp` | pass, 0 reports | 57 s | 2026-10-23 |
| AddressSanitizer | Debug | `functional.exe`, `rio` | pass, 0 reports | 57 s | 2026-10-23 |
| Application Verifier | Debug | `test_iocp.exe`, no pin | pass, 0 stops | 28 s | 2026-10-23 |
| Application Verifier | Debug | `test_iocp.exe`, `iocp` | pass, 0 stops | 28 s | 2026-10-23 |
| Application Verifier | Debug | `test_iocp.exe`, `rio` | pass, 0 stops | 28 s | 2026-10-23 |
| Application Verifier | Debug | `test_lifecycle.exe`, no pin | pass, 0 stops | 1 s | 2026-10-23 |
| Application Verifier | Debug | `test_lifecycle.exe`, `iocp` | pass, 0 stops | 1 s | 2026-10-23 |
| Application Verifier | Debug | `test_lifecycle.exe`, `rio` | pass, 0 stops | 2 s | 2026-10-23 |
| Application Verifier | Debug | `functional.exe`, no pin | pass, 0 stops | 121 s | 2026-10-23 |
| Application Verifier | Debug | `functional.exe`, `iocp` | pass, 0 stops | 117 s | 2026-10-23 |
| Application Verifier | Debug | `functional.exe`, `rio` | pass, 0 stops | 119 s | 2026-10-23 |

The Application Verifier runs are `py -3 tools\windows\appverif_run.py --xml <file> -- bin\Debug\<exe>` from an elevated prompt (the Basics layers, every
stop logged); the times are the verified process's own, as the script prints
them. No run found an AddressSanitizer report, a verifier stop or a suite
failure, so no source change came of them. The nightly Windows
AddressSanitizer job (`nightly.yml`, `windows / Release ASan`) repeats the
Release group every night and build-checks the Debug configuration.

## Checking the table

From the repository root:

```sh
python3 tools/check_platform_policy.py
```

The checker is standard-library Python. It parses the markdown table, then for
every supported or nightly row reads the named workflow file and requires every
token of the runner label to occur in it. It does not parse YAML; the workflow
files are searched as text because the labels are how the runners are chosen.
