# Supported platforms

RocketNet's support claim is a claim about CI, not about what the source might
compile on. A platform is supported when a leg in the per-commit workflow
builds it and runs the suite on it for every pull request and every push to
`main`. A platform that only a scheduled workflow runs is nightly. A platform
that no workflow runs is unsupported, even if the code happens to build there.

The table below is the whole claim. Each row names the workflow file and the
runner label that prove it. `tools/check_platform_policy.py` reads the table
and refuses a supported or nightly row whose label is not in the workflow it
names. See [docs/LINUX.md](LINUX.md) for the Linux build, transport ladder, kernel/provisioning requirements, and host verification guide.

## The table

| Operating system | Architecture | Compiler | Transports | Tier | Workflow | Runner label |
| --- | --- | --- | --- | --- | --- | --- |
| Ubuntu 24.04 (Linux, self-hosted fleet) | x86_64 | GCC 13 | portable, gso, io_uring, xdp | supported | .github/workflows/ci.yml | self-hosted, Linux, X64, fleet |
| Ubuntu 24.04 (Linux, cross/QEMU) | s390x | s390x-linux-gnu-gcc | portable | supported | .github/workflows/ci.yml | self-hosted, Linux, X64, fleet |
| [macOS 27 (self-hosted fleet)](MACOS.md) | arm64 | Apple Clang | portable, msgx | supported | .github/workflows/ci.yml | self-hosted, macOS, ARM64, rocketnet-macos |
| Windows 11 (self-hosted fleet) | x86_64 | MSVC | portable, iocp, rio | supported | .github/workflows/ci.yml | self-hosted, Windows, X64, rocketnet-windows |
| Ubuntu 24.04 (Linux, self-hosted fleet) | x86_64 | GCC | portable, gso, xdp | nightly | .github/workflows/nightly-leaks.yml | self-hosted, Linux, X64, fleet |
| Ubuntu 24.04 (Linux, self-hosted fleet) | x86_64 | Clang | portable | nightly | .github/workflows/fuzz-nightly.yml | self-hosted, Linux, X64, fleet |
| Ubuntu 24.04 (Linux, full-system guest) | s390x | GCC | portable | nightly | .github/workflows/nightly-bigendian-system.yml | self-hosted, Linux, X64, fleet |
| [macOS 27 (self-hosted fleet)](MACOS.md) | arm64 | Apple Clang | portable, msgx | nightly | .github/workflows/nightly-soak.yml | self-hosted, macOS, ARM64, rocketnet-macos |
| [macOS 27 (self-hosted fleet)](MACOS.md) | arm64 | Apple Clang | portable, msgx | nightly | .github/workflows/nightly-frame-budget.yml | self-hosted, macOS, ARM64, rocketnet-macos |
| [macOS 27 (self-hosted fleet)](MACOS.md) | arm64 | Apple Clang | portable, msgx | nightly | .github/workflows/nightly-examples.yml | self-hosted, macOS, ARM64, rocketnet-macos |
| [macOS 27 (self-hosted fleet)](MACOS.md) | x86_64 | Apple Clang | portable, msgx | nightly | .github/workflows/nightly-examples.yml | self-hosted, macOS, ARM64, rocketnet-macos |
| Windows 11 (self-hosted fleet) | x86_64 | MSVC | portable, iocp | nightly | .github/workflows/nightly-soak.yml | self-hosted, Windows, X64, rocketnet-windows |
| Windows 11 (self-hosted fleet) | x86_64 | MSVC | portable, iocp | nightly | .github/workflows/nightly.yml | self-hosted, Windows, X64, rocketnet-windows |
| Windows 11 (self-hosted fleet) | x86_64 | clang-cl (LLVM toolset) | portable | nightly | .github/workflows/nightly.yml | self-hosted, Windows, X64, rocketnet-windows |
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
and verifying on a Mac. The transports column names the backends the row actually
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
- `nightly-soak.yml` runs its soak job on `ubuntu-latest` with the
  `self-hosted, Linux, X64, fleet` label, its soak job on the
  `self-hosted, macOS, ARM64, rocketnet-macos` label, its soak-windows job on
  the `self-hosted, Windows, X64, rocketnet-windows` label, and its
  sanitized-soak job on the `self-hosted, Linux, X64, fleet` label.
