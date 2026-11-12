#!/usr/bin/env bash
# check_fp_contract.sh: the estate's strict floating-point floor on every
# compiler invocation in the tree. RocketNet builds with -ffp-contract=off
# (GCC/Clang) or /fp:precise (MSVC) so the wire's contraction invariance holds
# everywhere rocketnet code is compiled. This walks the build files that are
# NOT routed through the CMake target (which already carries the floor PRIVATE)
# and fails on any compiler line that omits it.
#
# sdk/xdp/Makefile is exempt: `clang ... -target bpfel|bpfeb -c server_xdp.c`
# compiles a BPF object, which has no floating point at all.
#
# `<compiler> --version` with no other argument is a version query, not a
# compilation, and is skipped.
set -u

# The files read are every tracked build file that can name a compiler,
# derived from `git ls-files`: Makefiles (Makefile, GNUmakefile, *.mk),
# workflows and actions (.github/**.yml), shell (*.sh, scripts/*, any file
# whose shebang names sh or bash) and Python (*.py). A run that reads no
# file fails. Exclusions, each with its reason:
#   sodium/            vendored; built only through the CMake target
#   sdk/xdp/Makefile   BPF object, no floating point (see above)
#   tools/check_fp_contract.sh  this checker; its self-test fixtures name
#                      compilers without the floor on purpose
EXCLUDED_RE='^(sodium/|sdk/xdp/Makefile$|tools/check_fp_contract\.sh$)'

default_files=()
default_files_from_tree()
{
    local listing name head
    if ! listing="$(git ls-files)"; then
        echo "check_fp_contract: git ls-files failed; nothing was checked"
        return 2
    fi
    while IFS= read -r name; do
        [ -n "$name" ] || continue
        [[ "$name" =~ $EXCLUDED_RE ]] && continue
        [ -f "$name" ] && [ ! -L "$name" ] || continue
        case "$name" in
            */Makefile|Makefile|*/GNUmakefile|GNUmakefile|*.mk) default_files+=("$name"); continue ;;
            .github/*.yml|.github/*.yaml) default_files+=("$name"); continue ;;
            *.sh|*.py|scripts/*) default_files+=("$name"); continue ;;
        esac
        head="$(head -c 64 "$name" 2>/dev/null | head -n 1 || true)"
        if [[ "$head" =~ ^\#!.*(/|[[:space:]])(ba)?sh([[:space:]]|$) ]]; then
            default_files+=("$name")
        fi
    done <<< "$listing"
    if [ "${#default_files[@]}" -eq 0 ]; then
        echo "check_fp_contract: no build file found in the tree; nothing was checked"
        return 1
    fi
    return 0
}

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
            *'$(CFLAGS)'*|*'${CFLAGS}'*|*'$CFLAGS '*|*'$CFLAGS') return 0 ;;
        esac
    fi
    if [ "$ok_cxxflags" -eq 1 ]; then
        case "$line" in
            *'$(CXXFLAGS)'*|*'${CXXFLAGS}'*|*'$CXXFLAGS '*|*'$CXXFLAGS') return 0 ;;
        esac
    fi
    if [ "$ok_flags" -eq 1 ]; then
        case "$line" in
            *'$(FLAGS)'*|*'${FLAGS}'*|*'$FLAGS '*|*'$FLAGS') return 0 ;;
        esac
    fi
    return 1
}

# Python: every list literal whose first element names a compiler (a
# variable called compiler, cc, cxx, cl or clang_cl, or a string naming one)
# is an invocation; it must carry -ffp-contract=off or /fp:precise inside the
# list unless it generates no code (a version or target query, -E,
# -fsyntax-only, /Zs, /E, /P). Docstrings and comments are not invocations.
check_python_file()
{
    python3 - "$1" <<'PY'
import io, re, sys, tokenize
path = sys.argv[1]
COMPILER_NAMES = {"compiler", "compiler_c", "compiler_cxx", "cc", "cxx", "cl", "clang_cl"}
COMPILER_STRING = re.compile(r"^(.*/)?([a-z0-9_]+-)*(cc|c\+\+|gcc|g\+\+|clang|clang\+\+|clang-cl|cl)(-[0-9]+)?(\.exe)?$")
NO_CODE = {"--version", "-dumpmachine", "-dumpversion", "-dumpfullversion", "-E", "-fsyntax-only", "/Zs", "/E", "/P",
           "-print-resource-dir", "--print-resource-dir"}
