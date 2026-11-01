#!/usr/bin/env bash
# build_pinned_esbmc.sh: builds the ESBMC the ACK run proof runs on
# (proofs/cbmc ack-run-bound-8 and ack-run-negative-control, nightly on
# studio by .github/workflows/nightly-ack-proof.yml) into one prefix, from
# release source tarballs over HTTPS checked against the sha256 pinned below.
# The proof never takes ESBMC, its solver or its libraries from the host.
#
# What is built, in order, each from its pinned source:
#   LLVM and clang 22.1.6   the C++ frontend ESBMC links (static libraries)
#   Z3 4.13.3               the solver the proof runs (--z3), static
#   Boost 1.89.0            date_time, program_options, iostreams, static
#   ESBMC <version>         with fmt 12.1.0, nlohmann json 3.11.3,
#                           yaml-cpp 0.9.0 and immer 0.9.1 handed to its
#                           FetchContent as verified local trees
# These are the LLVM major and Z3 that ESBMC's own release build uses.
#
# The prefix's toolchain-id file names the build: every version and the first
# 12 hex digits of this script's sha256, so any change to how ESBMC is built
# is a different toolchain and is rebuilt.
#
# Usage: bash tools/build_pinned_esbmc.sh <prefix> <esbmc version>
#   A prefix already holding this toolchain-id is left as it is. A version
#   with no sha256 here is refused. ESBMC is installed at <prefix>/bin/esbmc;
#   its dependencies under <prefix>/deps.
#
# Host needs: a C and C++ compiler (cc, c++), cmake, ninja, python3, curl,
# tar, xz, bzip2, shasum, and about 40 GB free under TMPDIR. On macOS the
# Xcode or command line tools SDK (ESBMC parses against it). A missing tool
# is reported as TOOL-MISSING and the script exits 1 without building.
set -euo pipefail

if [ "$#" -ne 2 ]; then
    echo "usage: build_pinned_esbmc.sh <prefix> <esbmc version>" >&2
    exit 1
fi
prefix=$1
esbmc_version=$2

case "$esbmc_version" in
    8.5.0) esbmc_tag=v8.5; esbmc_sha256=61a240ca75cccbd037292d4921b7da01bf12fef0ae760401d3284a3a8a17cff3 ;;
    *) echo "build_pinned_esbmc: no pinned ESBMC source for $esbmc_version; add its release sha256 here" >&2; exit 1 ;;
esac

llvm_version=22.1.6
llvm_sha256=6e0b376a1f6d9873e7dfb09ae6e04b9c7024400f01733fa4c29be69d5c138bc2
z3_version=4.13.3
z3_sha256=f59c9cf600ea57fb64ffeffbffd0f2d2b896854f339e846f48f069d23bc14ba0
boost_version=1.89.0
boost_sha256=85a33fa22621b4f314f8e85e1a5e2a9363d22e4f4992925d4bb3bc631b5a0c7a
fmt_version=12.1.0
fmt_sha256=ea7de4299689e12b6dddd392f9896f08fb0777ac7168897a244a6d6085043fea
json_version=3.11.3
json_sha256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
yaml_version=0.9.0
yaml_sha256=25cb043240f828a8c51beb830569634bc7ac603978e0f69d6b63558dadefd49a
immer_version=0.9.1
immer_sha256=b18b92ba60ec3186dc36ef671d3c2ae542cbb63eb6dc0e258476c6111a67c971

script_sha256=$(shasum -a 256 "${BASH_SOURCE[0]}" | cut -c1-12)
toolchain_id="pinned-esbmc-$esbmc_version-llvm-$llvm_version-z3-$z3_version-boost-$boost_version-script-$script_sha256"

if [ -x "$prefix/bin/esbmc" ] && [ "$(cat "$prefix/toolchain-id" 2>/dev/null)" = "$toolchain_id" ]; then
    echo "build_pinned_esbmc: $prefix already holds $toolchain_id"
    exit 0
fi

missing=0
for tool in cc c++ cmake ninja python3 curl tar xz bzip2 shasum; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "TOOL-MISSING $tool (any) (missing) on ${RUNNER_NAME:-$(hostname)}"
        missing=1
    fi
done
[ "$missing" -eq 0 ] || exit 1

work=$(mktemp -d "${TMPDIR:-/tmp}/pinned-esbmc.XXXXXX")
trap 'rm -rf "$work"' EXIT

free_kb=$(df -Pk "$work" | awk 'NR == 2 { print $4 }')
if [ "$free_kb" -lt $((40 * 1024 * 1024)) ]; then
    echo "build_pinned_esbmc: $((free_kb / 1024)) MB free under $work; the LLVM build needs about 40 GB" >&2
    exit 1
fi

jobs=$(getconf _NPROCESSORS_ONLN)

# fetch <url> <sha256> <directory>: download, verify, unpack into its own
# empty directory, and print the single top-level directory it holds
fetch() {
    local url=$1 sha=$2 dir=$3 file
    file="$work/$(basename "$url")"
    curl -fsSL -o "$file" "$url"
    echo "$sha  $file" | shasum -a 256 -c - >&2
    mkdir -p "$dir"
    tar -xf "$file" -C "$dir"
    rm -f "$file"
    find "$dir" -mindepth 1 -maxdepth 1 -type d | head -1
}

rm -rf "$prefix"
deps="$prefix/deps"
mkdir -p "$deps"

