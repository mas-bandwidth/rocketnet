# rocketnet fuzz targets

Developer notes for the libFuzzer targets in this directory: how to build
and run each one, where its corpus and dictionary live, how seeds are made,
how coverage is measured, and how a crash is replayed. Every target links
the development library (`ROCKETNET_DEV=1`). The targets that mint
connect tokens use only the published development keys from
`sdk/rocketnet_test_vectors.h`. No target reads a key from the environment.

## The targets

| target | what it drives | seeds | dictionary |
|---|---|---|---|
| `rocketnet_fuzz` | the stateless wire parsers: prefilter, fragment header and reassembly, base64, config values (`input[0] & 3` picks one) | `corpus/` | `rocketnet.dict` |
| `rocketnet_fuzz_message` | the message record decoder, with oracles on the layer's promises | `corpus_message/` | `rocketnet.dict` |
| `rocketnet_fuzz_connect_token` | the server's token ladder and the client's connect parse, raw and structured halves | `corpus_connect_token/` | `rocketnet.dict` |
| `rocketnet_fuzz_token_private` | the token layouts behind the signature and the sealed block, reached directly | `corpus_token_private/` | `dict/rocketnet_fuzz_token_private.dict` |
| `rocketnet_fuzz_receive_structured` | a real client and server through the handshake into the session, input as a decision stream | `corpus_receive_structured/` | `rocketnet.dict` |
| `rocketnet_fuzz_round_trip` | every wire form encoded then decoded; the values read must equal the values written | `corpus_round_trip/` | `dict/rocketnet_fuzz_round_trip.dict` |
| `rocketnet_fuzz_config` | `rocketnet_config_apply_env` over every env-backed config field | `corpus_config/` | `dict/rocketnet_fuzz_config.dict` |
| `rocketnet_fuzz_server_transport` | the server's real receive loop (`rocketnet_server_transport_update`) and the message server above it | `corpus_server_transport/` | `dict/rocketnet_fuzz_server_transport.dict` |
| `rocketnet_fuzz_client_transport` | the client's real receive loop (`rocketnet_client_transport_update`) and the message client above it | `corpus_client_transport/` | `dict/rocketnet_fuzz_client_transport.dict` |
| `rocketnet_fuzz_session` | a scripted session (send, tick, drop, duplicate, reorder, corrupt, inject) checked against the DST invariants in `../dst_checks.h` | `corpus_session/` | `dict/rocketnet_fuzz_session.dict` |

The input format of each target is described in the comment at the top of
its source file. The last three are the pair targets. They share
`rocketnet_fuzz_pair.h`: a real message server and message client joined by
the loopback transport, with the harness standing on the wire between them.
They run with no sockets, no threads and no clock, and their randomness is
deterministic. The pair is created and connected once per process and then
restored from a saved state for each input, so an input replays the same way
whether it runs alone or after other inputs.
On Linux the restore copies back only the pages the last input wrote: every
block owns whole pages, the pages are write-protected once both states are
saved, and the first write to each page is recorded by a SIGSEGV handler.
This took the transport targets from about 20 to 50-80 inputs a second per
worker. Elsewhere, or if protection cannot be set up, every restore copies
every block.

## Building

### With libFuzzer (Linux)

libFuzzer needs LLVM clang (`clang++`), not g++ or Apple clang. Build the
development library first, then the targets:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build --target rocketnet_dev --parallel 4
make -C fuzz -j 4                 # rocketnet_fuzz, _connect_token, _receive_structured, _message
make -C fuzz compile              # syntax check of those sources on any host
```

`fuzz/Makefile` builds the four original targets with
`-fsanitize=fuzzer,address,undefined`. The others are built the same way, by
hand or as the nightly job (`.github/workflows/fuzz-nightly.yml`) does:

```sh
cd fuzz
clang++ -g -O1 -std=c++17 -ffp-contract=off -DROCKETNET_DEV=1 -I../sodium -I../sdk -I../test \
    -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
    rocketnet_fuzz_session.cpp ../build/librocketnet_dev.a -lbpf -lxdp -luring -lpthread \
    -o rocketnet_fuzz_session
```

The pair targets compile `sdk/rocketnet_server_transport.cpp` and the
loopback driver into themselves, so the same command works for
`rocketnet_fuzz_server_transport` and `rocketnet_fuzz_client_transport`.

On a Mac, point `ROCKETNET_FUZZ_CXX` at an LLVM toolchain that ships the
libFuzzer runtime, for example
`make -C fuzz ROCKETNET_FUZZ_CXX="$HOME/sdk/llvm/bin/clang++"`. On macOS the
Makefile links the CoreFoundation and SystemConfiguration frameworks in
place of the Linux bpf, xdp and io_uring libraries.

### With libFuzzer (Windows)

On Windows the libFuzzer runtime ships with Visual Studio's ClangCL toolset
(the "C++ Clang tools for Windows" component): `clang-cl.exe` under
`VC/Tools/Llvm/x64/bin`, and `clang_rt.fuzzer-x86_64.lib` with the
AddressSanitizer runtime in `lib/windows` of its resource directory
(`clang-cl -print-resource-dir`). Build the development library once with
that toolset, instrumented, on the release C runtime (clang-cl refuses
AddressSanitizer with the debug runtime, `/MDd`), then compile each target
at the tree's warning floor and link it against the library. From Git Bash
at the checkout root:

```sh
cmake -B build-fuzz -T ClangCL -A x64 -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL \
    "-DCMAKE_C_FLAGS=-DWIN32 -D_WINDOWS -fsanitize=address -fsanitize=fuzzer-no-link" \
    "-DCMAKE_CXX_FLAGS=-DWIN32 -D_WINDOWS -EHsc -fsanitize=address -fsanitize=fuzzer-no-link"
