#!/usr/bin/env bash
# build_msan_libcxx.sh: builds the MemorySanitizer-instrumented C++ standard
# library the MSan gate (tools/run_msan.sh) links, for the clang on PATH, and
# installs it under /opt/msan-libcxx-<clang version>.
#
# MSan needs every object that writes memory instrumented. The system C++
# library (libstdc++) is not: its compiled parts (the red-black tree
# rebalance behind std::map and std::set, string growth) write node links
# and characters the shadow never sees, so the suite's own later reads of
# them are reported. The cause-level fix is an instrumented library, which
# LLVM builds from its own runtimes: libc++ and libc++abi with
# LLVM_USE_SANITIZER=MemoryWithOrigins, shared, at the release of the clang
# that builds the suite (the headers and the compiler move together).
#
# Usage: bash tools/build_msan_libcxx.sh [prefix]
#   prefix defaults to /opt/msan-libcxx-$(clang++ -dumpversion); writing it
#   needs sudo when it is not writable; a prefix whose toolchain-id file
#   names this clang version and this script (the first 12 hex digits of its
#   sha256) is left as it is, and any other is rebuilt. The download is the release's source
#   tarball over HTTPS, checked against the sha256 pinned below; a clang
#   version with no pin here is refused. About ten minutes on a 16-core bench.
#
# The fleet runs this on every Linux runner host through the fleet's
# provisioning; run_msan.sh prints TOOL-MISSING naming this script
# when the prefix for the active clang is absent, and the fuzz-nightly MSan
# job builds into a cached prefix of its own on a runner that lacks it.
#
# bash tools/build_msan_libcxx.sh --sha256 <clang version>
#   prints the pinned sha256 of that release's llvm-project source and exits
#   (1 with no pin): the one place the pins live, which the fuzz-nightly
#   libFuzzer engine step reads.
set -euo pipefail

pinned_sha256() {
    case "$1" in
        18.1.3) echo 2929f62d69dec0379e529eb632c40e15191e36f3bd58c2cb2df0413a0dc48651 ;;
        21.1.8) echo 4633a23617fa31a3ea51242586ea7fb1da7140e426bd62fc164261fe036aa142 ;;
        *) return 1 ;;
    esac
}

if [ "${1:-}" = "--sha256" ]; then
    if [ "$#" -ne 2 ] || ! pinned_sha256 "$2"; then
        echo "build_msan_libcxx: no pinned llvm-project source for clang ${2:-<none>}; add its release sha256 to tools/build_msan_libcxx.sh" >&2
        exit 1
    fi
    exit 0
fi

version=$(clang++ -dumpversion)
prefix=${1:-/opt/msan-libcxx-$version}

if ! sha256=$(pinned_sha256 "$version"); then
    echo "build_msan_libcxx: no pinned llvm-project source for clang $version; add its release sha256 here"
    exit 1
fi

script_sha256=$(sha256sum "${BASH_SOURCE[0]}" | cut -c1-12)
toolchain_id="msan-libcxx-clang-$version-script-$script_sha256"

if [ -f "$prefix/lib/libc++.so" ] && [ -f "$prefix/include/c++/v1/__config_site" ] \
    && [ "$(cat "$prefix/toolchain-id" 2>/dev/null)" = "$toolchain_id" ]; then
    echo "build_msan_libcxx: $prefix already holds $toolchain_id"
    exit 0
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/msan-libcxx.XXXXXX")
trap 'rm -rf "$work"' EXIT

tarball="llvm-project-$version.src.tar.xz"
curl -fsSL -o "$work/$tarball" "https://github.com/llvm/llvm-project/releases/download/llvmorg-$version/$tarball"
echo "$sha256  $work/$tarball" | sha256sum -c -
tar -xJf "$work/$tarball" -C "$work"

# shared libraries (a static libc++ cannot go into the SDK's shared
# libraries: its exception globals are local-exec TLS), and the system
# unwinder (libgcc_s) as every other binary on the host uses
generator="Unix Makefiles"
command -v ninja >/dev/null 2>&1 && generator=Ninja
cmake -G "$generator" -S "$work/llvm-project-$version.src/runtimes" -B "$work/build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DLLVM_ENABLE_RUNTIMES="libcxx;libcxxabi" \
    -DLLVM_USE_SANITIZER=MemoryWithOrigins \
    -DLIBCXX_ENABLE_STATIC=OFF \
    -DLIBCXXABI_ENABLE_STATIC=OFF \
    -DLIBCXXABI_USE_LLVM_UNWINDER=OFF \
    -DLIBCXX_INCLUDE_BENCHMARKS=OFF \
    -DLIBCXX_INCLUDE_TESTS=OFF \
    -DLIBCXXABI_INCLUDE_TESTS=OFF

jobs=$(( $(nproc) / 2 )); [ "$jobs" -ge 1 ] || jobs=1
nice -n 10 cmake --build "$work/build" --parallel "$jobs"

echo "$toolchain_id" > "$work/toolchain-id"
if [ -w "$(dirname "$prefix")" ] || [ -w "$prefix" ]; then
    cmake --install "$work/build"
    cp "$work/toolchain-id" "$prefix/toolchain-id"
else
    sudo cmake --install "$work/build"
    sudo cp "$work/toolchain-id" "$prefix/toolchain-id"
fi
echo "build_msan_libcxx: installed $toolchain_id in $prefix"
