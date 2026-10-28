#!/bin/sh
#
# tools/framework_vendor_main.sh -- clone the framework read-only and vendor
# rocketnet main into it with the framework's own re-vendor script.
#
#   tools/framework_vendor_main.sh <framework ref> <directory>
#
#   tools/framework_vendor_main.sh main "$RUNNER_TEMP/framework"
#       clone robotgamedevelopers/framework at main into <directory>, fetch
#       rocketnet main, and run the framework's scripts/revendor_rocketnet.sh
#       at that commit, which archives sdk/, sodium/, the XDP program and the
#       cmake recipes into game/ThirdParty/rocketnet/, rewrites its VENDOR.md
#       and checks the copy with scripts/check_vendored_rocketnet.sh
#
# Building the framework and running its smoke test are the caller's next
# steps, with the framework's own commands for each OS
# (.github/workflows/nightly-framework.yml). This script stops at a vendored,
# checked tree, because that is the part that is the same on every OS.
#
# The framework is never pushed to: the clone's push URL is "no-push", and
# the vendored tree is only ever built where it lies.
#
# The last line printed is the verdict, one line:
#
#   framework vendor: pass|fail framework=<sha> previous=<sha> pin=<sha> \
#     changed=<files> reason=<word>
#
# previous is the rocketnet commit the framework had vendored; changed is how
# many files under game/ThirdParty/rocketnet/ the re-vendor touched (0 when
# the framework already pins rocketnet main).
#
# THE TOKENS. Both repositories are private. FRAMEWORK_READ_TOKEN reads the
# framework; ROCKETNET_READ_TOKEN lets the re-vendor script fetch rocketnet
# main from https://github.com/mas-bandwidth/rocketnet.git. Each reaches git
# as an http.extraHeader through git's GIT_CONFIG_* environment, scoped to its
# own organisation's URL prefix, so neither is on a command line, in a clone's
# config or in a log. This script never runs under `set -x`. A missing
# framework token is a failure that says so. FRAMEWORK_URL overrides the clone
# URL (an ssh alias with a read-only deploy key, on a bench); no framework
# token is read for a URL that is not https://github.com/. Without
# ROCKETNET_READ_TOKEN the re-vendor fetch uses whatever git credentials the
# machine has, which is what a bench wants.
#
# Environment:
#   FRAMEWORK_READ_TOKEN   read-only token for the framework's https clone
#   FRAMEWORK_URL          clone URL (default https://github.com/robotgamedevelopers/framework.git)
#   ROCKETNET_READ_TOKEN   read token for rocketnet's https fetch (CI: github.token)
#   ROCKETNET_PIN          the rocketnet commit to vendor (default: origin/main
#                          after a fetch); the framework's script refuses one
#                          that is not on main

set -u

usage()
{
    echo "usage: tools/framework_vendor_main.sh <framework ref> <directory>"
}

