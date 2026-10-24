#!/bin/sh
#
# tools/space_release_soak.sh -- build the integrating application against
# this rocketnet release and hold a bot swarm on it under real time.
#
#   tools/space_release_soak.sh <space ref> <seconds>
#
#   tools/space_release_soak.sh main 90
#       clone robotgamedevelopers/space at main, prove it vendors this
#       checkout's HEAD byte for byte, build it with its own Makefile, hold
#       eight bots on its dedicated server for 90 seconds, and read the logs
#
# The steps, in order, and every one of them runs:
#
#   1. clone space read-only into a temporary directory under this checkout
#      (tmp/, which git ignores) and check out <space ref>
#   2. tools/check_space_vendor.py --space-dir on that clone, against the
#      release commit (ROCKETNET_RELEASE, default this checkout's HEAD)
#   3. ./space env local, then space's own `make` (build-fast: the dedicated
#      server, the launcher, happy_path and the backend)
#   4. scripts/bot_swarm.sh 8 <seconds>: the backend and the dedicated server
#      come up through dist/happy_path, eight headless bots connect, and the
#      swarm is held <seconds> once all are connected. happy_path stops the
#      processes it started, by pid, when the bound ends.
#   5. tools/check_space_soak_log.py over space's logs/server and the swarm's
#      output: any dropped frame from the first frame on, any large hitch, a
#      server exit before the bound, a BOTS FAIL line or no client connected
#      fails the run
#
# A vendor failure does not stop the build and the soak: the soak still says
# whether this release holds real time under space, and the verdict fails on
# the vendor result all the same. A clone or build failure stops the run,
# because nothing after it can be read.
#
# The last line printed is the verdict, one line:
#
#   space release soak: pass|fail space=<sha> release=<sha> vendor=pass|fail \
#     build=pass|fail <check_space_soak_log.py verdict fields>
#
# Exit 0 only when the vendor check, the build, the swarm and the log check
# all pass.
#
# THE TOKEN. The clone of https://github.com/... reads SPACE_READ_TOKEN from
# the environment and hands it to git as an http.extraHeader through git's
# GIT_CONFIG_* environment, so it is never on a command line, never in the
# clone's config and never in a log. This script never runs under `set -x`.
# A missing token is a failure that says so. SPACE_URL overrides the clone
# URL (an ssh alias holding a read-only deploy key, on a bench); no token is
# needed or read for a URL that is not https://github.com/.
#
# NEEDS: Linux with four or more CPUs (scripts/soak.sh: the dedicated server
# sizes its workers from the CPU count and dies below four), passwordless
# sudo (bot_swarm.sh builds a veth pair and a network namespace), Go, a C++
# toolchain, cmake and libcurl headers. It never stops a process it did not
# start: bot_swarm.sh refuses when a stack is already running.
#
# Environment:
#   SPACE_READ_TOKEN   read-only token for the https clone
#   SPACE_URL          clone URL (default https://github.com/robotgamedevelopers/space.git)
#   ROCKETNET_RELEASE  the rocketnet commit being released (default HEAD)
#   SPACE_MAKE_JOBS    make -j for space's build (default 4)
#   SPACE_SOAK_KEEP    1 keeps the work directory after a pass (it is always
#                      kept after a fail, and its path is printed)

set -u

bots=8

usage()
{
    echo "usage: tools/space_release_soak.sh <space ref> <seconds>"
}

if [ $# -ne 2 ]; then
    usage
    exit 1
fi

space_ref=$1
seconds=$2

case "$seconds" in
    ''|*[!0-9]*|0*)
        echo "space_release_soak: FAIL -- <seconds> is a whole number, 1 or more (got '$seconds')"
        usage
        exit 1
        ;;
esac

