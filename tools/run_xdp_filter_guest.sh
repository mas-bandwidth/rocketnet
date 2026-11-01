#!/usr/bin/env bash
# RocketNet. Copyright 2026 Más Bandwidth LLC.
# Licensed under the GNU Affero General Public License v3.0
#
# Runs ./bin/test_xdp_filter -v against the built server_xdp.o inside a
# virtme-ng (vng) guest of the host's matching kernel, requiring all 18
# registered BPF_PROG_TEST_RUN and guest-only XDP cases to print "ok" with
# zero skips and the exact final line "test_xdp_filter: 18 cases passed".
#
#   bash tools/run_xdp_filter_guest.sh [options]
#
# Options:
#   --artifact-dir DIR   directory for verifier/kernel/case/cleanup artifacts
#                        (default: build/xdp-filter-guest)
#   --binary PATH        test_xdp_filter binary (default: ./bin/test_xdp_filter)
#   --object PATH        compiled BPF object (default: build/generated/server_xdp.o
#                        or build/server_xdp.o)
#   --vm-kernel PATH     guest kernel image (default: ~/vm-kernels/vmlinuz-$(uname -r))
#   --kvm-device PATH    KVM device node (default: /dev/kvm)
#   --vng PATH           virtme-ng binary name or path (default: vng)
#   --memory SIZE        guest memory size passed to vng --memory (default: 4G)
#   --cpus COUNT         guest CPU count passed to vng --cpus (default: 4; must be >= 4)
#   --timeout SECONDS    guest wall-clock timeout in seconds (default: 180)
#   --verify-log FILE    validate an existing or synthetic output log without
#                        starting a guest or sending kernel traffic
#   --verify-exit CODE   simulated process exit status for --verify-log (default: 0)
#   -h, --help           show this help text

set -u

NAME="run_xdp_filter_guest.sh"
ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
ARTIFACT_DIR="$ROOT_DIR/build/xdp-filter-guest"
BINARY="$ROOT_DIR/bin/test_xdp_filter"
OBJECT=""
VM_KERNEL="${HOME:-}/vm-kernels/vmlinuz-$(uname -r)"
KVM_DEVICE="/dev/kvm"
VNG_BIN="vng"
GUEST_MEMORY="4G"
GUEST_CPUS="4"
TIMEOUT_SECONDS="180"
VERIFY_LOG=""
VERIFY_EXIT="0"