- `nightly-frame-budget.yml` has its Linux leg on
  `self-hosted, Linux, X64, rocketnet-quiet`, macOS leg on
  `self-hosted, macOS, ARM64, rocketnet-macos`, and Windows leg (hosted
  `windows-2025`) disabled (`if: false`, no Windows fleet runner registered).
- `nightly-examples.yml` has its Linux leg on
  `self-hosted, Linux, X64, fleet`, macOS leg on
  `self-hosted, macOS, ARM64, rocketnet-macos`, and Windows leg (hosted
  `windows-2025`) disabled (`if: false`, no Windows fleet runner registered).
- `nightly.yml`'s `windows-transport-extension` job (hosted `windows-2025`) is
  disabled (`if: false`, no Windows fleet runner registered).
- `ci.yml`'s `platform-selftests` job runs on the
  `self-hosted, Linux, X64, fleet` and
  `self-hosted, macOS, ARM64, rocketnet-macos` labels.
- `fuzz-nightly.yml` runs on the `self-hosted, Linux, X64, fleet` label, and
  its fuzz-macos job on `self-hosted, macOS, ARM64, rocketnet-macos`.

## The Windows verification runs

The table above is the support claim. The table below is not: it records the
Windows Debug, AddressSanitizer and Application Verifier runs, which are made
on the native Windows member (`self-hosted, Windows, X64, rocketnet-windows`),
one run at a time, and are owed to the current API whenever the source moves
under them. A row that says "not made" is a row that was not run, not a row
that passed. `BUILDING.md` names the commands.

| Group | Configuration | Runs | Result | Date |
| --- | --- | --- | --- | --- |
| Debug | Debug | `test.exe`, `test_lifecycle.exe`, `test_iocp.exe`, `functional.exe` (no pin, `ROCKETNET_SERVER_TRANSPORT=iocp`, `ROCKETNET_SERVER_TRANSPORT=rio`) | not made | 2026-10-23 |
| AddressSanitizer | Release | the same six runs | not made | 2026-10-23 |
| AddressSanitizer | Debug | the same six runs | not made | 2026-10-23 |
| Application Verifier | Debug | `test_iocp.exe`, `test_lifecycle.exe`, `functional.exe` (no pin, `iocp`, `rio`) | not made | 2026-10-23 |

The 2026-10-23 rows were recorded on Linux, where no native Windows member is
reachable: the runs themselves were not made there. The Linux Release gate
(`make -j16`, then `./bin/test` and `./bin/functional`) was re-verified at this
head on this attempt and was green: both suites ended in `*** ALL TESTS PASSED
***`. A Linux Release AddressSanitizer build (`-DROCKETNET_SANITIZE_ADDRESS=ON`)
of the same tree ran `test` and `functional` and both ended in `*** ALL TESTS
PASSED ***` with zero AddressSanitizer reports (no `AddressSanitizer` or
`ERROR: AddressSanitizer` line in either run's output). That is evidence about
the shared sources only; it is not a Windows row. The nightly Windows
AddressSanitizer job (`nightly.yml`, `windows / Release ASan`) runs the Release
group every night; the Debug AddressSanitizer configuration is build-checked
there and its suites are owed to the member.

## Checking the table

From the repository root:

```sh
python3 tools/check_platform_policy.py
```

The checker is standard-library Python. It parses the markdown table, then for
every supported or nightly row reads the named workflow file and requires every
token of the runner label to occur in it. It does not parse YAML; the workflow
files are searched as text because the labels are how the runners are chosen.