repo_root=$( cd "$( dirname "$0" )/.." && pwd )
space_url=${SPACE_URL:-https://github.com/robotgamedevelopers/space.git}
release=${ROCKETNET_RELEASE:-HEAD}
make_jobs=${SPACE_MAKE_JOBS:-4}

release_sha=$( git -C "$repo_root" rev-parse --verify -q "$release^{commit}" )
if [ -z "$release_sha" ]; then
    echo "space_release_soak: FAIL -- the release '$release' is not a commit in $repo_root"
    exit 1
fi

mkdir -p "$repo_root/tmp"
work=$( mktemp -d "$repo_root/tmp/space-soak.XXXXXX" ) || {
    echo "space_release_soak: FAIL -- could not make a work directory under $repo_root/tmp"
    exit 1
}
space_dir="$work/space"

space_sha=-
vendor=fail
build=fail

verdict()
{
    # $1 pass|fail, $2 the log check's fields (or a reason)
    echo "space release soak: $1 space=$space_sha release=$release_sha vendor=$vendor build=$build $2"
    if [ "$1" = pass ] && [ "${SPACE_SOAK_KEEP:-0}" != 1 ]; then
        rm -rf "$work"
    else
        echo "space_release_soak: logs kept in $work" >&2
    fi
}

# ---- 1. clone, read-only -------------------------------------------------

case "$space_url" in
    https://github.com/*)
        if [ -z "${SPACE_READ_TOKEN:-}" ]; then
            echo "space_release_soak: FAIL -- SPACE_READ_TOKEN is not set: the gate reads $space_url with that token and will not report a release as proven without it"
            verdict fail "reason=no-token"
            exit 1
        fi
        # printf is a shell builtin: the token reaches base64 on a pipe,
        # never as an argument.
        auth=$( printf 'x-access-token:%s' "$SPACE_READ_TOKEN" | base64 | tr -d '\n' )
        GIT_CONFIG_COUNT=1 \
        GIT_CONFIG_KEY_0=http.https://github.com/.extraheader \
        GIT_CONFIG_VALUE_0="AUTHORIZATION: basic $auth" \
        GIT_TERMINAL_PROMPT=0 \
            git clone --quiet --no-checkout "$space_url" "$space_dir" > "$work/clone.log" 2>&1
        clone_status=$?
        auth=
        ;;
    *)
        GIT_TERMINAL_PROMPT=0 git clone --quiet --no-checkout "$space_url" "$space_dir" > "$work/clone.log" 2>&1
        clone_status=$?
        ;;
esac

if [ $clone_status -ne 0 ]; then
    echo "space_release_soak: FAIL -- could not clone $space_url"
    sed 's/^/  /' "$work/clone.log"
    verdict fail "reason=clone"
    exit 1
fi

# Read-only: the clone has no push URL.
git -C "$space_dir" remote set-url --push origin no-push

target=$( git -C "$space_dir" rev-parse --verify -q "origin/$space_ref^{commit}" \
       || git -C "$space_dir" rev-parse --verify -q "$space_ref^{commit}" )
if [ -z "$target" ] || ! git -C "$space_dir" checkout --quiet --detach "$target" > "$work/checkout.log" 2>&1; then
    echo "space_release_soak: FAIL -- '$space_ref' is not a branch or commit of $space_url"
    verdict fail "reason=ref"
    exit 1
fi
space_sha=$target
echo "space_release_soak: space $space_ref is $space_sha, release is $release_sha"

# ---- 2. the vendor gate --------------------------------------------------

if python3 "$repo_root/tools/check_space_vendor.py" --repo "$repo_root" --release "$release_sha" \
        --space-dir "$space_dir" > "$work/vendor.log" 2>&1; then
    vendor=pass
fi
sed 's/^/  vendor: /' "$work/vendor.log"

# ---- 3. space's own build ------------------------------------------------

echo "space_release_soak: building space (make -j$make_jobs build-fast)"
if ( cd "$space_dir" && ./space env local && make -s -j"$make_jobs" build-fast ) > "$work/build.log" 2>&1; then
    build=pass
else
    echo "space_release_soak: FAIL -- space's build failed"
    echo "---------------- build log (last 40 lines) ----------------"
    tail -40 "$work/build.log"
    echo "------------------------------------------------------------"
    verdict fail "reason=build"
    exit 1
fi

# ---- 4. the swarm --------------------------------------------------------

echo "space_release_soak: scripts/bot_swarm.sh $bots $seconds"
( cd "$space_dir" && scripts/bot_swarm.sh "$bots" "$seconds" ) > "$work/swarm.log" 2>&1
swarm_status=$?
sed 's/^/  swarm: /' "$work/swarm.log"

# ---- 5. the logs ---------------------------------------------------------

python3 "$repo_root/tools/check_space_soak_log.py" \
    --server-log "$space_dir/logs/server" --swarm-output "$work/swarm.log" \
    --seconds "$seconds" --bots "$bots" > "$work/check.log" 2> "$work/check-details.log"
check_status=$?
sed 's/^/  check: /' "$work/check-details.log"

fields=$( sed -n 's/^verdict: [a-z]* //p' "$work/check.log" | head -1 )
if [ -z "$fields" ]; then
    fields="reason=no-check-verdict"
    check_status=1
fi
if [ $swarm_status -ne 0 ]; then
    fields="$fields swarm_exit=$swarm_status"
fi

if [ "$vendor" = pass ] && [ $swarm_status -eq 0 ] && [ $check_status -eq 0 ]; then
    verdict pass "$fields"
    exit 0
fi
verdict fail "$fields"
exit 1