REQUIRED_CASES=(
    "ipv4_rocketnet_packet_redirects"
    "ipv6_rocketnet_packet_redirects"
    "ipv6_extension_headers"
    "ipv4_options"
    "ipv4_fragments"
    "ipv6_fragments"
    "ipv6_wrong_address_passes"
    "ipv6_short_header_drops"
    "ipv6_too_large_drops"
    "ipv6_icmpv6_passes"
    "ipv6_same_drops_as_ipv4"
    "config_layout_matches_header"
    "replay_drops_behind_the_window"
    "replay_never_writes_the_map"
    "replay_key_layout_matches_header"
    "xdp_on_link_reply_reaches_the_client"
    "xdp_create_fails_part_way_tears_down"
    "xsk_bind_falls_back_to_copy"
)
EXPECTED_CASE_COUNT=${#REQUIRED_CASES[@]}
EXPECTED_SUMMARY="test_xdp_filter: 18 cases passed"

usage()
{
    sed -n '2,/^set -u/p' "${BASH_SOURCE[0]}" | sed '$d' | sed 's/^# \{0,1\}//'
}

refuse()
{
    echo "$NAME: $*" >&2
    exit 2
}

fail_qualification()
{
    echo "XDP-FILTER-GUEST FAIL: $*" >&2
    if [ -n "${ARTIFACT_DIR:-}" ] && [ -d "${ARTIFACT_DIR:-}" ]; then
        echo "FAIL: $*" >> "$ARTIFACT_DIR/summary.txt"
    fi
    exit 1
}

while [ $# -gt 0 ]; do
    case "$1" in
        --artifact-dir) [ $# -ge 2 ] || refuse "--artifact-dir requires a path"; ARTIFACT_DIR=$2; shift 2 ;;
        --binary)       [ $# -ge 2 ] || refuse "--binary requires a path"; BINARY=$2; shift 2 ;;
        --object)       [ $# -ge 2 ] || refuse "--object requires a path"; OBJECT=$2; shift 2 ;;
        --vm-kernel)    [ $# -ge 2 ] || refuse "--vm-kernel requires a path"; VM_KERNEL=$2; shift 2 ;;
        --kvm-device)   [ $# -ge 2 ] || refuse "--kvm-device requires a path"; KVM_DEVICE=$2; shift 2 ;;
        --vng)          [ $# -ge 2 ] || refuse "--vng requires a command or path"; VNG_BIN=$2; shift 2 ;;
        --memory)       [ $# -ge 2 ] || refuse "--memory requires a size"; GUEST_MEMORY=$2; shift 2 ;;
        --cpus)         [ $# -ge 2 ] || refuse "--cpus requires a count"; GUEST_CPUS=$2; shift 2 ;;
        --timeout)      [ $# -ge 2 ] || refuse "--timeout requires seconds"; TIMEOUT_SECONDS=$2; shift 2 ;;
        --verify-log)   [ $# -ge 2 ] || refuse "--verify-log requires a file"; VERIFY_LOG=$2; shift 2 ;;
        --verify-exit)  [ $# -ge 2 ] || refuse "--verify-exit requires an integer"; VERIFY_EXIT=$2; shift 2 ;;
        -h|--help)      usage; exit 0 ;;
        *)              refuse "unknown option $1" ;;
    esac
done

case "$GUEST_CPUS" in
    ''|*[!0-9]*) refuse "--cpus must be a positive integer" ;;
esac
if [ "$GUEST_CPUS" -lt 4 ]; then
    refuse "--cpus must be at least 4 (isolcpus=1-3 for single-queue and two-queue guest cases)"
fi

case "$TIMEOUT_SECONDS" in
    ''|*[!0-9]*|0) refuse "--timeout must be a positive integer" ;;
esac

TIMEOUT_COMMAND=(timeout "$TIMEOUT_SECONDS")
if [ "${ROCKETNET_NIGHTLY_NO_TIMEOUT:-}" = 1 ]; then
    TIMEOUT_COMMAND=()
fi

case "$VERIFY_EXIT" in
    ''|*[!0-9]*) refuse "--verify-exit must be a non-negative integer" ;;
esac

mkdir -p "$ARTIFACT_DIR" || exit 1
ARTIFACT_DIR=$(cd "$ARTIFACT_DIR" && pwd)
: > "$ARTIFACT_DIR/summary.txt"

git -C "$ROOT_DIR" rev-parse HEAD > "$ARTIFACT_DIR/head.txt" 2>/dev/null || echo "unknown" > "$ARTIFACT_DIR/head.txt"

