# OSS-Fuzz integration (prepared, not submitted)

This directory holds what rocketnet needs to run on
[OSS-Fuzz](https://github.com/google/oss-fuzz), Google's continuous fuzzing
service for open source software. **It is prepared only. Nothing has been
submitted to OSS-Fuzz or any other external service**, and `primary_contact`
in `project.yaml` is empty until Glenn fills it.

The fuzz targets themselves live in [fuzz/](../fuzz); `build.sh` compiles
the development library (ROCKETNET_DEVELOPMENT=1, the published development
keys the targets mint tokens with) from `sdk/` and the vendored `sodium/`,
links every target present in `fuzz/`, zips each target's committed seed
corpus to `<target>_seed_corpus.zip` and copies its dictionary
(`fuzz/dict/<target>.dict`, or the shared `fuzz/rocketnet.dict`).

Linux builds of the platform layer compile the XDP transport
unconditionally, so the image installs libbpf and libxdp; the build embeds
a zero-length XDP object (the transport then refuses at create) and leaves
io_uring out. The base-builder image's Ubuntu release must carry
`libxdp-dev`; if it does not, the XDP compile-out on hosts without a BPF
clang (an approved change) removes the dependency.

## Submitting to OSS-Fuzz (for Glenn, when decided)

1. Fill `primary_contact` in `project.yaml`.
2. Fork https://github.com/google/oss-fuzz and copy this directory to
   `projects/rocketnet/` in the fork.
3. Open a pull request.

## Testing the integration locally

From an oss-fuzz checkout (requires Docker):

    python infra/helper.py build_image rocketnet
    python infra/helper.py build_fuzzers --sanitizer address rocketnet
    python infra/helper.py run_fuzzer rocketnet rocketnet_fuzz_session

## The floating-point floor

OSS-Fuzz builds through `build.sh` with its own `$CFLAGS`/`$CXXFLAGS`, which
never carry `-ffp-contract=off`, so every compile line in `build.sh` adds it.
`tools/check_oss_fuzz_fp_contract.cmake` (the `oss_fuzz_fp_contract` ctest)
fails if a `$CC`/`$CXX` compile line lacks it.
