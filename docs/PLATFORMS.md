# Supported platforms

RocketNet's support claim is a claim about CI, not about what the source might
compile on. A platform is supported when a leg in the per-commit workflow
builds it and runs the suite on it for every pull request and every push to
`main`. A platform that only a scheduled workflow runs is nightly. A platform
that no workflow runs is unsupported, even if the code happens to build there.

The table below is the whole claim. Each row names the workflow file and the
runner's operating-system and architecture tokens. `tools/check_platform_policy.py`
checks that each row is backed by the workflow it names. See [docs/LINUX.md](LINUX.md) for the Linux build, transport list, kernel/provisioning requirements, and host verification guide, and [docs/WINDOWS.md](WINDOWS.md) for the Windows build, transports, host requirements and verification.

## The table

| Operating system | Architecture | Compiler | Transports | Tier | Workflow | Runner label | Per-commit functional |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Ubuntu 26.04 (Linux, CI) | x86_64 | GCC 15 | portable, gso, io_uring, xdp | supported | .github/workflows/ci.yml | Linux, X64 | full: Run functional suite (automatic transport order — the fall, on Linux; RSS enforced) |
| Ubuntu 26.04 (Linux, cross/QEMU) | s390x | s390x-linux-gnu-gcc | build only: portable, gso, io_uring, xdp; run: no transport (byte cases, XDP filter as host C) | supported | .github/workflows/ci.yml | Linux, X64 | subset: Run the big-endian wire check; Run the byte vectors and the XDP filter cases on big-endian |
| [macOS 27](MACOS.md) | arm64 | Apple Clang | portable, msgx | supported | .github/workflows/ci.yml | macOS, ARM64 | full: Run functional suite (automatic transport order — the fall, on Linux; RSS enforced) |
| [Windows 11](WINDOWS.md) | x86_64 | MSVC | portable, iocp, rio | supported | .github/workflows/ci.yml | Windows, X64 | subset: Run happy_path |
| Ubuntu 26.04 (Linux, CI) | x86_64 | GCC | portable, gso, xdp | nightly | .github/workflows/nightly-leaks.yml | Linux, X64 | - |
| Ubuntu 26.04 (Linux, CI) | x86_64 | Clang | portable | nightly | .github/workflows/fuzz-nightly.yml | Linux, X64 | - |
| Ubuntu 24.04 (Linux, full-system guest) | s390x | GCC | portable (pinned), and the automatic transport order's grant (gso built; io_uring and xdp not built) | nightly | .github/workflows/nightly-bigendian-system.yml | Linux, X64 | - |
| Ubuntu 24.04 LTS (Linux, container) | x86_64 | GCC 13 | build and unit suite: portable, gso, io_uring, xdp (libxdp 1.4) | nightly | .github/workflows/ci.yml | Linux, X64 | - |
| Ubuntu 22.04 LTS (Linux, container) | x86_64 | GCC 11 | build and unit suite: portable, gso, xdp (libbpf 0.5); io_uring not built (liburing 2.1) | nightly | .github/workflows/ci.yml | Linux, X64 | - |
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
suite by its whole name (the rest of the cell is that one name, so a
semicolon in it is part of the name). `subset: <step>; <step>` names, each by
its whole name, the steps that run the platform-specific
subset, and the rest of the suite for that row runs in the nightly tier. The
Windows 11 row is a subset: the whole functional suite takes about 223 s on
the Windows runner, which is over the two-minute per-commit law and the
three-minute job bound, so per commit the Windows runner builds the whole tree
and runs the happy path (`happy path / windows`), and the Release unit suite
runs on every commit in its own job (`windows / Release / unit`); the lifecycle
binary, the native driver contract and the transport family matrix (`windows /
Release` legs (a) to (c)) run in the nightly tier (`nightly-ci.yml`), and the
whole suite, the Debug unit suite included, runs nightly in `nightly.yml` (the
Debug leg under the default and IOCP transports, the Release ASan leg under the
default, IOCP and RIO transports). The s390x row is a subset for the same
reason in kind: per commit it runs the wire and sodium checks, the standard's
byte vectors and the XDP filter cases under qemu-user, and the socket-crossing
suite runs in the nightly full-system guest. [Big-endian coverage](#big-endian-coverage)
below says exactly what is built and run where.

The CI runners are Ubuntu 26.04, so the per-commit Linux row is 26.04.
Ubuntu 22.04 LTS and 24.04 LTS remain supported releases to build and deploy
on, and in this table's terms their CI is nightly: the `distro-container`
job in `ci.yml` (run by `nightly-ci.yml`) installs each release's packages in
a container of that release on a CI runner, builds the tree with its
compiler, and runs the unit suite and the package check (and
`test_xdp_compat` on 22.04). It runs no functional suite, and the container
shares the runner's kernel, so it qualifies the toolchain and userspace
libraries, not the 5.15 or 6.8 kernels. See [docs/LINUX.md](LINUX.md) for
the distribution table.

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
cross-compiles the library and suite for s390x and runs no transport: it runs
the wire and sodium checks and the byte cases under qemu-user. The
socket-crossing suite runs on s390x only in the nightly full-system guest. The
nightly row is how that suite is claimed.
A big-endian Linux build with clang, libbpf and libxdp compiles the xdp
transport in, its kernel program built for the target's byte order (`bpfeb`),
but no row claims the xdp transport on big-endian: the cross leg never loads
the program and the nightly guest builds without it.

### Big-endian coverage

What is built and tested on big-endian (s390x), and what is not.

| Check | Where | When | What it would catch |
| --- | --- | --- | --- |
| The library and the whole unit suite compile for s390x, xdp included | `ci.yml`, `big-endian` | every commit | a byte-order break the compiler sees |
| The embedded XDP object is big-endian (`cmake/CheckXdpObjectByteOrder.cmake`) | `ci.yml`, `big-endian` (any big-endian build with XDP) | every commit | a cross build embedding the build machine's object |
| The XDP program compiles for `bpfeb` against the s390x kernel headers | `ci.yml`, `big-endian` | every commit | a header or macro only one byte order has |
| `tools/bigendian/wire_check.cpp` and `test_sodium` under qemu-user | `ci.yml`, `big-endian` | every commit | the endian helpers, address parses, a same-host seal and open |
| The standard's byte vectors, 59 cases, and 32 XDP filter cases (`tools/bigendian/run_byte_cases.sh`) | `ci.yml`, `big-endian`, qemu-user | every commit | a field written or read in host order that has a vector |
| The XDP filter compiled as host C for s390x, 30 cases | `ci.yml`, `big-endian`, qemu-user; and `./bin/test` in the guest | every commit; nightly | a conversion in the XDP program that swaps whatever the byte order is |
| `./bin/test`, `./bin/functional` (automatic transport order, then portable pinned) | nightly guest | nightly | big-endian to big-endian behaviour of the rungs the guest grants |
| Cross-endian interoperability, three clients each way (`tools/bigendian/run_interop.sh`) | nightly guest against the x86-64 runner | nightly | a wire field in host order that has no vector |
| `tools/check_byte_order.py` over sdk/ | `ci.yml`, lint | every commit | a fixed-direction swap or a host-order read of wire bytes, at review |

Not built or not run on big-endian:

- The XDP data plane. No big-endian host attaches the XDP program or runs the
  AF_XDP frame path: the nightly guest installs no clang, so XDP is compiled
  out there, and the per-commit leg runs no transport. The filter's decisions
  run big-endian only as host C, with the kernel's byte-order macros for
  s390x; the verifier, the real helpers and the real maps are not exercised
  big-endian.
- The io_uring rung: the guest installs no liburing, so it is not built there,
  and no transport runs under qemu-user (it reports on the emulator, not the
  platform). gso runs only if the guest's automatic transport order grants it.
- A physical big-endian machine. Every big-endian run is emulated.
- Local table hashes (never on the wire) pick different buckets on
  big-endian; nothing checks that a seeded IPv6 simulator run is identical
  across byte orders.

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
An empty value, `-`, or a label that no job of the named workflow runs on
(every token in one `runs-on:` or matrix `runner:` label set, not tokens
gathered from several runners or from comments) is a failed check, not a
documentation note. `tools/check_platform_policy.py` exits
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
one job runs on a runner whose label set (one `runs-on:` value, or one matrix
`runner:` value) holds every listed operating-system and architecture token.
For every supported row it also requires the per-commit functional column to
name, as `full` or `subset`, steps whose whole names are steps of such a job,
and refuses a named step whose `if:`, or whose job's `if:`, is false on every
per-commit run (`false`, or `inputs.full`, which only the nightly caller
sets); a supported row with no such declaration fails. Every transport a
supported row names outside a `build only:` clause must be a server or client
transport of a `TRANSPORT_MATRIX_<OS>` cell in such a job. It does not parse
YAML; the workflow files are read as text, and the `if:` expressions with a
small reader of GitHub's expression syntax that refuses what it cannot read.
`python3 tools/check_platform_policy.py --self-check` plants each refused case
in a scratch tree and fails if any is let through.