validate_guest_log()
{
    local log_file=$1
    local run_exit=$2

    if [ ! -r "$log_file" ]; then
        fail_qualification "guest log $log_file is missing or unreadable"
    fi

    grep -E '^verifier: ' "$log_file" > "$ARTIFACT_DIR/verifier.txt" 2>/dev/null || : > "$ARTIFACT_DIR/verifier.txt"
    grep -E '^[a-z0-9_]+: (ok|FAILED|skipped.*)$' "$log_file" > "$ARTIFACT_DIR/cases.txt" 2>/dev/null || : > "$ARTIFACT_DIR/cases.txt"

    if [ "$run_exit" -ne 0 ]; then
        fail_qualification "test_xdp_filter exited non-zero ($run_exit)"
    fi

    if grep -E -q ': skipped|skipped \(|[0-9]+ skipped' "$log_file"; then
        local skipped_lines
        skipped_lines=$(grep -E ': skipped|skipped \(|[0-9]+ skipped' "$log_file" | tr '\n' ';')
        fail_qualification "skipped case(s) detected: $skipped_lines"
    fi

    if grep -E -q '^[a-z0-9_]+: FAILED$|cases failed|  FAIL ' "$log_file"; then
        local failed_lines
        failed_lines=$(grep -E '^[a-z0-9_]+: FAILED$|cases failed|  FAIL ' "$log_file" | head -n 5 | tr '\n' ';')
        fail_qualification "failed case(s) detected: $failed_lines"
    fi

    if [ ! -s "$ARTIFACT_DIR/verifier.txt" ]; then
        fail_qualification "verifier log line ('verifier: ...') is missing from test_xdp_filter -v output"
    fi

    local actual_cases=()
    local line
    while IFS= read -r line; do
        [ -n "$line" ] || continue
        actual_cases+=("$line")
    done < "$ARTIFACT_DIR/cases.txt"

    local actual_count=${#actual_cases[@]}
    local req_case
    for req_case in "${REQUIRED_CASES[@]}"; do
        if ! grep -qx "$req_case: ok" "$ARTIFACT_DIR/cases.txt"; then
            fail_qualification "required case '$req_case' did not report '$req_case: ok' (ran $actual_count case lines)"
        fi
    done

    if [ "$actual_count" -ne "$EXPECTED_CASE_COUNT" ]; then
        fail_qualification "expected exactly $EXPECTED_CASE_COUNT cases, got $actual_count"
    fi

    local idx=0
    for req_case in "${REQUIRED_CASES[@]}"; do
        if [ "${actual_cases[$idx]}" != "$req_case: ok" ]; then
            fail_qualification "case $((idx + 1)) mismatch: expected '$req_case: ok', got '${actual_cases[$idx]}'"
        fi
        idx=$((idx + 1))
    done

    local last_line
    last_line=$(grep -v '^[[:space:]]*$' "$log_file" | tail -n 1)
    if [ "$last_line" != "$EXPECTED_SUMMARY" ]; then
        fail_qualification "final summary line is '${last_line:-<empty>}', expected '$EXPECTED_SUMMARY'"
    fi

    if ! grep -qx "$EXPECTED_SUMMARY" "$log_file"; then
        fail_qualification "exact summary '$EXPECTED_SUMMARY' not found in output"
    fi
}

if [ -n "$VERIFY_LOG" ]; then
    [ -r "$VERIFY_LOG" ] || fail_qualification "verify-log file $VERIFY_LOG is not readable"
    cp -f "$VERIFY_LOG" "$ARTIFACT_DIR/guest.log"
    printf 'verify-log mode: file=%s simulated_exit=%s\n' "$VERIFY_LOG" "$VERIFY_EXIT" > "$ARTIFACT_DIR/command.txt"
    printf 'host_kernel=%s\nmode=verify-log\n' "$(uname -r)" > "$ARTIFACT_DIR/kernel.txt"
    validate_guest_log "$ARTIFACT_DIR/guest.log" "$VERIFY_EXIT"
    {
        echo "PASS: $EXPECTED_SUMMARY"
        echo "mode: verify-log"
        echo "cases_passed: $EXPECTED_CASE_COUNT/$EXPECTED_CASE_COUNT"
        echo "skipped: 0"
    } > "$ARTIFACT_DIR/summary.txt"
    echo "XDP-FILTER-GUEST PASS (verify-log): $EXPECTED_SUMMARY (0 skipped)"
    exit 0
fi

if [ "$(uname -s)" != "Linux" ]; then
    fail_qualification "Linux host required (found $(uname -s))"
fi

if [ -z "$OBJECT" ]; then
    if [ -r "$ROOT_DIR/build/generated/server_xdp.o" ]; then
        if [ ! -r "$ROOT_DIR/build/server_xdp.o" ] || ! cmp -s "$ROOT_DIR/build/generated/server_xdp.o" "$ROOT_DIR/build/server_xdp.o"; then
            cp -f "$ROOT_DIR/build/generated/server_xdp.o" "$ROOT_DIR/build/server_xdp.o" 2>/dev/null || true
        fi
        if [ -r "$ROOT_DIR/build/server_xdp.o" ]; then
            OBJECT="$ROOT_DIR/build/server_xdp.o"
        else
            OBJECT="$ROOT_DIR/build/generated/server_xdp.o"
        fi
    elif [ -r "$ROOT_DIR/build/server_xdp.o" ]; then
        OBJECT="$ROOT_DIR/build/server_xdp.o"
    else
        OBJECT="$ROOT_DIR/build/generated/server_xdp.o"
    fi
fi

if ! command -v "$VNG_BIN" >/dev/null 2>&1; then
    fail_qualification "prerequisite missing: virtme-ng ('$VNG_BIN') is not installed or not on PATH"
fi

if [ ! -r "$VM_KERNEL" ]; then
    fail_qualification "prerequisite missing: guest kernel image '$VM_KERNEL' is not readable"
fi

if [ ! -w "$KVM_DEVICE" ]; then
    fail_qualification "prerequisite missing: KVM device '$KVM_DEVICE' is not writable"
fi

if [ ! -x "$BINARY" ]; then
    fail_qualification "prerequisite missing: test binary '$BINARY' is not executable"
fi

BINARY=$(cd "$(dirname "$BINARY")" && pwd)/$(basename "$BINARY")
BIN_DIR=$(dirname "$BINARY")

if [ ! -x "$BIN_DIR/server" ] || [ ! -x "$BIN_DIR/client" ]; then
    fail_qualification "prerequisite missing: sibling binaries '$BIN_DIR/server' and '$BIN_DIR/client' must be executable for guest cases"
fi

if [ ! -r "$OBJECT" ] || [ ! -s "$OBJECT" ]; then
    fail_qualification "prerequisite missing: BPF object '$OBJECT' is missing or empty"
fi

OBJECT=$(cd "$(dirname "$OBJECT")" && pwd)/$(basename "$OBJECT")

sha256sum "$OBJECT" "$BINARY" "$BIN_DIR/server" "$BIN_DIR/client" > "$ARTIFACT_DIR/object.sha256" || fail_qualification "could not hash binaries and BPF object"

host_snapshot()
{
    local label=$1
    echo "=== host_$label ==="
    echo "loadavg: $(cat /proc/loadavg 2>/dev/null || echo unknown)"
    echo "test_interfaces: $(ip -o link show 2>/dev/null | awk -F': ' '{print $2}' | grep -E '^(rnzc|rnxsk|rnonlink|rnteardown|rnxdp)' | tr '\n' ' ' || true)"
    echo "netns: $(ip netns list 2>/dev/null | tr '\n' ' ' || true)"
    echo "xdp_links: $(ip link show 2>/dev/null | grep -E 'xdp|prog/xdp' | tr '\n' ' ' || true)"
    echo "processes: $(ps -eo pid=,comm=,args= 2>/dev/null | grep -E '(/bin/test_xdp_filter|/bin/server|/bin/client)' | grep -v grep | grep -v "$NAME" || true)"
}

host_snapshot "before" > "$ARTIFACT_DIR/cleanup.txt"

GUEST_SCRIPT="$ARTIFACT_DIR/guest-run.sh"
GUEST_STATUS="$ARTIFACT_DIR/guest.status"
GUEST_LOG="$ARTIFACT_DIR/guest.log"
GUEST_KERNEL="$ARTIFACT_DIR/guest-kernel.txt"
GUEST_CLEANUP="$ARTIFACT_DIR/guest-cleanup.txt"
rm -f "$GUEST_STATUS" "$GUEST_LOG" "$GUEST_KERNEL" "$GUEST_CLEANUP"

cat > "$GUEST_SCRIPT" <<EOF
#!/usr/bin/env bash
set -u
{
    echo "guest_uname_r=\$(uname -r)"
    echo "guest_uname_a=\$(uname -a)"
    echo "guest_cmdline=\$(cat /proc/cmdline 2>/dev/null || true)"
    echo "guest_isolated_cpus=\$(cat /sys/devices/system/cpu/isolated 2>/dev/null || true)"
    echo "guest_online_cpus=\$(cat /sys/devices/system/cpu/online 2>/dev/null || true)"
} > "$GUEST_KERNEL"

mountpoint -q /sys/fs/bpf || mount -t bpf bpf /sys/fs/bpf
ip link set lo up

# Ensure clang is not required at runtime inside the guest while keeping standard system utilities
"$BINARY" -v "$OBJECT" > "$GUEST_LOG" 2>&1
rc=\$?
echo "guest_test_exit=\$rc" > "$GUEST_STATUS"

{
    echo "=== guest_after ==="
    echo "guest_test_interfaces: \$(ip -o link show 2>/dev/null | awk -F': ' '{print \$2}' | grep -E '^(rnzc|rnxsk|rnonlink|rnteardown|rnxdp)' | tr '\n' ' ' || true)"
    echo "guest_netns: \$(ip netns list 2>/dev/null | tr '\n' ' ' || true)"
    echo "guest_xdp_links: \$(ip link show 2>/dev/null | grep -E 'xdp|prog/xdp' | tr '\n' ' ' || true)"
    echo "guest_bpf_pins: \$(find /sys/fs/bpf -mindepth 1 \\( -type f -o -name 'rocketnet*' \\) 2>/dev/null | tr '\n' ' ' || true)"
    echo "guest_processes: \$(ps -eo pid=,comm=,args= 2>/dev/null | grep -E '(/bin/test_xdp_filter|/bin/server|/bin/client)' | grep -v grep || true)"
} > "$GUEST_CLEANUP"
exit "\$rc"
EOF
chmod +x "$GUEST_SCRIPT"

GUEST_APPEND="irqaffinity=0 isolcpus=1-3 nohz_full=1-3"
RWDIR_ARGS=(--rwdir "$ROOT_DIR")
case "$ARTIFACT_DIR/" in
    "$ROOT_DIR/"*) ;;
    *) RWDIR_ARGS+=(--rwdir "$ARTIFACT_DIR") ;;