if [ $# -ne 2 ]; then
    usage
    exit 1
fi

framework_ref=$1
framework_dir=$2

repo_root=$( cd "$( dirname "$0" )/.." && pwd )
framework_url=${FRAMEWORK_URL:-https://github.com/robotgamedevelopers/framework.git}

framework_sha=-
previous=-
pin=-
changed=-

verdict()
{
    # $1 pass|fail, $2 reason
    echo "framework vendor: $1 framework=$framework_sha previous=$previous pin=$pin changed=$changed reason=$2"
}

if [ -e "$framework_dir" ]; then
    echo "framework_vendor_main: FAIL -- $framework_dir already exists; give a directory that does not"
    verdict fail no-fresh-directory
    exit 1
fi

log_dir=$( mktemp -d "${TMPDIR:-/tmp}/framework-vendor.XXXXXX" ) || {
    echo "framework_vendor_main: FAIL -- could not make a log directory"
    verdict fail no-log-directory
    exit 1
}

# ---- 1. clone the framework, read-only -----------------------------------

case "$framework_url" in
    https://github.com/*)
        if [ -z "${FRAMEWORK_READ_TOKEN:-}" ]; then
            echo "framework_vendor_main: FAIL -- FRAMEWORK_READ_TOKEN is not set: $framework_url is private"
            verdict fail no-token
            exit 1
        fi
        # printf is a shell builtin: the token reaches base64 on a pipe,
        # never as an argument.
        auth=$( printf 'x-access-token:%s' "$FRAMEWORK_READ_TOKEN" | base64 | tr -d '\n' )
        GIT_CONFIG_COUNT=1 \
        GIT_CONFIG_KEY_0=http.https://github.com/robotgamedevelopers/.extraheader \
        GIT_CONFIG_VALUE_0="AUTHORIZATION: basic $auth" \
        GIT_TERMINAL_PROMPT=0 \
            git clone --quiet --no-checkout "$framework_url" "$framework_dir" > "$log_dir/clone.log" 2>&1
        clone_status=$?
        auth=
        ;;
    *)
        GIT_TERMINAL_PROMPT=0 git clone --quiet --no-checkout "$framework_url" "$framework_dir" > "$log_dir/clone.log" 2>&1
        clone_status=$?
        ;;
esac

if [ $clone_status -ne 0 ]; then
    echo "framework_vendor_main: FAIL -- could not clone $framework_url"
    sed 's/^/  /' "$log_dir/clone.log"
    verdict fail clone
    exit 1
fi

git -C "$framework_dir" remote set-url --push origin no-push

target=$( git -C "$framework_dir" rev-parse --verify -q "origin/$framework_ref^{commit}" \
       || git -C "$framework_dir" rev-parse --verify -q "$framework_ref^{commit}" )
if [ -z "$target" ] || ! git -C "$framework_dir" checkout --quiet --detach "$target" > "$log_dir/checkout.log" 2>&1; then
    echo "framework_vendor_main: FAIL -- '$framework_ref' is not a branch or commit of $framework_url"
    verdict fail ref
    exit 1
fi
framework_sha=$target

manifest="$framework_dir/game/ThirdParty/rocketnet/VENDOR.md"
if [ -f "$manifest" ]; then
    previous=$( sed -nE 's/^\| commit \| \*\*([0-9a-f]{40})\*\* \|$/\1/p' "$manifest" )
    [ -n "$previous" ] || previous=-
fi

# ---- 2. rocketnet main ---------------------------------------------------

if [ -n "${ROCKETNET_PIN:-}" ]; then
    pin=$( git -C "$repo_root" rev-parse --verify -q "$ROCKETNET_PIN^{commit}" ) || pin=
else
    if ! git -C "$repo_root" fetch --quiet origin main > "$log_dir/fetch.log" 2>&1; then
        echo "framework_vendor_main: FAIL -- could not fetch rocketnet main into $repo_root"
        sed 's/^/  /' "$log_dir/fetch.log"
        pin=-
        verdict fail fetch
        exit 1
    fi
    pin=$( git -C "$repo_root" rev-parse --verify -q "refs/remotes/origin/main^{commit}" ) || pin=
fi
if [ -z "$pin" ]; then
    pin=-
    echo "framework_vendor_main: FAIL -- no rocketnet commit to vendor"
    verdict fail pin
    exit 1
fi
echo "framework_vendor_main: framework $framework_ref is $framework_sha, vendoring rocketnet $pin over $previous"

# ---- 3. the framework's own re-vendor ------------------------------------

revendor="$framework_dir/scripts/revendor_rocketnet.sh"
if [ ! -f "$revendor" ]; then
    echo "framework_vendor_main: FAIL -- the framework has no scripts/revendor_rocketnet.sh"
    verdict fail no-revendor-script
    exit 1
fi

# The re-vendor script clones from ROCKETNET_REPO (this checkout, read
# locally), then fetches main from GitHub itself to prove the pin is on main.
if [ -n "${ROCKETNET_READ_TOKEN:-}" ]; then
    auth=$( printf 'x-access-token:%s' "$ROCKETNET_READ_TOKEN" | base64 | tr -d '\n' )
    GIT_CONFIG_COUNT=1 \
    GIT_CONFIG_KEY_0=http.https://github.com/mas-bandwidth/.extraheader \
    GIT_CONFIG_VALUE_0="AUTHORIZATION: basic $auth" \
    GIT_TERMINAL_PROMPT=0 \
    ROCKETNET_REPO="$repo_root" \
        bash "$revendor" "$pin" > "$log_dir/revendor.log" 2>&1
    revendor_status=$?
    auth=
else
    GIT_TERMINAL_PROMPT=0 ROCKETNET_REPO="$repo_root" \
        bash "$revendor" "$pin" > "$log_dir/revendor.log" 2>&1
    revendor_status=$?
fi
sed 's/^/  revendor: /' "$log_dir/revendor.log"

if [ $revendor_status -ne 0 ]; then
    echo "framework_vendor_main: FAIL -- the framework's re-vendor script refused rocketnet $pin"
    verdict fail revendor
    exit 1
fi

changed=$( git -C "$framework_dir" status --porcelain -- game/ThirdParty/rocketnet | grep -vc ' game/ThirdParty/rocketnet/VENDOR.md$' )
echo "framework_vendor_main: files under game/ThirdParty/rocketnet changed by the re-vendor (VENDOR.md aside):"
git -C "$framework_dir" status --porcelain -- game/ThirdParty/rocketnet | sed 's/^/  /'

rm -rf "$log_dir"
verdict pass vendored
exit 0
