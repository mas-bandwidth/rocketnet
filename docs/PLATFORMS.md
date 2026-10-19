# Supported platforms

RocketNet's support claim is a claim about what every commit builds and tests
across operating systems, architectures, compilers, and transports. A platform
is **supported** when per-commit verification builds the library and runs the
unit and functional test suites on it for every pull request and every push to
`main`. The supported platforms are Linux x86_64, macOS arm64 and Windows
x86_64. s390x is a **byte-order check**: verification builds and checks the
library there to catch an endianness break, and it is not a supported
deployment platform. A platform with no verification leg is **not tested**,
even if the source compiles there.

## Platform matrix

| Operating system | Architecture | Compiler | Transports | Tier |
| --- | --- | --- | --- | --- |
| Ubuntu 24.04 (Linux) | x86_64 | GCC 13, Clang | portable, gso, io_uring, xdp, loopback | supported |
| macOS 15 | arm64 | Apple Clang | portable, msgx, loopback | supported |
| Windows Server 2025 | x86_64 | MSVC | portable, iocp, rio, loopback | supported |
| Ubuntu 24.04 (Linux, cross/QEMU) | s390x | s390x-linux-gnu-gcc | none (in-memory wire check only) | byte-order check, every commit |
| Ubuntu 24.04 (Linux, full-system guest) | s390x | GCC | portable, loopback | byte-order check, nightly |
| Linux | arm64 (aarch64) | none | none | not tested |
| Windows | arm64 | none | none | not tested |
| macOS | x86_64 | none | none | not tested |

The **Transports** column names the transports built and exercised on that
platform. The built-in transport registry (`rocketnet_drivers.inc`) provides:

- `portable` and `loopback` on every supported platform,
- `gso`, `io_uring`, and `xdp` on Linux,
- `msgx` on macOS,
- `iocp` and `rio` on Windows.

## Policy rules

### What supported promises

A supported row promises three things:

1. Every commit builds the library with the compiler in the row and runs the
   unit and functional suites (or the documented platform subset) on the
   operating system and architecture in the row.
2. A failure on that platform blocks merging.
3. Every listed transport for that platform is compiled and exercised by its
   unit and functional tests.

One limit on the Linux row: verification runs unprivileged, so the `xdp`
transport is compiled and its unprivileged checks run, but its privileged path
(BPF program load and attach, AF_XDP binds) is not exercised by verification;
see [BUILDING.md](../BUILDING.md).

The s390x byte-order check: every commit cross-compiles the library and suite
for `s390x` and runs the wire and cryptography checks under `qemu-user`; the
full socket-crossing suite runs on `s390x` nightly in a full-system guest.

### Adding or dropping a platform

A platform is added as supported by enabling its per-commit verification leg
and adding its row to the table in the same change. To drop or demote a platform,
update or remove its verification leg and its table row together.
