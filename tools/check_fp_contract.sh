#!/usr/bin/env bash
# check_fp_contract.sh: the estate's strict floating-point floor on every
# compiler invocation in the tree. RocketNet builds with -ffp-contract=off
# (GCC/Clang) or /fp:precise (MSVC) so the wire's contraction invariance holds
# everywhere rocketnet code is compiled. This walks the build files that are
# NOT routed through the CMake target (which already carries the floor PUBLIC)
# and fails on any compiler line that omits it.
#
# server_xdp/Makefile is exempt: `clang ... -target bpf -c server_xdp.c`
# compiles a BPF object, which has no floating point at all.
set -u

files="Makefile bench/Makefile .github/workflows/ci.yml .github/workflows/fuzz-nightly.yml .github/workflows/nightly.yml .github/workflows/nightly-soak.yml .github/workflows/release-check.yml"

is_compiler()
{
    local cmd="$1"
    # pull the last dash-delimited component so target-triple prefixes
    # (e.g. s390x-linux-gnu-g++) and paths (/usr/bin/clang++) still match
    local last="${cmd##*-}"
    case "$last" in
        cc|cl|c++|cl.exe|clang|clang++|gcc|g++|'$(CC)'|'$(CXX)') return 0 ;;
    esac
    return 1
}

has_floor()
{
    case "$1" in
        *-ffp-contract=off*|*/fp:precise*|*'$(CFLAGS)'*|*'$(CXXFLAGS)'*) return 0 ;;
    esac
    return 1
}

bad=0
for f in $files; do
    [ -f "$f" ] || continue
    lineno=0
    buf=""
    while IFS= read -r raw || [ -n "$raw" ]; do
        lineno=$((lineno + 1))
        if [ -n "$buf" ]; then
            buf="$buf $raw"
        else
            buf="$raw"
        fi
        case "$buf" in
            *\\) continue ;;
        esac
        # strip leading whitespace; take the command word
        line="${buf#"${buf%%[![:space:]]*}"}"
        cmd="${line%%[[:space:]]*}"
        # cmake -D defines configure the toolchain; the actual compilation is
        # routed through the CMake target, which carries the floor PUBLIC.
        case "$cmd" in
            -D*) buf=""; continue ;;
        esac
        if is_compiler "$cmd"; then
            if ! has_floor "$buf"; then
                echo "$f:$lineno: compiler invocation missing -ffp-contract=off or /fp:precise: $buf"
                bad=1
            fi
        fi
        buf=""
    done < "$f"
done

# the shared CFLAGS/CXXFLAGS definitions themselves must carry the floor, so a
# rule that expands $(CFLAGS) inherits it.
for f in Makefile bench/Makefile; do
    [ -f "$f" ] || continue
    while IFS= read -r line; do
        stript="${line#"${line%%[![:space:]]*}"}"
        case "$stript" in
            CFLAGS=*|CFLAGS?=*|CXXFLAGS=*|CXXFLAGS?=*|override\ CFLAGS=*|override\ CXXFLAGS=*)
                if ! has_floor "$line"; then
                    echo "$f: CFLAGS/CXXFLAGS missing -ffp-contract=off or /fp:precise: $line"
                    bad=1
                fi
                ;;
        esac
    done < "$f"
done

if [ "$bad" -eq 0 ]; then
    echo "ok: every compiler invocation carries the FP floor"
fi
exit "$bad"