esac

{
    printf 'vng_command:'
    for arg in ${TIMEOUT_COMMAND[@]+"${TIMEOUT_COMMAND[@]}"} "$VNG_BIN"; do
        printf ' %q' "$arg"
    done
    printf ' --run %q --memory %s --cpus %s --append %q' \
        "$VM_KERNEL" "$GUEST_MEMORY" "$GUEST_CPUS" "$GUEST_APPEND"
    for arg in "${RWDIR_ARGS[@]}"; do
        printf ' %q' "$arg"
    done
    printf ' -- sudo bash %q\n' "$GUEST_SCRIPT"
    printf 'guest_command: %q -v %q\n' "$BINARY" "$OBJECT"
} > "$ARTIFACT_DIR/command.txt"

# Run vng from a short working directory so its virtiofs socket path stays well below 108 bytes.
SHORT_DIR=$(mktemp -d /tmp/rnxdpflt.XXXXXX)
trap 'rm -rf "$SHORT_DIR"' EXIT

(
    cd "$SHORT_DIR" && \
    ${TIMEOUT_COMMAND[@]+"${TIMEOUT_COMMAND[@]}"} "$VNG_BIN" \
        --run "$VM_KERNEL" \
        --memory "$GUEST_MEMORY" \
        --cpus "$GUEST_CPUS" \
        --append "$GUEST_APPEND" \
        "${RWDIR_ARGS[@]}" \
        -- sudo bash "$GUEST_SCRIPT"
) > "$ARTIFACT_DIR/vng.log" 2>&1
VNG_EXIT=$?

