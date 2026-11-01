#!/usr/bin/env bash
# check_fp_contract.sh: the estate's strict floating-point floor on every
# compiler invocation in the tree. RocketNet builds with -ffp-contract=off
# (GCC/Clang) or /fp:precise (MSVC) so the wire's contraction invariance holds
# everywhere rocketnet code is compiled. This walks the build files that are
# NOT routed through the CMake target (which already carries the floor PRIVATE)
# and fails on any compiler line that omits it.
#
# sdk/xdp/Makefile is exempt: `clang ... -target bpf -c server_xdp.c`
# compiles a BPF object, which has no floating point at all.
#
# `<compiler> --version` with no other argument is a version query, not a
# compilation, and is skipped.
set -u

default_files=(
    Makefile
    benchmarks/Makefile
    tests/fuzz/Makefile
    tools/fuzz_ndebug.sh
    tools/check_fuzz_oracle.py
    .github/workflows/ci.yml
    .github/workflows/fuzz-nightly.yml
    .github/workflows/nightly.yml
    .github/workflows/nightly-soak.yml
    .github/workflows/release-check.yml
)

# clang-cl, by name or through a variable that names it
# ("$ROCKETNET_FUZZ_CLANG_CL", "$CLANG_CL").
is_clang_cl()
{
    case "$1" in
        *clang-cl*|*CLANG_CL*) return 0 ;;
    esac
    return 1
}

is_compiler()
{
    local cmd="$1"
    cmd="${cmd#\"}"
    cmd="${cmd%\"}"
    cmd="${cmd#\'}"
    cmd="${cmd%\'}"
    is_clang_cl "$cmd" && return 0
    case "$cmd" in
        '$(CC)'|'$(CXX)'|'${CC}'|'${CXX}'|'$CC'|'$CXX') return 0 ;;
    esac
    # 1. Strip any leading directory path (/usr/bin/clang++-18 -> clang++-18).
    local base="${cmd##*/}"
    # 2. Strip an optional trailing -[0-9]+ version suffix (clang++-18 -> clang++).
    if [[ "$base" =~ ^(.+)-[0-9]+(\.exe)?$ ]]; then
        base="${BASH_REMATCH[1]}${BASH_REMATCH[2]}"
    fi
    # 3. Pull the last dash-delimited component so target-triple prefixes
    # (e.g. s390x-linux-gnu-g++) still match.
    local last="${base##*-}"
    case "$last" in
        cc|cl|c++|cl.exe|clang|clang++|gcc|g++) return 0 ;;
    esac
    return 1
}

has_literal_floor()
{
    if is_clang_cl "${2:-}"; then
        case "$1" in
            *'-Xclang -ffp-contract=off'*) return 0 ;;
        esac
        return 1
    fi
    case "$1" in
        *-ffp-contract=off*|*/fp:precise*) return 0 ;;
    esac
    return 1
}

# $1 is the line, $2 its command word, $3/$4/$5 whether CFLAGS/CXXFLAGS/FLAGS
# carry a verified floor in the current file.
has_floor()
{
    local line="$1"
    local cmd="${2:-}"
    local ok_cflags="${3:-0}"
    local ok_cxxflags="${4:-0}"
    local ok_flags="${5:-0}"

    if has_literal_floor "$line" "$cmd"; then
        return 0
    fi
    if is_clang_cl "$cmd"; then
        return 1
    fi
    if [ "$ok_cflags" -eq 1 ]; then
        case "$line" in
            *'$(CFLAGS)'*|*'${CFLAGS}'*) return 0 ;;
        esac
    fi
    if [ "$ok_cxxflags" -eq 1 ]; then
        case "$line" in
            *'$(CXXFLAGS)'*|*'${CXXFLAGS}'*) return 0 ;;
        esac
    fi
    if [ "$ok_flags" -eq 1 ]; then
        case "$line" in
            *'$(FLAGS)'*|*'${FLAGS}'*) return 0 ;;
        esac
    fi
    return 1
}

