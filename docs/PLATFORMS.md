# Supported platforms

RocketNet's support claim is a claim about the CI that runs, not about what the
source might compile on. A platform is supported when the per-commit CI builds
it and runs the suite on it for every pull request and every push to `main`. A
platform that only a scheduled workflow runs is nightly. A platform that no
workflow runs is unsupported, even if the code happens to build there.

The table below is the whole claim. It names the transports each row exercises
or proves. A transport that a row does not name is not part of that row's
claim.

## The table

| Operating system | Architecture | Compiler | Transports | Tier |
| --- | --- | --- | --- | --- |
| Ubuntu 24.04 (Linux) | x86_64 | GCC 13 | portable, gso, io_uring, xdp | supported |
| Ubuntu 24.04 (Linux, cross/QEMU) | s390x | s390x-linux-gnu-gcc | portable | supported |
| macOS 27 | arm64 | Apple Clang | portable, msgx | supported |
| Windows Server 2025 | x86_64 | MSVC | portable, iocp, rio | supported |
| Ubuntu 24.04 (Linux) | x86_64 | GCC | portable, gso, io_uring, xdp | nightly |
| Ubuntu 24.04 (Linux) | x86_64 | Clang | portable | nightly |
| Ubuntu 24.04 (Linux, full-system guest) | s390x | GCC | portable | nightly |
| macOS 27 | arm64 | Apple Clang | portable, msgx | nightly |
| Windows Server 2025 | x86_64 | MSVC | portable, iocp, rio | nightly |
| Linux | arm64 | none | none | unsupported |
| Windows | arm64 | none | none | unsupported |
| macOS | x86_64 | none | none | unsupported |

The registry (`sdk/rocketnet_drivers.inc`) lists the built-in transports:
`portable` on every platform, `gso`, `io_uring` and `xdp` on Linux, `msgx` on
macOS, `iocp` and `rio` on Windows, and in-memory `loopback` for pin-only use
everywhere.

## Policy rules

### What supported promises

A supported row promises three things, and only these three:

1. Every commit's CI includes a leg that builds the library with the compiler
   in the row and runs the unit and functional suite, or the platform-specific
   subset the row describes, on the operating system and architecture in the
   row.
2. A red leg on that platform is a merge blocker. It is not a nightly finding
   that may wait for morning.
3. The platform and the transports in the row are the ones that leg exercises.

It does not promise another compiler, another architecture, another runner
image, privileged XDP attach, line-rate measurements, or a transport the row
does not name. In particular, the per-commit `big-endian` leg cross-compiles
the library and suite for s390x and runs the wire and sodium checks under
qemu-user; the socket-crossing suite runs on s390x only in the nightly
full-system guest. The nightly row is how that suite is claimed.

### Adding a platform

1. Add a leg to the per-commit workflow that builds and runs the suite on it.
   If the platform is not fit for the per-commit budget, add the leg to a
   nightly workflow instead and it becomes nightly, not supported.
2. Add one row to the table in the same change. The row names the operating
   system, architecture, compiler, transports and tier.
3. Run the platform policy check.

A platform is not added by landing code that compiles on it, by adding it to a
comment, or by naming it in a release note. It is added by the CI leg and the
row together.

### Dropping a platform

Remove the CI leg and its row in the same change. A row may not outlive the
leg that proves it. To demote a platform, move its leg from the per-commit
workflow to a nightly workflow and change the row's tier in the same change.
To withdraw support entirely, delete the leg and the row; the unsupported rows
in the table record architectures for which no leg exists.

### A claim with no leg is refused

A supported or nightly row must name a real platform and its transports. An
empty value or `-` is a failed check, not a documentation note.

## Checking the table

From the repository root:

```sh
python3 tools/check_platform_policy.py
```

The checker is standard-library Python. It reads this page and the workflows
that prove each tier.
