# Supported platforms

RocketNet's support claim is a claim about CI, not about what the source might
compile on. A platform is supported when a leg in the per-commit workflow
builds it and runs the suite on it for every pull request and every push to
`main`. A platform that only a scheduled workflow runs is nightly. A platform
that no workflow runs is unsupported, even if the code happens to build there.

The table below is the whole claim. Each row names the workflow file and the
runner's operating-system and architecture tokens. `tools/check_platform_policy.py`
checks that each row is backed by the workflow it names. See [docs/LINUX.md](LINUX.md) for the Linux build, transport ladder, kernel/provisioning requirements, and host verification guide, and [docs/WINDOWS.md](WINDOWS.md) for the Windows build, transports, host requirements and verification.

## The table

| Operating system | Architecture | Compiler | Transports | Tier | Workflow | Runner label | Per-commit functional |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Ubuntu 24.04 (Linux, CI) | x86_64 | GCC 13 | portable, gso, io_uring, xdp | supported | .github/workflows/ci.yml | Linux, X64 | full: Run functional suite |
| Ubuntu 24.04 (Linux, cross/QEMU) | s390x | s390x-linux-gnu-gcc | portable | supported | .github/workflows/ci.yml | Linux, X64 | subset: Run the big-endian wire check |
| [macOS 27](MACOS.md) | arm64 | Apple Clang | portable, msgx | supported | .github/workflows/ci.yml | macOS, ARM64 | full: Run functional suite |
| [Windows 11](WINDOWS.md) | x86_64 | MSVC | portable, iocp, rio | supported | .github/workflows/ci.yml | Windows, X64 | subset: Functional: lifecycle binary; Functional: native driver contract; Transport family matrix |
| Ubuntu 24.04 (Linux, CI) | x86_64 | GCC | portable, gso, xdp | nightly | .github/workflows/nightly-leaks.yml | Linux, X64 | - |
| Ubuntu 24.04 (Linux, CI) | x86_64 | Clang | portable | nightly | .github/workflows/fuzz-nightly.yml | Linux, X64 | - |
| Ubuntu 24.04 (Linux, full-system guest) | s390x | GCC | portable | nightly | .github/workflows/nightly-bigendian-system.yml | Linux, X64 | - |
| [macOS 27](MACOS.md) | arm64 | Apple Clang | portable, msgx | nightly | .github/workflows/nightly-soak.yml | macOS, ARM64 | - |
| [macOS 27](MACOS.md) | arm64 | Apple Clang | portable, msgx | nightly | .github/workflows/nightly-frame-budget.yml | macOS, ARM64 | - |
| [macOS 27](MACOS.md) | arm64 | Apple Clang | portable, msgx | nightly | .github/workflows/nightly-examples.yml | macOS, ARM64 | - |
| [macOS 27](MACOS.md) | x86_64 | Apple Clang | portable, msgx | nightly | .github/workflows/nightly-examples.yml | macOS, ARM64 | - |
| [Windows 11](WINDOWS.md) | x86_64 | MSVC | portable, iocp | nightly | .github/workflows/nightly-soak.yml | Windows, X64 | - |
| [Windows 11](WINDOWS.md) | x86_64 | MSVC | portable, iocp | nightly | .github/workflows/nightly.yml | Windows, X64 | - |
| [Windows 11](WINDOWS.md) | x86_64 | clang-cl (LLVM toolset) | portable | nightly | .github/workflows/nightly.yml | Windows, X64 | - |
| Linux | arm64 | none | none | unsupported | - | - | - |
| Windows | arm64 | MSVC (cross, build-checked) | none | unsupported | - | - | - |

The last column says which functional coverage each supported row runs on
every commit. `full: <step>` names the step that runs the whole functional
suite; the checker accepts a step-name prefix, used here where a full name
contains a semicolon. `subset: <step>; <step>` names the steps that run the platform-specific
subset, and the rest of the suite for that row runs in the nightly tier. The
Windows 11 row is a subset: the whole functional suite takes about 223 s on
the Windows runner, which is over the two-minute per-commit law and the
three-minute job bound, so per commit the Windows runner runs the build, the
unit suite, the lifecycle binary, the native driver contract and the transport
family matrix, and the whole suite runs nightly in `nightly.yml` (the Debug
leg under the default and IOCP transports, the Release ASan leg under the
default, IOCP and RIO transports). The s390x row is a subset for the same
reason in kind: per commit it runs the wire and sodium checks under qemu-user,
and the socket-crossing suite runs in the nightly full-system guest.

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
3. The workflow file and runner operating-system and architecture tokens in
   the row correspond to labels present in that file.

It does not promise another compiler, another architecture, another runner
image, privileged XDP attach, line-rate measurements, or a transport the row does
not name. In particular, the per-commit `big-endian` leg in `ci.yml`
cross-compiles the library and suite for s390x and runs the wire and sodium
checks under qemu-user; the socket-crossing suite runs on s390x only in the
nightly full-system guest. The nightly row is how that suite is claimed.

A row's memory bounds follow the same split as its suites: the unit suite
reports memory, printing each case's peak resident set against its bound
without making that OS measurement part of its verdict, and the functional
suite enforces memory, failing on the same bounds through its
`test_unit_case_memory` row, which runs the unit cases and reads the same
bound registry.

### Adding a platform

1. Register a runner with a stable label and add a leg to `ci.yml` that builds
   and runs the suite on it. If the platform is not fit for the per-commit
   budget, add the leg to a nightly workflow instead and it becomes nightly,
   not supported.
2. Add one row to the table in the same change. The row names the operating
   system, architecture, compiler, transports, tier, workflow file and runner
   operating-system and architecture tokens.
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

A supported or nightly row must name both a workflow file and runner
operating-system and architecture tokens.
An empty value, `-`, or a label that does not occur in the named workflow is a
failed check, not a documentation note. `tools/check_platform_policy.py` exits
1 and names the row. This is deliberate: a support claim without a leg is the
one thing the policy exists to prevent.

## Additional checks

Other workflows run release-metadata checks, static analysis, model checking,
fuzzing, leak checks, examples, soak tests and frame-budget checks on platforms
already listed above. These checks add coverage without changing the support
claim.

## Checking the table

From the repository root:

```sh
python3 tools/check_platform_policy.py
```

The checker is standard-library Python. It parses the markdown table, then for
every supported or nightly row reads the named workflow file and checks that
the listed operating-system and architecture tokens occur in it. For every
supported row it also requires the per-commit functional column to name, as
`full` or `subset`, steps whose names occur in a job of that workflow that
holds the row's runner tokens; a supported row with no such declaration fails.
It does not
parse YAML; the workflow files are searched as text.