# LLVM and clang: static libraries, the two targets ESBMC parses for, nothing
# the frontend does not link
llvm_src=$(fetch "https://github.com/llvm/llvm-project/releases/download/llvmorg-$llvm_version/llvm-project-$llvm_version.src.tar.xz" "$llvm_sha256" "$work/llvm")
cmake -G Ninja -S "$llvm_src/llvm" -B "$work/llvm-build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$deps" \
    -DLLVM_ENABLE_PROJECTS=clang \
    -DLLVM_TARGETS_TO_BUILD="AArch64;X86" \
    -DLLVM_ENABLE_ASSERTIONS=OFF \
    -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF \
    -DLLVM_INCLUDE_DOCS=OFF -DCLANG_INCLUDE_TESTS=OFF -DCLANG_INCLUDE_DOCS=OFF \
    -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_LIBXML2=OFF \
    -DLLVM_ENABLE_LIBEDIT=OFF -DLLVM_ENABLE_FFI=OFF -DLLVM_ENABLE_BINDINGS=OFF
ninja -C "$work/llvm-build" -j "$jobs" install
rm -rf "$work/llvm" "$work/llvm-build"

# Z3: the static library and headers
z3_src=$(fetch "https://github.com/Z3Prover/z3/archive/refs/tags/z3-$z3_version.tar.gz" "$z3_sha256" "$work/z3")
cmake -G Ninja -S "$z3_src" -B "$work/z3-build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$deps" \
    -DZ3_BUILD_LIBZ3_SHARED=OFF \
    -DZ3_BUILD_EXECUTABLE=OFF -DZ3_BUILD_TEST_EXECUTABLES=OFF \
    -DZ3_ENABLE_EXAMPLE_TARGETS=OFF -DZ3_BUILD_PYTHON_BINDINGS=OFF \
    -DZ3_BUILD_DOCUMENTATION=OFF -DZ3_INCLUDE_GIT_HASH=OFF -DZ3_INCLUDE_GIT_DESCRIBE=OFF \
    -DPython3_EXECUTABLE="$(command -v python3)"
ninja -C "$work/z3-build" -j "$jobs" install
rm -rf "$work/z3" "$work/z3-build"

# Boost: the three compiled libraries ESBMC requires, static, with no
# compression backends (ESBMC reads goto binaries from memory only)
boost_src=$(fetch "https://archives.boost.io/release/$boost_version/source/boost_${boost_version//./_}.tar.bz2" "$boost_sha256" "$work/boost")
( cd "$boost_src" \
  && ./bootstrap.sh --with-libraries=date_time,program_options,iostreams --prefix="$deps" \
  && ./b2 -j "$jobs" link=static variant=release threading=multi \
       -sNO_ZLIB=1 -sNO_BZIP2=1 -sNO_LZMA=1 -sNO_ZSTD=1 install ) > "$work/boost.log" 2>&1 \
  || { tail -50 "$work/boost.log"; exit 1; }
rm -rf "$work/boost"

# ESBMC: FetchContent takes each library from its verified tree; LLVM and Z3
# come from deps, so nothing is downloaded at configure time
fmt_src=$(fetch "https://github.com/fmtlib/fmt/archive/refs/tags/$fmt_version.tar.gz" "$fmt_sha256" "$work/fmt")
json_src=$(fetch "https://github.com/nlohmann/json/releases/download/v$json_version/json.tar.xz" "$json_sha256" "$work/json")
yaml_src=$(fetch "https://github.com/jbeder/yaml-cpp/archive/refs/tags/yaml-cpp-$yaml_version.tar.gz" "$yaml_sha256" "$work/yaml-cpp")
immer_src=$(fetch "https://github.com/arximboldi/immer/archive/refs/tags/v$immer_version.tar.gz" "$immer_sha256" "$work/immer")
esbmc_src=$(fetch "https://github.com/esbmc/esbmc/archive/refs/tags/$esbmc_tag.tar.gz" "$esbmc_sha256" "$work/esbmc")
cmake -G Ninja -S "$esbmc_src" -B "$work/esbmc-build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DCMAKE_PREFIX_PATH="$deps" \
    -DBUILD_TESTING=OFF -DENABLE_REGRESSION=OFF -DENABLE_WERROR=OFF \
    -DDOWNLOAD_DEPENDENCIES=ON \
    -DFETCHCONTENT_FULLY_DISCONNECTED=ON \
    -DFETCHCONTENT_SOURCE_DIR_FMT="$fmt_src" \
    -DFETCHCONTENT_SOURCE_DIR_JSON="$json_src" \
    -DFETCHCONTENT_SOURCE_DIR_YAML-CPP="$yaml_src" \
    -DFETCHCONTENT_SOURCE_DIR_IMMER="$immer_src" \
    -DLLVM_DIR="$deps/lib/cmake/llvm" -DClang_DIR="$deps/lib/cmake/clang" \
    -DZ3_DIR="$deps" -DENABLE_Z3=ON \
    -DBoost_ROOT="$deps" -DBoost_USE_STATIC_LIBS=ON \
    -DENABLE_SMTLIB=OFF -DENABLE_GOTO_CONTRACTOR=OFF -DENABLE_MIMALLOC=OFF \
    -DENABLE_BITWUZLLOB=OFF -DENABLE_NEUROSYM=OFF \
    -DENABLE_PYTHON_FRONTEND=OFF -DENABLE_SOLIDITY_FRONTEND=OFF -DENABLE_JIMPLE_FRONTEND=OFF \
    -DPython_EXECUTABLE="$(command -v python3)"
ninja -C "$work/esbmc-build" -j "$jobs" install

got=$("$prefix/bin/esbmc" --version 2>&1 | head -1)
case "$got" in
    *"$esbmc_version"*) ;;
    *) echo "build_pinned_esbmc: built esbmc reports '$got', not $esbmc_version" >&2; exit 1 ;;
esac
echo "$toolchain_id" > "$prefix/toolchain-id"
echo "build_pinned_esbmc: $prefix holds $toolchain_id ($got)"
