#!/bin/sh
# check_agents_md.sh: verify AGENTS.md identifiers against sdk/rocketnet.h,
# verify the user-facing guides' identifiers against the SDK and the
# documented environment/example authorities, and compile + run the marked
# AGENTS.md and README.md examples.
set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
AGENTS_MD="$ROOT/AGENTS.md"
README_MD="$ROOT/README.md"
HEADER="$ROOT/sdk/rocketnet.h"
ENVIRONMENT_MD="$ROOT/docs/ENVIRONMENT.md"
GUIDES="$ROOT/README.md $ROOT/USAGE.md $ROOT/KEYS.md $ROOT/docs/GETTING-STARTED.md $ROOT/docs/ERRORS-AND-LOGGING.md"

usage() {
    echo "usage: $0 [identifiers|example|all]" >&2
    exit 2
}

for DOC in "$AGENTS_MD" "$README_MD"; do
    if [ ! -f "$DOC" ]; then
        echo "error: missing $DOC" >&2
        exit 1
    fi
done

if [ ! -f "$HEADER" ]; then
    echo "error: missing $HEADER" >&2
    exit 1
fi

run_identifiers() {
    TEXT="$(awk -F'`' '{ for (i = 1; i <= NF; i++) { if (i % 2 == 1) printf "%s ", $i; else if ($i !~ /^[A-Za-z0-9_.\/-]+\.(cpp|h|md)(:[0-9]+)?$/) printf "%s ", $i } printf "\n" }' "$AGENTS_MD")"
    IDS="$(printf '%s\n' "$TEXT" | grep -o -E '\b(rocketnet_[a-z0-9_]+|ROCKETNET_[A-Z0-9_]+)\b' | sort -u)"
    if [ -z "$IDS" ]; then
        echo "error: no rocketnet identifiers found in $AGENTS_MD" >&2
        exit 1
    fi
    count=0
    missing=0
    for id in $IDS; do
        count=$((count + 1))
        # AGENTS.md names public identifiers from sdk/rocketnet.h; the one
        # other authority it may name is a documented environment variable.
        if ! grep -q -w "$id" "$HEADER" && ! grep -q -w "$id" "$ENVIRONMENT_MD"; then
            echo "missing in sdk/rocketnet.h: $id"
            missing=$((missing + 1))
        fi
    done
    if [ "$missing" -ne 0 ]; then
        echo "error: $missing of $count identifiers missing from sdk/rocketnet.h" >&2
        exit 1
    fi
    echo "checked $count identifiers in AGENTS.md against sdk/rocketnet.h"
}

run_guide_identifiers() {
    # The guides also document internal SDK names, environment variables and
    # example helpers, so a guide identifier passes only when it is found in
    # one of those authorities; an invented name is found in none of them.
    for DOC in $GUIDES; do
        if [ ! -f "$DOC" ]; then
            echo "error: missing $DOC" >&2
            exit 1
        fi
        TEXT="$(awk -F'`' '{ for (i = 1; i <= NF; i++) { if (i % 2 == 1) printf "%s ", $i; else if ($i !~ /^[A-Za-z0-9_.\/-]+\.(cpp|h|md)(:[0-9]+)?$/) printf "%s ", $i } printf "\n" }' "$DOC")"
        IDS="$(printf '%s\n' "$TEXT" | grep -o -E '\b(rocketnet_[a-z0-9_]+|ROCKETNET_[A-Z0-9_]+|rn_[a-z0-9_]+)\b' | sort -u)"
        if [ -z "$IDS" ]; then
            echo "error: no rocketnet identifiers found in $DOC" >&2
            exit 1
        fi
        count=0
        missing=0
        for id in $IDS; do
            count=$((count + 1))
            if ! grep -q -w "$id" "$ROOT"/sdk/*.h "$ROOT"/sdk/*.cpp "$ROOT"/functional/*.cpp "$ENVIRONMENT_MD" "$ROOT/examples/backend/README.md" "$ROOT/examples/shutdown.h" "$ROOT/CMakeLists.txt"; then
                echo "missing in SDK, environment and example authorities: $id (in $DOC)"
                missing=$((missing + 1))
            fi
        done
        if [ "$missing" -ne 0 ]; then
            echo "error: $missing of $count identifiers in $DOC are not named by any authority" >&2
            exit 1
        fi
        echo "checked $count identifiers in $DOC against the SDK, environment and example authorities"
    done
}

run_example() {
    DOC="$1"
    MARKER="$2"
    STEM="$3"
    LIBNAME="${4:-rocketnet}"
    DEFINES="${5:-}"
    BUILD_DIR="$ROOT/build"
    LIB="$BUILD_DIR/lib${LIBNAME}.a"
    if [ ! -f "$LIB" ]; then
        echo "error: missing $LIB (build the library under build/ first)" >&2
        exit 1
    fi
    SRC="$BUILD_DIR/check_${STEM}_example.c"
    BIN="$BUILD_DIR/check_${STEM}_example"
    trap 'rm -f "$SRC" "$BIN"' EXIT INT TERM

    sed -n "/<!-- ${MARKER}:example:begin -->/,/<!-- ${MARKER}:example:end -->/p" "$DOC" \
        | sed '/^<!--/d; /^```/d' > "$SRC"

    if [ ! -s "$SRC" ]; then
        echo "error: failed to extract example block from $DOC" >&2
        exit 1
    fi

    OS="$(uname -s)"
    if [ "$OS" = "Linux" ]; then
        LIBS="-lbpf -lxdp -luring -lstdc++ -lpthread -lm"
    elif [ "$OS" = "Darwin" ]; then
        LIBS="-lc++ -lpthread -lm -framework CoreFoundation -framework SystemConfiguration"
    else
        LIBS="-lstdc++ -lpthread -lm"
    fi

    ${CC:-cc} -Wall -Werror $DEFINES -I"$ROOT/sdk" "$SRC" "$LIB" $LIBS -o "$BIN"
    "$BIN"
    rm -f "$SRC" "$BIN"
    trap - EXIT INT TERM
}

MODE="${1:-all}"
case "$MODE" in
    identifiers)
        run_identifiers
        run_guide_identifiers
        ;;
    example)
        run_example "$AGENTS_MD" check_agents_md agents_md rocketnet_dev -DROCKETNET_DEV=1
        ;;
    all)
        run_identifiers
        run_guide_identifiers
        run_example "$AGENTS_MD" check_agents_md agents_md rocketnet_dev -DROCKETNET_DEV=1
        run_example "$README_MD" check_readme readme
        ;;
    *)
        usage
        ;;
esac
