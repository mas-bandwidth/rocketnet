#!/bin/sh
# check_agents_md.sh: verify AGENTS.md identifiers against sdk/rocketnet.h
# and compile + run its marked loopback integration example.
set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
AGENTS_MD="$ROOT/AGENTS.md"
HEADER="$ROOT/sdk/rocketnet.h"

usage() {
    echo "usage: $0 {identifiers|example}" >&2
    exit 2
}

if [ "$#" -ne 1 ]; then
    usage
fi

if [ ! -f "$AGENTS_MD" ]; then
    echo "error: missing $AGENTS_MD" >&2
    exit 1
fi

if [ ! -f "$HEADER" ]; then
    echo "error: missing $HEADER" >&2
    exit 1
fi

case "$1" in
    identifiers)
        TEXT="$(awk -F'`' '{ for (i = 1; i <= NF; i++) { if (i % 2 == 1) printf "%s ", $i; else if ($i !~ /\// && $i !~ /\.(cpp|h)(:[0-9]+)?$/) printf "%s ", $i } printf "\n" }' "$AGENTS_MD")"
        IDS="$(printf '%s\n' "$TEXT" | grep -o -E '\b(rocketnet_[a-z0-9_]+|ROCKETNET_[A-Z0-9_]+)\b' | sort -u)"
        if [ -z "$IDS" ]; then
            echo "error: no rocketnet identifiers found in $AGENTS_MD" >&2
            exit 1
        fi
        count=0
        missing=0
        for id in $IDS; do
            count=$((count + 1))
            if ! grep -q -w "$id" "$HEADER"; then
                echo "missing in sdk/rocketnet.h: $id"
                missing=$((missing + 1))
            fi
        done
        if [ "$missing" -ne 0 ]; then
            echo "error: $missing of $count identifiers missing from sdk/rocketnet.h" >&2
            exit 1
        fi
        echo "checked $count identifiers in AGENTS.md against sdk/rocketnet.h"
        ;;
    example)
        BUILD_DIR="$ROOT/build"
        LIB="$BUILD_DIR/librocketnet.a"
        if [ ! -f "$LIB" ]; then
            echo "error: missing $LIB (build the library under build/ first)" >&2
            exit 1
        fi
        SRC="$BUILD_DIR/check_agents_md_example.c"
        BIN="$BUILD_DIR/check_agents_md_example"
        trap 'rm -f "$SRC" "$BIN"' EXIT INT TERM

        sed -n '/<!-- check_agents_md:example:begin -->/,/<!-- check_agents_md:example:end -->/p' "$AGENTS_MD" \
            | sed '/^<!--/d; /^```/d' > "$SRC"

        if [ ! -s "$SRC" ]; then
            echo "error: failed to extract example block from $AGENTS_MD" >&2
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

        ${CC:-cc} -Wall -Werror -I"$ROOT/sdk" "$SRC" "$LIB" $LIBS -o "$BIN"
        "$BIN"
        ;;
    *)
        usage
        ;;
esac