check_file()
{
    local f="$1"
    local file_bad=0
    [ -f "$f" ] || { echo "check_fp_contract: missing file $f"; return 1; }

    # Pass 1: check CFLAGS / CXXFLAGS / FLAGS definitions in this file and
    # record which ones are defined with a valid FP floor. Also check Python
    # compiler-flag lists (`flags = [compiler, ...]`).
    local ok_cflags=0 ok_cxxflags=0 ok_flags=0
    local bad_cflags=0 bad_cxxflags=0 bad_flags=0
    local ok_py_flags=0 bad_py_flags=0
    local lineno=0 buf="" start_line=0 in_py_flags=0 py_buf="" py_start=0

    while IFS= read -r raw || [ -n "$raw" ]; do
        lineno=$((lineno + 1))
        if [ "$in_py_flags" -eq 1 ]; then
            py_buf="$py_buf $raw"
            if [[ "$raw" == *"]"* ]]; then
                in_py_flags=0
                if has_literal_floor "$py_buf" "clang++"; then
                    [ "$bad_py_flags" -eq 0 ] && ok_py_flags=1
                else
                    echo "$f:$py_start: Python compiler flags missing -ffp-contract=off: $py_buf"
                    file_bad=1
                    bad_py_flags=1
                    ok_py_flags=0
                fi
            fi
            continue
        fi
        local stript="${raw#"${raw%%[![:space:]]*}"}"
        if [[ "$f" == *.py && "$stript" =~ ^flags[[:space:]]*=[[:space:]]*\[compiler, ]]; then
            py_buf="$stript"
            py_start=$lineno
            if [[ "$stript" == *"]"* ]]; then
                if has_literal_floor "$py_buf" "clang++"; then
                    [ "$bad_py_flags" -eq 0 ] && ok_py_flags=1
                else
                    echo "$f:$py_start: Python compiler flags missing -ffp-contract=off: $py_buf"
                    file_bad=1
                    bad_py_flags=1
                    ok_py_flags=0
                fi
            else
                in_py_flags=1
            fi
            continue
        fi

        if [ -n "$buf" ]; then
            buf="${buf%\\} $raw"
        else
            buf="$raw"
            start_line=$lineno
        fi
        case "$buf" in
            *\\) continue ;;
        esac
        stript="${buf#"${buf%%[![:space:]]*}"}"
        case "$stript" in
            ''|'#'*) buf=""; continue ;;
        esac
        if [[ "$stript" =~ ^(override[[:space:]]+|export[[:space:]]+)?(CFLAGS|CXXFLAGS|FLAGS)[[:space:]]*(::=|:=|\?=|\+=|=)(.*)$ ]]; then
            local varname="${BASH_REMATCH[2]}"
            if has_floor "$buf" "" "$ok_cflags" "$ok_cxxflags" "$ok_flags"; then
                case "$varname" in
                    CFLAGS)   [ "$bad_cflags" -eq 0 ] && ok_cflags=1 ;;
                    CXXFLAGS) [ "$bad_cxxflags" -eq 0 ] && ok_cxxflags=1 ;;
                    FLAGS)    [ "$bad_flags" -eq 0 ] && ok_flags=1 ;;
                esac
            else
                echo "$f:$start_line: $varname definition missing -ffp-contract=off or /fp:precise: $buf"
                file_bad=1
                case "$varname" in
                    CFLAGS)   bad_cflags=1; ok_cflags=0 ;;
                    CXXFLAGS) bad_cxxflags=1; ok_cxxflags=0 ;;
                    FLAGS)    bad_flags=1; ok_flags=0 ;;
                esac
            fi
        fi
        buf=""
    done < "$f"

    # Pass 2: check every compiler invocation in this file.
    lineno=0
    buf=""
    start_line=0
    while IFS= read -r raw || [ -n "$raw" ]; do
        lineno=$((lineno + 1))
        if [ -n "$buf" ]; then
            buf="$buf $raw"
        else
            buf="$raw"
            start_line=$lineno
        fi
        case "$buf" in
            *\\) continue ;;
        esac
        local line="${buf#"${buf%%[![:space:]]*}"}"
        case "$line" in
            ''|'#'*) buf=""; continue ;;
        esac
        if [[ "$f" == *.py && "$line" =~ ^subprocess\.(run|check_call|check_output)\((flags[[:space:]]*\+|\[[[:space:]]*compiler,) ]]; then
            if [[ "$line" == *"flags"* ]]; then
                if [ "$ok_py_flags" -ne 1 ] && ! has_literal_floor "$line" "clang++"; then
                    echo "$f:$start_line: Python compiler invocation without verified -ffp-contract=off: $line"
                    file_bad=1
                fi
            elif ! has_literal_floor "$line" "clang++"; then
                echo "$f:$start_line: Python compiler invocation missing -ffp-contract=off: $line"
                file_bad=1
            fi
            buf=""
            continue
        fi
        if [[ "$line" =~ ^-?[[:space:]]*run:[[:space:]]*(.+)$ ]]; then
            line="${BASH_REMATCH[1]}"
            line="${line#"${line%%[![:space:]]*}"}"
        fi
        local cmd="${line%%[[:space:]]*}"
        case "$cmd" in
            -D*) buf=""; continue ;;
        esac
        if is_compiler "$cmd"; then
            local first="${line%%[|;&]*}"
            read -r -a words <<< "$first"
            local w1="${words[1]:-}"
            w1="${w1%\"}"
            w1="${w1%)}"
            w1="${w1%\"}"
            if [ "${#words[@]}" -eq 2 ] && { [ "$w1" = "--version" ] || [ "$w1" = "-dumpfullversion" ] || [ "$w1" = "-dumpversion" ]; }; then
                buf=""
                continue
            fi
            if ! has_floor "$buf" "$cmd" "$ok_cflags" "$ok_cxxflags" "$ok_flags"; then
                echo "$f:$start_line: compiler invocation missing -ffp-contract=off, /fp:precise (cl) or -Xclang -ffp-contract=off (clang-cl): $buf"
                file_bad=1
            fi
        fi
        buf=""
    done < "$f"

    return "$file_bad"
}

