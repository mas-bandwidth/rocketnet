#!/usr/bin/env bash
# check_fp_contract.sh: the estate's strict floating-point floor on every
# compiler invocation in the tree. RocketNet builds with -ffp-contract=off
# (GCC/Clang) or /fp:precise (MSVC) so the wire's contraction invariance holds
# everywhere rocketnet code is compiled. This walks the build files that are
# NOT routed through the CMake target (which already carries the floor PRIVATE)
# and fails on any compiler line that omits it.
#
# server_xdp/Makefile is exempt: `clang ... -target bpf -c server_xdp.c`
# compiles a BPF object, which has no floating point at all.
#
# `<compiler> --version` with no other argument is a version query, not a
# compilation, and is skipped.
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
        # routed through the CMake target, which carries the floor PRIVATE.
        case "$cmd" in
            -D*) buf=""; continue ;;
        esac
        if is_compiler "$cmd"; then
            # `clang --version` (alone, optionally piped) prints the version and
            # compiles nothing, so it has no floor to carry. Only the exact
            # two-word form is exempt: any further argument is a compile line.
            first="${line%%[|;&]*}"
            read -r -a words <<< "$first"
            if [ "${#words[@]}" -eq 2 ] && [ "${words[1]}" = "--version" ]; then
                buf=""
                continue
            fi
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
        # a variable definition is NAME followed by optional whitespace then one
        # of =, ?=, +=. GNU make allows whitespace before the operator
        # (`CFLAGS  ?= ...`), so match it explicitly; the old case patterns only
        # matched the operator flush against the name and let the spaced form
        # through unexamined.
        if [[ "$stript" =~ ^(override[[:space:]]+)?(CFLAGS|CXXFLAGS)[[:space:]]*(\?=|\+=|=) ]]; then
            if ! has_floor "$line"; then
                echo "$f: CFLAGS/CXXFLAGS missing -ffp-contract=off or /fp:precise: $line"
                bad=1
            fi
        fi
    done < "$f"
done

if [ "$bad" -eq 0 ]; then
    echo "ok: every compiler invocation carries the FP floor"
fi
exit "$bad"