rm -rf "$SHORT_DIR"
trap - EXIT

{
    echo "host_uname_r=$(uname -r)"
    echo "host_uname_a=$(uname -a)"
    echo "vm_kernel=$VM_KERNEL"
    if [ -r "$GUEST_KERNEL" ]; then
        cat "$GUEST_KERNEL"
    else
        echo "guest_kernel_info=missing"
    fi
} > "$ARTIFACT_DIR/kernel.txt"

if [ -r "$GUEST_CLEANUP" ]; then
    cat "$GUEST_CLEANUP" >> "$ARTIFACT_DIR/cleanup.txt"
fi
host_snapshot "after" >> "$ARTIFACT_DIR/cleanup.txt"

GUEST_EXIT=$(sed -nE 's/^guest_test_exit=([0-9]+)$/\1/p' "$GUEST_STATUS" 2>/dev/null | tail -n 1)
if [ -z "$GUEST_EXIT" ]; then
    fail_qualification "virtme-ng guest did not record a test_xdp_filter exit status (vng exit=$VNG_EXIT; see $ARTIFACT_DIR/vng.log)"
fi

if [ "$VNG_EXIT" -ne 0 ] && [ "$GUEST_EXIT" -eq 0 ]; then
    fail_qualification "vng exited non-zero ($VNG_EXIT) despite guest_test_exit=0"