FLOOR = ("-ffp-contract=off", "/fp:precise")
with open(path, encoding="utf-8", errors="replace") as handle:
    source = handle.read()
try:
    tokens = [t for t in tokenize.generate_tokens(io.StringIO(source).readline)
              if t.type not in (tokenize.COMMENT, tokenize.NL, tokenize.INDENT, tokenize.DEDENT)]
except (tokenize.TokenError, SyntaxError) as error:
    print(f"{path}: cannot tokenize: {error}")
    sys.exit(1)

def literal(token):
    try:
        value = eval(token.string)
    except Exception:
        return None
    return value if isinstance(value, str) else None

# statements: token spans between NEWLINE tokens
statements, current = [], []
for token in tokens:
    if token.type in (tokenize.NEWLINE, tokenize.ENDMARKER):
        if current:
            statements.append(current)
        current = []
    else:
        current.append(token)

def strings(span):
    return [s for s in (literal(t) for t in span if t.type == tokenize.STRING) if s is not None]

# a name every assignment (=, +=) of which carries the floor
floored, unfloored = set(), set()
for span in statements:
    if len(span) >= 2 and span[0].type == tokenize.NAME and span[1].type == tokenize.OP and span[1].string in ("=", "+="):
        texts = strings(span)
        if any(any(f in s for f in FLOOR) for s in texts):
            floored.add(span[0].string)
        elif span[1].string == "=":
            unfloored.add(span[0].string)
floored -= unfloored

bad = 0
for span in statements:
    for index, token in enumerate(span):
        if token.type != tokenize.OP or token.string != "[" or index + 1 >= len(span):
            continue
        # a subscript (x[...], f()[...], y[0][...]) is not a list
        if index > 0 and (span[index - 1].type in (tokenize.NAME, tokenize.STRING) or span[index - 1].string in (")", "]")):
            if span[index - 1].type != tokenize.NAME or span[index - 1].string not in ("return", "in", "and", "or", "not", "else", "yield"):
                continue
        first = span[index + 1]
        if first.type == tokenize.NAME:
            if first.string not in COMPILER_NAMES:
                continue
        elif first.type == tokenize.STRING:
            value = literal(first)
            if value is None or not COMPILER_STRING.match(value):
                continue
        else:
            continue
        texts = strings(span)
        names = {t.string for t in span if t.type == tokenize.NAME}
        if any(s in NO_CODE or s.startswith("--print-") or s.startswith("-print-") for s in texts):
            break
        if any(any(f in s for f in FLOOR) for s in texts) or names & floored:
            break
        line = source.splitlines()[first.start[0] - 1].strip()
        print(f"{path}:{first.start[0]}: Python compiler invocation missing -ffp-contract=off or /fp:precise: {line}")
        bad = 1
        break
sys.exit(bad)
PY
}