cmake --build build-fuzz --config Debug --target rocketnet_dev --parallel 4
CLANG_CL="$(cygpath "$(vswhere -latest -property installationPath)")/VC/Tools/Llvm/x64/bin/clang-cl.exe"
"$CLANG_CL" -nologo -Zi -O1 -std:c++17 -EHsc -utf-8 -fp:precise \
    -W4 -WX -Wshadow -Wconversion -Wsign-conversion -Wcast-qual -Wformat=2 -Wundef -Wdouble-promotion \
    -DWIN32 -D_WINDOWS -DROCKETNET_DEV=1 -fsanitize=fuzzer,address \
    -Isodium -Isdk -Itest fuzz/rocketnet_fuzz_session.cpp build-fuzz/Debug/rocketnet_dev.lib \
    -fuse-ld=lld -Fe:bin/rocketnet_fuzz_session.exe
cp "$("$CLANG_CL" -print-resource-dir)/lib/windows/clang_rt.asan_dynamic-x86_64.dll" bin/
mkdir -p corpus_session
ASAN_OPTIONS=detect_leaks=0 ./bin/rocketnet_fuzz_session.exe \
    -dict=fuzz/dict/rocketnet_fuzz_session.dict -max_total_time=60 corpus_session fuzz/corpus_session
```

The link set is the replay drivers' one: `rocketnet_dev.lib` alone, since
the platform source names `WS2_32`, `IPHLPAPI`, `Qwave` and `winmm` itself.
LeakSanitizer does not exist on Windows (AddressSanitizer there has no leak
checker), so the Windows leg runs with `detect_leaks=0` and leaks are
checked on Linux. The nightly `fuzz-windows` job in
`.github/workflows/fuzz-nightly.yml` runs exactly these commands for every
target in the table above; the netcode differential target stays Linux only.

### Without libFuzzer: the replay drivers

CMake builds a `fuzz_replay_<target>` binary for every target. Each one is
the harness linked with `replay_main.cpp`, a main that calls
`LLVMFuzzerTestOneInput` once per file in argument order. These build with
any compiler (Apple clang, MSVC, g++) and land in `bin/`:

```sh
cmake --build build --target fuzz_replay_rocketnet_fuzz_session --parallel 4
./bin/fuzz_replay_rocketnet_fuzz_session fuzz/corpus_session/*
```

With no file arguments a replay driver runs its standalone mode instead:
`FUZZ_ITERS` pseudo-random inputs (default 200000) of up to 4096 bytes from
a fixed stream seeded by `FUZZ_SEED` (default `0x1234567`). The stream is
fixed, so a crash found this way reproduces with the same two variables.

## Running

Copy the committed corpus out first, so a run never changes the tree:

```sh
mkdir -p /tmp/corpus_session && cp fuzz/corpus_session/* /tmp/corpus_session/
./fuzz/rocketnet_fuzz_session -dict=fuzz/dict/rocketnet_fuzz_session.dict \
    -max_total_time=60 /tmp/corpus_session
```

Targets without their own dictionary use `fuzz/rocketnet.dict`. Leak
checking stays on. The pair targets keep their pair in statics, so what they
hold is reachable, not leaked.

Environment switches that every pair target reads:

| variable | effect |
|---|---|
| `ROCKETNET_FUZZ_CHECK_REPLAY=1` | runs every input twice and requires the same counters both times |
| `ROCKETNET_FUZZ_CHECK_RESTORE=1` | after every restore, requires every block to equal its saved copy byte for byte |
| `ROCKETNET_FUZZ_DUMP_COUNTERS=1` | prints every nonzero counter after each input, to show which branch an input reached |
| `ROCKETNET_FUZZ_SEED_DIR=<dir>` | runs the seed generator into `<dir>` and exits (see below) |
| `ROCKETNET_FUZZ_SEED_FROM=fuzz/corpus` | gives the generator the committed wire corpus to wrap |
| `ROCKETNET_FUZZ_COVERAGE_INPUTS_ONLY=1` | zeroes the coverage counters once setup is done (set by `tools/fuzz_coverage.sh`) |

## Corpora

Each `corpus_*` directory is the committed seed set for one target (see the
table). The files have three origins:

- **generated seeds** with descriptive names (`raw_payload`,
  `sealed_keep_alive`, `every_type_small`, ...), written by the target's own
  seed generator;
- **wire seeds** (`prefilter_pcap_*`, and their `wire_*` and `garbage_*`
  wrappings in the pair corpora), derived from the pcapng captures under
  `tests/pcapng/` by `tools/pcapng_fuzz_seeds.py --write`. `--check` confirms
  the committed set matches the captures;
- **regressions** (`regress_<name>`), minimized findings kept forever (see
  "Replaying a crash").

## Seed generators

`rocketnet_fuzz_server_transport`, `rocketnet_fuzz_client_transport`,
`rocketnet_fuzz_session`, `rocketnet_fuzz_round_trip`,
`rocketnet_fuzz_token_private` and `rocketnet_fuzz_config` each contain a
deterministic seed generator. Set `ROCKETNET_FUZZ_SEED_DIR` and run the
binary (the libFuzzer build or the replay driver) to use it:

```sh
ROCKETNET_FUZZ_SEED_DIR=fuzz/corpus_server_transport ROCKETNET_FUZZ_SEED_FROM=fuzz/corpus \
    ./bin/fuzz_replay_rocketnet_fuzz_server_transport
```

The pair generators record one real handshake and one scripted session
between the deterministic pair, then turn every recorded packet type into
seeds. Each seed is replayed through the target before it is written. The
generator exits 1 and prints `INCOMPLETE` unless every packet type the
target's receive loop reads has a seed that raises that type's counter (or,
for the session target, unless every reliability type is delivered in both
directions). A type with no seed would be a parser the fuzzer never enters,
because random mutation cannot produce a valid AEAD tag. Seeds are
deterministic, so a regenerated corpus differs only when the code under it
changed.

## Coverage

`tools/fuzz_coverage.sh` builds the replay drivers with source-based
coverage, runs each over its committed corpus plus a batch of pseudo-random
inputs, and prints line and function coverage per area of the
untrusted-input surface, per target, and for all targets together:

```sh
CC=clang CXX=clang++ FUZZ_COV_JOBS=4 tools/fuzz_coverage.sh rocketnet_fuzz_session rocketnet_fuzz_server_transport
```

The comment at the top of the script lists its variables (`FUZZ_COV_ITERS`,
`FUZZ_COV_BUILD`, `FUZZ_COV_OUT`, `FUZZ_COV_JOBS`, `FUZZ_COV_SETUP`, `SLOT`,
`LLVM_PROFDATA`, `LLVM_COV`). By default the pair targets count only what
inputs reach, not their one-time create and connect. To get a comparable
before and after number for a change, run it the same way on both commits.
On Linux the script adds `-mllvm -runtime-counter-relocation`, which
continuous-mode profiles need there; without it every area reads 0%. It
also defines `ROCKETNET_FUZZ_COVERAGE_BUILD`, the only build in which the pair
harnesses call the profile runtime (so the replay drivers link on macOS
without it). The script refuses a `build-fuzz-cov/` configured without the
current flags; delete it and rerun.

## Replaying a crash

libFuzzer writes a reproducer such as `crash-<sha1>` into the working
directory. To replay it:

```sh
./fuzz/rocketnet_fuzz_session crash-<sha1>                        # libFuzzer build
./bin/fuzz_replay_rocketnet_fuzz_session crash-<sha1>             # any compiler
ROCKETNET_FUZZ_DUMP_COUNTERS=1 ./bin/fuzz_replay_rocketnet_fuzz_session crash-<sha1>
```

Shrink it with `-minimize_crash=1 -runs=100000 crash-<sha1>`. An oracle
failure prints `fuzz oracle failed: <property>` before it aborts. A crash in
the standalone mode reproduces with the same `FUZZ_ITERS` and `FUZZ_SEED`.

After the fix, keep the finding as a regression. For `rocketnet_fuzz`
(prefilter selector) and `rocketnet_fuzz_message`,
`python3 tools/fuzz_finding_to_pcapng.py <harness> <input> --name <name>`
writes `fuzz/<corpus>/regress_<name>` and `tests/pcapng/regress/<name>.pcapng`,
so the unit tier replays it too. `python3 tools/check_fuzz_regressions.py`
checks that every regression seed has its capture. For any other target,
commit the minimized input to its corpus as `regress_<name>`.

## In CI

- `ci.yml` `fuzz-smoke` replays the committed corpora through the
  `fuzz_replay_*` drivers on every push.
- `fuzz-nightly.yml` builds the libFuzzer targets on Linux, macOS and
  Windows, restores each target's growing corpus from the cache (one cache
  per platform), runs a bounded campaign (`max_total_time`, 900 s by
  default) and uploads any reproducer.

## OSS-Fuzz (prepared, not submitted)

`oss-fuzz/` holds `build.sh`, `Dockerfile` and `project.yaml` for running
these targets on OSS-Fuzz. `build.sh` compiles the development library from
`sdk/` and the vendored `sodium/`, links every target present here, zips
each target's corpus to `<target>_seed_corpus.zip` and copies its
dictionary. Every compile line carries `-ffp-contract=off`, which
`tools/check_oss_fuzz_fp_contract.cmake` enforces (the `oss_fuzz_fp_contract`
CMake target). Nothing has been submitted. `oss-fuzz/README.md` has the
local test steps and the submission steps for when that is decided.