fi

if [ -r "$GUEST_LOG" ]; then
    cat "$GUEST_LOG"
fi

validate_guest_log "$GUEST_LOG" "$GUEST_EXIT"

# Check guest and host cleanup invariants
guest_leftover_ifaces=$(sed -n 's/^guest_test_interfaces: //p' "$ARTIFACT_DIR/cleanup.txt" | tr -d '[:space:]')
guest_leftover_netns=$(sed -n 's/^guest_netns: //p' "$ARTIFACT_DIR/cleanup.txt" | tr -d '[:space:]')
guest_leftover_xdp=$(sed -n 's/^guest_xdp_links: //p' "$ARTIFACT_DIR/cleanup.txt" | tr -d '[:space:]')
guest_leftover_pins=$(sed -n 's/^guest_bpf_pins: //p' "$ARTIFACT_DIR/cleanup.txt" | tr -d '[:space:]')
guest_leftover_procs=$(sed -n 's/^guest_processes: //p' "$ARTIFACT_DIR/cleanup.txt" | tr -d '[:space:]')

if [ -n "$guest_leftover_ifaces" ] || [ -n "$guest_leftover_netns" ] || [ -n "$guest_leftover_xdp" ] || [ -n "$guest_leftover_pins" ] || [ -n "$guest_leftover_procs" ]; then
    fail_qualification "leftover guest state after test_xdp_filter (ifaces='$guest_leftover_ifaces' netns='$guest_leftover_netns' xdp='$guest_leftover_xdp' pins='$guest_leftover_pins' procs='$guest_leftover_procs')"
fi

host_after_block=$(awk '/^=== host_after ===/{flag=1; next} flag{print}' "$ARTIFACT_DIR/cleanup.txt")
host_leftover_ifaces=$(printf '%s\n' "$host_after_block" | sed -n 's/^test_interfaces: //p' | tr -d '[:space:]')
host_leftover_procs=$(printf '%s\n' "$host_after_block" | sed -n 's/^processes: //p' | tr -d '[:space:]')
if [ -n "$host_leftover_ifaces" ] || [ -n "$host_leftover_procs" ]; then
    fail_qualification "leftover host state after guest run (ifaces='$host_leftover_ifaces' procs='$host_leftover_procs')"
fi

{
    echo "PASS: $EXPECTED_SUMMARY"
    echo "vng_exit: $VNG_EXIT"
    echo "guest_test_exit: $GUEST_EXIT"
    echo "cases_passed: $EXPECTED_CASE_COUNT/$EXPECTED_CASE_COUNT"
    echo "skipped: 0"
} > "$ARTIFACT_DIR/summary.txt"

echo "XDP-FILTER-GUEST PASS: $EXPECTED_SUMMARY (0 skipped, artifacts in $ARTIFACT_DIR)"
exit 0
