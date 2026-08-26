# Building rocketnet

rocketnet builds on Linux and macOS with CMake, driven by the Makefile:

    make

Binaries and libraries land in `dist/`. Run the test suite with:

    ./dist/test

On Linux the build links the XDP dataplane, so it needs the dev packages CI installs:

    sudo apt install cmake build-essential libbpf-dev libxdp-dev libcurl4-openssl-dev xxd

The embedded XDP program (`server_xdp/`) is packed as a source tarball by the build and
compiles later on the target kernel, which needs BTF; it is not compiled by `make` here.

## Floating point: rocketnet builds with -ffp-contract=off

Estate policy for mas-bandwidth network libraries: builds are strict about floating point
contraction. This build sets the right flag on every target, so nothing is required of you to
build rocketnet itself:

  - GCC/Clang: `-ffp-contract=off`. Not merely the absence of `-ffast-math` — GCC's default
    is `-ffp-contract=fast`, which contracts across statement boundaries, and clang's default
    `=on` still fuses within a single expression.
  - MSVC: `/fp:precise`.

rocketnet's float wire encoding is a bit copy (no arithmetic), so no flag is required of consumers
for correct wire bytes today. The strict build is the family floor: contraction is
architecture-dependent (FMA is in the aarch64 baseline and absent from the x86-64 one), so
float arithmetic near a wire diverges bit-wise between architectures without it. If you
compile rocketnet's sources into your own build rather than linking the library, carry the same
flag.