run_selftest()
{
    local tmp
    tmp="$(mktemp -d)"
    trap 'rm -rf "$tmp"' RETURN

    # 1. Plain versioned compiler without floor must fail.
    printf 'clang-18 -c a.c\n' > "$tmp/case1.sh"
    if check_file "$tmp/case1.sh" >/dev/null 2>&1; then
        echo "check_fp_contract self-test failed: clang-18 -c a.c passed" >&2
        return 1
    fi

    # 2. Inline YAML run: compiler without floor must fail.
    printf '      - name: Compile\n        run: clang++ -c a.cpp\n' > "$tmp/case2.yml"
    if check_file "$tmp/case2.yml" >/dev/null 2>&1; then
        echo "check_fp_contract self-test failed: inline run: clang++ -c a.cpp passed" >&2
        return 1
    fi

    # 3. Makefile with FLAGS := -O2 (missing floor) and $(CXX) $(FLAGS) must fail.
    printf 'FLAGS := -O2\nall:\n\t$(CXX) $(FLAGS) -c a.cpp\n' > "$tmp/Makefile.bad_flags"
    if check_file "$tmp/Makefile.bad_flags" >/dev/null 2>&1; then
        echo "check_fp_contract self-test failed: FLAGS := -O2 Makefile passed" >&2
        return 1
    fi

    # 4. Makefile with unverified $(FLAGS) (no definition in file) must fail.
    printf 'all:\n\t$(CXX) $(FLAGS) -c a.cpp\n' > "$tmp/Makefile.undef_flags"
    if check_file "$tmp/Makefile.undef_flags" >/dev/null 2>&1; then
        echo "check_fp_contract self-test failed: undefined $(FLAGS) Makefile passed" >&2
        return 1
    fi

    # 5. Absolute-path versioned compiler (/usr/bin/clang++-18 -c a.cpp) must fail.
    printf '/usr/bin/clang++-18 -c a.cpp\n' > "$tmp/case5.sh"
    if check_file "$tmp/case5.sh" >/dev/null 2>&1; then
        echo "check_fp_contract self-test failed: /usr/bin/clang++-18 -c a.cpp passed" >&2
        return 1
    fi

    # 6. tools/check_fuzz_oracle.py with -ffp-contract=off stripped must fail.
    sed "s/'-ffp-contract=off', //g; s/'-ffp-contract=off'//g" tools/check_fuzz_oracle.py > "$tmp/bad_oracle.py"
    if check_file "$tmp/bad_oracle.py" >/dev/null 2>&1; then
        echo "check_fp_contract self-test failed: check_fuzz_oracle.py without -ffp-contract=off passed" >&2
        return 1
    fi

    # 7. Positive controls must pass.
    printf 'FLAGS := -O2 -ffp-contract=off\nall:\n\t$(CXX) $(FLAGS) -c a.cpp\n\t/usr/bin/clang++-18 -ffp-contract=off -c b.cpp\n' > "$tmp/Makefile.good"
    if ! check_file "$tmp/Makefile.good" >/dev/null 2>&1; then
        echo "check_fp_contract self-test failed: valid Makefile.good failed" >&2
        return 1
    fi
    if ! check_file "tools/check_fuzz_oracle.py" >/dev/null 2>&1; then
        echo "check_fp_contract self-test failed: tools/check_fuzz_oracle.py failed" >&2
        return 1
    fi

    echo "check_fp_contract self-test: ok (6 negative and 2 positive cases)"
    return 0
}

if [ "${1:-}" = "--self-test" ]; then
    run_selftest
    exit $?
fi

if [ "$#" -gt 0 ]; then
    files=("$@")
else
    files=("${default_files[@]}")
fi

bad=0
for f in "${files[@]}"; do
    if ! check_file "$f"; then
        bad=1
    fi
done

if [ "$bad" -eq 0 ]; then
    echo "ok: every compiler invocation carries the FP floor"
fi
exit "$bad"
