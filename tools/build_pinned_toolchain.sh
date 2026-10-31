#!/usr/bin/env bash
# build_pinned_toolchain.sh: builds the toolchain the instruction count gate
# measures with (benchmarks/instruction_count.md) into one prefix: gcc and
# valgrind at the versions benchmarks/instruction_baseline.json names, from
# release source tarballs over HTTPS checked against the sha256 pinned below.
#
# Instruction counts are only comparable under the exact compiler and
# valgrind the baseline was measured with, so the gate never takes either
# from the host: it builds them here, caches the prefix keyed by the pins,
# and runs the measurement with explicit paths into it.
#
# Usage: bash tools/build_pinned_toolchain.sh <prefix> <gcc version> <valgrind version>
#   A prefix already holding both pinned versions is left as it is. A version
#   with no sha256 here is refused. gcc's in-tree prerequisites (gmp, mpfr,
#   mpc) are fetched from gcc.gnu.org over HTTPS and checked against the
#   sha512 list shipped inside the verified gcc tarball.
#
# Host needs: a C and C++ compiler (cc, c++), make, m4, curl, tar, xz, bzip2,
# sha256sum, sha512sum, and about 8 GB free under TMPDIR. A missing tool is
# reported as TOOL-MISSING and the script exits 1 without building.
set -euo pipefail

if [ "$#" -ne 3 ]; then
    echo "usage: build_pinned_toolchain.sh <prefix> <gcc version> <valgrind version>" >&2
    exit 1
fi
prefix=$1
gcc_version=$2
valgrind_version=$3

case "$gcc_version" in
    15.2.0) gcc_sha256=438fd996826b0c82485a29da03a72d71d6e3541a83ec702df4271f6fe025d24e ;;
    *) echo "build_pinned_toolchain: no pinned gcc source for $gcc_version; add its release sha256 here" >&2; exit 1 ;;
esac
case "$valgrind_version" in
    3.26.0) valgrind_sha256=8d54c717029106f1644aadaf802ab9692e53d93dd015cbd19e74190eba616bd7 ;;
    *) echo "build_pinned_toolchain: no pinned valgrind source for $valgrind_version; add its release sha256 here" >&2; exit 1 ;;
esac

if [ -x "$prefix/bin/g++" ] && [ -x "$prefix/bin/valgrind" ] \
    && [ "$("$prefix/bin/g++" -dumpfullversion)" = "$gcc_version" ] \
    && [ "$(VALGRIND_LIB="$prefix/libexec/valgrind" "$prefix/bin/valgrind" --version)" = "valgrind-$valgrind_version" ]; then
    echo "build_pinned_toolchain: $prefix already holds gcc $gcc_version and valgrind $valgrind_version"
    exit 0
fi

missing=0
for tool in cc c++ make m4 curl tar xz bzip2 sha256sum sha512sum; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "TOOL-MISSING $tool (any) (missing) on ${RUNNER_NAME:-$(hostname)}"
        missing=1
    fi
done
[ "$missing" -eq 0 ] || exit 1

work=$(mktemp -d "${TMPDIR:-/tmp}/pinned-toolchain.XXXXXX")
trap 'rm -rf "$work"' EXIT

free_kb=$(df -Pk "$work" | awk 'NR == 2 { print $4 }')
if [ "$free_kb" -lt $((8 * 1024 * 1024)) ]; then
    echo "build_pinned_toolchain: $((free_kb / 1024)) MB free under $work; the gcc build needs about 8 GB" >&2
    exit 1
fi

jobs=$(nproc)

# gcc: C and C++ only, one stage (the host compiler builds it directly), no
# multilib, no sanitizer or OpenMP runtimes the gate does not link
gcc_tarball="gcc-$gcc_version.tar.xz"
curl -fsSL -o "$work/$gcc_tarball" "https://ftp.gnu.org/gnu/gcc/gcc-$gcc_version/$gcc_tarball"
echo "$gcc_sha256  $work/$gcc_tarball" | sha256sum -c -
tar -xJf "$work/$gcc_tarball" -C "$work"
gcc_src="$work/gcc-$gcc_version"
for lib in gmp mpfr mpc; do
    line=$(grep -E "^[0-9a-f]{128}  $lib-[0-9.]+\.tar\.(bz2|gz|xz)$" "$gcc_src/contrib/prerequisites.sha512")
    archive=${line##* }
    curl -fsSL -o "$work/$archive" "https://gcc.gnu.org/pub/gcc/infrastructure/$archive"
    (cd "$work" && echo "$line" | sha512sum -c -)
    tar -xf "$work/$archive" -C "$gcc_src"
    ln -s "${archive%.tar.*}" "$gcc_src/$lib"
done
mkdir "$work/gcc-build"
(
    cd "$work/gcc-build"
    "$gcc_src/configure" \
        --prefix="$prefix" \
        --enable-languages=c,c++ \
        --disable-bootstrap \
        --disable-multilib \
        --disable-nls \
        --enable-default-pie \
        --without-isl \
        --disable-libsanitizer \
        --disable-libgomp \
        --disable-libitm \
        --disable-libquadmath \
        --disable-libvtv
    make -j"$jobs"
    make install-strip
)
rm -rf "$work/gcc-build" "$gcc_src"

valgrind_tarball="valgrind-$valgrind_version.tar.bz2"
curl -fsSL -o "$work/$valgrind_tarball" "https://sourceware.org/pub/valgrind/$valgrind_tarball"
echo "$valgrind_sha256  $work/$valgrind_tarball" | sha256sum -c -
tar -xjf "$work/$valgrind_tarball" -C "$work"
(
    cd "$work/valgrind-$valgrind_version"
    ./configure --prefix="$prefix"
    make -j"$jobs"
    make install
)

"$prefix/bin/g++" --version | head -1
VALGRIND_LIB="$prefix/libexec/valgrind" "$prefix/bin/valgrind" --version
