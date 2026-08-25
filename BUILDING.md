# Building RocketNet

RocketNet builds on Linux and macOS with CMake, driven by the Makefile:

    make

The library, tests and examples land in `dist/`. Run the test suite with:

    make test

Windows joins when the Windows platform leg is exercised for real.

## Floating point: RocketNet builds with -ffp-contract=off

Estate policy for mas-bandwidth network libraries: builds are strict about floating point
contraction. This build sets the right flag on every target, so nothing is required of you to
build RocketNet itself:

  - GCC/Clang: `-ffp-contract=off`. Not merely the absence of `-ffast-math` — GCC's default
    is `-ffp-contract=fast`, which contracts across statement boundaries, and clang's default
    `=on` still fuses within a single expression.
  - MSVC: `/fp:precise`.

RocketNet's floats are connection config and stats, so no flag is required of consumers for
correct wire bytes today. The strict build is the family floor: contraction is
architecture-dependent (FMA is in the aarch64 baseline and absent from the x86-64 one), so
float arithmetic near a wire diverges bit-wise between architectures without it. If you
compile RocketNet's sources into your own build rather than linking the library, carry the
same flag.
