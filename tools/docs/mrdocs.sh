#!/bin/sh
# mrdocs.sh: locate or download MrDocs 2026.9.29, build the documentation
# compilation database in build/docs/cmake, and generate the HTML reference
# documentation under build/docs/html.
set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

MRDOCS_VERSION="2026.9.29"
MRDOCS_SHA256_LINUX="9d96e42f303046fb0b221aa9071eabccc40b9683e0b7af2b02a929d15476ca6a"
MRDOCS_SHA256_DARWIN="b393adf3df2180fc00edd50d6a976a79de3a1b9fbab0e93844495e4faa1c8435"
MRDOCS_DIR="$ROOT/build/mrdocs-${MRDOCS_VERSION}"

check_version() {
    bin="$1"
    [ -x "$bin" ] || return 1
    "$bin" --version 2>/dev/null | grep -q "$MRDOCS_VERSION"
}

sha256_file() {
    file="$1"
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$file" | awk '{print $1}'
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$file" | awk '{print $1}'
    else
        python3 - "$file" << 'PY'
import hashlib, sys
h = hashlib.sha256()
with open(sys.argv[1], "rb") as f:
    for chunk in iter(lambda: f.read(65536), b""):
        h.update(chunk)
print(h.hexdigest())
PY
    fi
}

MRDOCS_BIN=""
if [ -n "${MRDOCS:-}" ] && check_version "$MRDOCS"; then
    MRDOCS_BIN="$MRDOCS"
elif command -v mrdocs >/dev/null 2>&1 && check_version "$(command -v mrdocs)"; then
    MRDOCS_BIN="$(command -v mrdocs)"
elif check_version "$MRDOCS_DIR/bin/mrdocs"; then
    MRDOCS_BIN="$MRDOCS_DIR/bin/mrdocs"
else
    os_name="$(uname -s)"
    case "$os_name" in
        Linux)
            platform="Linux"
            expected_sha="$MRDOCS_SHA256_LINUX"
            ;;
        Darwin)
            platform="Darwin"
            expected_sha="$MRDOCS_SHA256_DARWIN"
            ;;
        *)
            echo "error: unsupported OS for MrDocs download: $os_name" >&2
            exit 1
            ;;
    esac
    mkdir -p "$MRDOCS_DIR"
    archive="$MRDOCS_DIR/MrDocs-${MRDOCS_VERSION}-${platform}.tar.xz"
    url="https://github.com/cppalliance/mrdocs/releases/download/${MRDOCS_VERSION}/MrDocs-${MRDOCS_VERSION}-${platform}.tar.xz"
    if [ ! -f "$archive" ]; then
        curl -fsSL -o "$archive" "$url"
    fi
    actual_sha="$(sha256_file "$archive")"
    if [ "$actual_sha" != "$expected_sha" ]; then
        echo "error: sha256 mismatch for $archive: expected $expected_sha, got $actual_sha" >&2
        rm -f "$archive"
        exit 1
    fi
    tar -xJf "$archive" -C "$MRDOCS_DIR" --strip-components=1
    MRDOCS_BIN="$MRDOCS_DIR/bin/mrdocs"
fi

"$MRDOCS_BIN" --version

CONFIG_PATH="${MRDOCS_CONFIG:-$ROOT/mrdocs.yml}"
if [ ! -f "$CONFIG_PATH" ]; then
    echo "error: missing MrDocs config file: $CONFIG_PATH" >&2
    exit 1
fi

mkdir -p "$ROOT/build/docs"
ACTIVE_CONFIG="$(python3 - "$ROOT" "$CONFIG_PATH" << 'PY'
import os, sys, yaml

root, cfg_path = sys.argv[1], sys.argv[2]
with open(cfg_path, encoding="utf-8") as f:
    data = yaml.safe_load(f) or {}

inputs = data.get("input", [])
if isinstance(inputs, str):
    inputs = [inputs]
for inp in inputs:
    p = inp if os.path.isabs(inp) else os.path.join(root, inp)
    if not os.path.isdir(p):
        sys.stderr.write(f"error: input directory does not exist: {inp}\n")
        sys.exit(1)

for key in ("source-root", "compilation-database", "cmake-build-dir", "output"):
    if key in data and isinstance(data[key], str) and not os.path.isabs(data[key]):
        data[key] = os.path.join(root, data[key])
data["input"] = [i if os.path.isabs(i) else os.path.join(root, i) for i in inputs]

# The counters: docs/mrdocs/CMakeLists.txt writes the documentation-only
# expansion of ROCKETNET_COUNTER_ROWS as gen/rocketnet_counters.h in the
# CMake build directory (tools/docs/counters_doc.py); the reference takes it
# in from there.
cmake_build_dir = data.get("cmake-build-dir", os.path.join(root, "build", "docs", "cmake"))
data["input"].append(os.path.join(cmake_build_dir, "gen"))
patterns = data.get("file-patterns", [])
if isinstance(patterns, str):
    patterns = [patterns]
if "rocketnet_counters.h" not in patterns:
    patterns.append("rocketnet_counters.h")
data["file-patterns"] = patterns

resolved = os.path.join(root, "build", "docs", "mrdocs.resolved.yml")
with open(resolved, "w", encoding="utf-8") as out:
    yaml.safe_dump(data, out, sort_keys=False)
print(resolved)
PY
)"

cmake -S "$ROOT/docs/mrdocs" -B "$ROOT/build/docs/cmake" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON >/dev/null
cmake --build "$ROOT/build/docs/cmake" >/dev/null

rm -rf "$ROOT/build/docs/html"
# One extraction thread. MrDocs merges the translation units (public.cpp and
# public_c.c both declare the public surface) in the order their threads
# finish, and the opaque records come out of the merge differently by order:
# rocketnet_client_t and rocketnet_server_t were linked or not, and reported
# or not, from run to run on the same tree. One thread merges in compilation
# database order, so identical trees give identical reports
# (tools/docs/check_mrdocs_deterministic.sh checks it).
set +e
"$MRDOCS_BIN" --config="$ACTIVE_CONFIG" --concurrency=1 >"$ROOT/build/docs/mrdocs.log" 2>&1
mrdocs_rc=$?
set -e

cat "$ROOT/build/docs/mrdocs.log"
if [ "$mrdocs_rc" -ne 0 ]; then
    exit "$mrdocs_rc"
fi

if [ ! -f "$ROOT/build/docs/html/index.html" ]; then
    echo "error: mrdocs did not generate build/docs/html/index.html" >&2
    exit 1
fi