check_file()
{
    local f="$1"
    local file_bad=0
    [ -f "$f" ] || { echo "check_fp_contract: missing file $f"; return 1; }
    if [[ "$f" == *.py ]]; then
        check_python_file "$f"
        return $?
    fi
    # a flags definition without the floor is reported only when the file
    # also invokes a compiler (FLAGS is an ordinary name in a shell script)
    local definition_reports="" saw_compiler=0


    # Pass 1: check CFLAGS / CXXFLAGS / FLAGS definitions in this file and
    # record which ones are defined with a valid FP floor.
    local ok_cflags=0 ok_cxxflags=0 ok_flags=0
    local bad_cflags=0 bad_cxxflags=0 bad_flags=0
    local lineno=0 buf="" start_line=0 stript

    while IFS= read -r raw || [ -n "$raw" ]; do
        lineno=$((lineno + 1))
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
                definition_reports="$definition_reports$f:$start_line: $varname definition missing -ffp-contract=off or /fp:precise: $buf
"
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
        if [[ "$line" =~ ^-?[[:space:]]*run:[[:space:]]*(.+)$ ]]; then
            line="${BASH_REMATCH[1]}"
            line="${line#"${line%%[![:space:]]*}"}"
        fi
        local cmd="${line%%[[:space:]]*}"
        case "$cmd" in
            -D*) buf=""; continue ;;
        esac
        if is_compiler "$cmd"; then
            saw_compiler=1
            local first="${line%%[|;&]*}"
            read -r -a words <<< "$first"
            local w1="${words[1]:-}"
            w1="${w1%\"}"
            w1="${w1%)}"
            w1="${w1%\"}"
            local w2="${words[2]:-}"
            w2="${w2%\"}"
            w2="${w2%)}"
            w2="${w2%\"}"
            case "$w1" in
                --version|-dumpfullversion|-dumpversion|--print-file-name=*|-print-file-name=*|--print-resource-dir|-print-resource-dir)
                    if [ "${#words[@]}" -eq 2 ] || { [ "${#words[@]}" -eq 3 ] && [[ "$w2" =~ ^[0-9]*\>/dev/null$ ]]; }; then
                        buf=""
                        continue
                    fi
                    ;;
            esac
            if ! has_floor "$buf" "$cmd" "$ok_cflags" "$ok_cxxflags" "$ok_flags"; then
                echo "$f:$start_line: compiler invocation missing -ffp-contract=off, /fp:precise (cl) or -Xclang -ffp-contract=off (clang-cl): $buf"
                file_bad=1
            fi
        fi
        buf=""
    done < "$f"

    if [ "$saw_compiler" -eq 1 ] && [ -n "$definition_reports" ]; then
        printf '%s' "$definition_reports"
        file_bad=1
    fi
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

    # 7. Query flag combined with compilation arguments without floor must fail.
    printf 'clang++ --print-file-name=libclang_rt.fuzzer_osx.a foo.cpp -o foo\n' > "$tmp/case7.sh"
    if check_file "$tmp/case7.sh" >/dev/null 2>&1; then
        echo "check_fp_contract self-test failed: clang++ --print-file-name with compile args passed" >&2
        return 1
    fi

    # 8. Positive controls (including standalone query flags) must pass.
    printf 'FLAGS := -O2 -ffp-contract=off\nall:\n\t$(CXX) $(FLAGS) -c a.cpp\n\t/usr/bin/clang++-18 -ffp-contract=off -c b.cpp\n' > "$tmp/Makefile.good"
    if ! check_file "$tmp/Makefile.good" >/dev/null 2>&1; then
        echo "check_fp_contract self-test failed: valid Makefile.good failed" >&2
        return 1
    fi
    printf '"$candidate/bin/clang++" --print-file-name=libclang_rt.fuzzer_osx.a\nfuzzer_rt=$("$candidate/bin/clang++" --print-file-name=libclang_rt.fuzzer_osx.a 2>/dev/null)\nclang++ --print-resource-dir\n' > "$tmp/queries.good.sh"
    if ! check_file "$tmp/queries.good.sh" >/dev/null 2>&1; then
        echo "check_fp_contract self-test failed: standalone query flags failed" >&2
        return 1
    fi
    if ! check_file "tools/check_fuzz_oracle.py" >/dev/null 2>&1; then
        echo "check_fp_contract self-test failed: tools/check_fuzz_oracle.py failed" >&2
        return 1
    fi

    # 9. Python: a literal compiler list without the floor fails; with it, a
    # query, a subscript and a floored name pass.
    printf 'import subprocess\nsubprocess.run(["clang++", "-O2", "-c", "a.cpp"])\n' > "$tmp/bad_list.py"
    if check_file "$tmp/bad_list.py" >/dev/null 2>&1; then
        echo "check_fp_contract self-test failed: a Python [\"clang++\", ...] without the floor passed" >&2
        return 1
    fi
    printf 'import subprocess\ncompiler = "cc"\ncommand = [compiler] + flags + ["a.c"]\n' > "$tmp/bad_name.py"
    if check_file "$tmp/bad_name.py" >/dev/null 2>&1; then
        echo "check_fp_contract self-test failed: [compiler] + flags with an unknown flags passed" >&2
        return 1
    fi
    printf 'import subprocess\nflags = ["-ffp-contract=off", "-O2"]\nflags += ["-DX=1"]\nsubprocess.run(["clang++", "-O2", "-ffp-contract=off", "a.cpp"])\nsubprocess.run([compiler, "--version"])\ncache[compiler] = 1\ncommand = [compiler] + flags + ["a.c"]\n"""\ngcc -fanalyzer runs over the parser\n"""\n' > "$tmp/good.py"
    if ! check_file "$tmp/good.py" >/dev/null 2>&1; then
        echo "check_fp_contract self-test failed: valid Python invocations failed" >&2
        check_file "$tmp/good.py" >&2
        return 1
    fi

    # 10. A shell FLAGS variable in a file that compiles nothing is not a
    # compiler flags definition.
    printf 'FLAGS="$(cat /sys/class/net/eth0/flags)"\necho "$FLAGS"\n' > "$tmp/not_flags.sh"
    if ! check_file "$tmp/not_flags.sh" >/dev/null 2>&1; then
        echo "check_fp_contract self-test failed: a non-compiler FLAGS variable failed" >&2
        return 1
    fi

    # 11. The tree: the file list comes from git, so a compiler line in any
    # tracked workflow or script is read; a tree with no build file and a
    # git that cannot list both fail.
    local self repo
    self="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")"
    repo="$tmp/repo"
    mkdir -p "$repo/.github/workflows" "$repo/tools"
    git -C "$repo" init -q
    printf 'jobs:\n  a:\n    steps:\n      - run: g++ -O2 -ffp-contract=off a.cpp\n' > "$repo/.github/workflows/ci.yml"
    printf 'jobs:\n  a:\n    steps:\n      - run: g++ -O2 dst.cpp -o dst\n' > "$repo/.github/workflows/nightly-dst.yml"
    printf '#!/usr/bin/env bash\nclang++ -O1 -fsanitize=memory a.cpp\n' > "$repo/tools/run_msan.sh"
    git -C "$repo" add .github tools
    if (cd "$repo" && bash "$self") >/dev/null 2>&1; then
        echo "check_fp_contract self-test failed: a tree with g++ in nightly-dst.yml and tools/run_msan.sh passed" >&2
        return 1
    fi
    if [ "$(cd "$repo" && bash "$self" 2>/dev/null | grep -c 'missing -ffp-contract=off')" != 2 ]; then
        echo "check_fp_contract self-test failed: the tree run did not name both planted lines" >&2
        return 1
    fi
    git -C "$repo" rm -q --cached .github/workflows/nightly-dst.yml tools/run_msan.sh
    if ! (cd "$repo" && bash "$self") >/dev/null 2>&1; then
        echo "check_fp_contract self-test failed: a clean tree failed" >&2
        return 1
    fi
    git -C "$repo" rm -q --cached .github/workflows/ci.yml
    if (cd "$repo" && bash "$self") >/dev/null 2>&1; then
        echo "check_fp_contract self-test failed: a tree with no build file passed" >&2
        return 1
    fi
    if (cd "$repo" && GIT_DIR=/nonexistent bash "$self") >/dev/null 2>&1; then
        echo "check_fp_contract self-test failed: git unable to list passed" >&2
        return 1
    fi

    echo "check_fp_contract self-test: ok (13 negative and 6 positive cases)"
    return 0
}

if [ "${1:-}" = "--self-test" ]; then
    run_selftest
    exit $?
fi

if [ "$#" -gt 0 ]; then
    files=("$@")
else
    default_files_from_tree || exit $?
    files=("${default_files[@]}")
    echo "check_fp_contract: reading ${#files[@]} build files from the tree"
fi

bad=0
for f in "${files[@]}"; do
    if ! check_file "$f"; then
        bad=1
    fi
done

if [ "$bad" -eq 0 ]; then
    echo "ok: every compiler invocation in ${#files[@]} files carries the FP floor"
fi
exit "$bad"
