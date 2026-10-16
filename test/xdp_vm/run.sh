#!/usr/bin/env bash
#
# The XDP kernel tier: one fresh VM per kernel in kernels.tsv, booted with
# virtme-ng, the guest running guest.sh as root inside its own kernel.
#
#     test/xdp_vm/run.sh                    every kernel, every row
#     test/xdp_vm/run.sh --kernel NAME      one kernel
#     test/xdp_vm/run.sh --record           record the handshake captures instead (see README.md)
#     test/xdp_vm/run.sh --preflight        check the toolchain and stop
#
# The host needs no root and is never changed: no link, namespace, sysctl,
# bpffs pin or attached program. The guest sees the host's root filesystem
# read-only with its writes in an overlay, gets no host network device, and
# writes only into this run's own directory. run.sh snapshots the host before
# and after and fails host_untouched on any byte of difference. Two runs at
# once share nothing but the fetched, read-only kernel images.
#
# XDP_VM_WORK names the work directory (default build/xdp_vm in the
# checkout): fetched kernels under kernels/, each run under runs/<id>/.
# Exit 0: every row passed on every kernel. Exit 1: a row failed. Exit 2: the
# toolchain is missing or a pinned artifact was refused.

set -uo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd "$here/../.." && pwd)
work=${XDP_VM_WORK:-$repo/build/xdp_vm}
only_kernel=""
mode=rows

while [ $# -gt 0 ]; do
    case "$1" in
        --kernel) only_kernel=$2; shift 2 ;;
        --record) mode=record; shift ;;
        --preflight) mode=preflight; shift ;;
        -h|--help) sed -n '2,25p' "$0"; exit 0 ;;
        *) echo "unknown argument $1" >&2; exit 2 ;;
    esac
done

say() { printf '%s\n' "$*"; }
die() { say "run.sh: $*" >&2; exit 2; }

[ "$(uname -s)" = Linux ] || die "the XDP kernel tier runs on Linux only"
[ "$(id -u)" != 0 ] || die "run.sh runs as an ordinary user; only the guest is root"

# ---------------------------------------------------------------------------
# preflight: every tool is named before anything is fetched or booted

missing=()
for tool in qemu-system-x86_64 clang cc make pkg-config python3 curl sha256sum flock tar xz ip timeout bpftool sysctl diff; do
    command -v "$tool" >/dev/null 2>&1 || missing+=("$tool")
done
command -v dpkg-deb >/dev/null 2>&1 || command -v ar >/dev/null 2>&1 || missing+=("dpkg-deb or ar")
pkg-config --atleast-version=1.0 libbpf 2>/dev/null || missing+=("libbpf >= 1.0")
pkg-config --exists libxdp 2>/dev/null || missing+=("libxdp")
if ! command -v vng >/dev/null 2>&1; then
    python3 -m ensurepip --version >/dev/null 2>&1 || missing+=("Python venv/ensurepip for pinned virtme-ng")
    for module in argcomplete requests; do
        python3 -c "import $module" >/dev/null 2>&1 || missing+=("existing Python $module for pinned virtme-ng (no dependency downloads)")
    done
fi

kvm_flags=()
if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
    accel="kvm"
else
    accel="tcg"
    kvm_flags=(--disable-kvm)
fi

if [ ${#missing[@]} -gt 0 ]; then
    say "preflight: missing ${missing[*]}"
    say "preflight: acceleration would be $accel"
    die "the toolchain is incomplete; nothing was fetched or booted"
fi
say "preflight: toolchain present; acceleration $accel"
if [ "$accel" = tcg ]; then
    say "preflight: /dev/kvm is not usable by $(id -un); QEMU's TCG emulates the guest, roughly ten to fifty times slower"
fi
[ "$mode" = preflight ] && exit 0

mkdir -p "$work/kernels" "$work/runs" || die "could not create $work"
run_id=$(date -u +%Y%m%dT%H%M%SZ)-$$
run="$work/runs/$run_id"
mkdir -p "$run/build/objects" "$run/build/bin" || die "could not create $run"
say "run: $run"

# ---------------------------------------------------------------------------
# the host snapshot: what a run must never change

snapshot() {
    {
        echo "## ip -d link"; ip -d link show 2>&1
        echo "## ip netns"; ip netns list 2>&1
        echo "## bpftool prog show"; bpftool prog show 2>&1
        echo "## bpftool net"; bpftool net show 2>&1
        echo "## ls -laR /sys/fs/bpf"; ls -laR /sys/fs/bpf 2>&1
        echo "## sysctl net.core net.ipv4"; sysctl net.core net.ipv4 2>&1 | LC_ALL=C sort
    } > "$1"
}

snapshot "$run/host_before.txt"
snapshot_checked=0
host="FAIL host snapshot not compared"
finish_snapshot() {
    if [ "$snapshot_checked" = 0 ]; then
        snapshot "$run/host_after.txt"
        if diff -u "$run/host_before.txt" "$run/host_after.txt" > "$run/host_diff.txt"; then
            host="PASS byte-identical"
        else
            host="FAIL the host snapshot changed: $run/host_diff.txt"
        fi
        printf '%s\n' "$host" > "$run/host_untouched.txt"
        snapshot_checked=1
    fi
    [[ "$host" = PASS* ]]
}
# Refusals and failed builds still owe an after-snapshot. The original
# failure status is preserved; a changed host can never turn into success.
on_exit() {
    local status=$?
    trap - EXIT
    finish_snapshot || { [ "$status" != 0 ] || status=1; }
    say "host_untouched: $host"
    exit "$status"
}
trap on_exit EXIT

# ---------------------------------------------------------------------------
# pinned artifacts: fetched once under a lock, refused on a sha256 mismatch

pinned() { grep -v '^#' "$here/kernels.tsv" | awk -F'\t' -v n="$1" -v k="$2" '$1 == n && $3 == k'; }

# fetch URL SHA256 DEST: exit 0 with DEST verified, exit 1 on a refusal
fetch() {
    local url=$1 sha=$2 dest=$3
    (
        flock 9
        if [ ! -f "$dest" ]; then
            curl -fsSL --retry 2 -o "$dest.part" "$url" || { say "fetch of $url failed"; rm -f "$dest.part"; exit 1; }
            mv "$dest.part" "$dest"
        fi
        got=$(sha256sum "$dest" | cut -d' ' -f1)
        if [ "$got" != "$sha" ]; then
            say "REFUSED $(basename "$dest"): sha256 $got is not the pinned $sha"
            mv "$dest" "$dest.refused"
            exit 1
        fi
    ) 9>"$work/kernels/.lock"
}

# virtme-ng: the machine's own vng, else the pinned release in a virtualenv here
vng=$(command -v vng || true)
if [ -z "$vng" ]; then
    row=$(pinned virtme-ng tool)
    url=$(cut -f4 <<<"$row"); sha=$(cut -f5 <<<"$row"); version=$(cut -f2 <<<"$row")
    wheel="$work/kernels/$(basename "$url")"
    fetch "$url" "$sha" "$wheel" || exit 2
    (
        flock 9
        if [ ! -x "$work/vng-venv/bin/vng" ]; then
            python3 -m venv --system-site-packages "$work/vng-venv" && "$work/vng-venv/bin/pip" install --quiet --no-deps --no-index "$wheel"
        fi
    ) 9>"$work/kernels/.lock" || die "could not install virtme-ng $version into $work/vng-venv"
    vng="$work/vng-venv/bin/vng"
fi
say "vng: $vng ($("$vng" --version 2>&1 | head -1))"

# extract a .deb without root
deb_extract() {
    local deb=$1 into=$2
    mkdir -p "$into"
    if command -v dpkg-deb >/dev/null 2>&1; then
        dpkg-deb -x "$deb" "$into"
    else
        local member
        member=$(ar t "$deb" | grep '^data\.tar' | head -1)
        (
            cd "$into" || exit 1
            case "$member" in
                *.zst) ar p "$deb" "$member" | tar --zstd -x -f - ;;
                *.xz)  ar p "$deb" "$member" | tar -J -x -f - ;;
                *.gz)  ar p "$deb" "$member" | tar -z -x -f - ;;
                *)     ar p "$deb" "$member" | tar -x -f - ;;
            esac
        )
    fi
}

# prepare NAME: print "IMAGE<TAB>MODULES" for a kernel, preparing it once
prepare() {
    local name=$1 dir="$work/kernels/$1"
    local image_row modules_row source_row
    image_row=$(pinned "$name" deb-image); modules_row=$(pinned "$name" deb-modules); source_row=$(pinned "$name" source)
    if [ -n "$image_row" ]; then
        local version; version=$(cut -f2 <<<"$image_row")
        local image_deb
        image_deb="$work/kernels/$(basename "$(cut -f4 <<<"$image_row")")"
        local modules_deb
        modules_deb="$work/kernels/$(basename "$(cut -f4 <<<"$modules_row")")"
        fetch "$(cut -f4 <<<"$image_row")" "$(cut -f5 <<<"$image_row")" "$image_deb" || return 1
        fetch "$(cut -f4 <<<"$modules_row")" "$(cut -f5 <<<"$modules_row")" "$modules_deb" || return 1
        (
            flock 9
            if [ ! -f "$dir/.prepared" ]; then
                rm -rf "$dir/root"
                deb_extract "$image_deb" "$dir/root" && deb_extract "$modules_deb" "$dir/root" && touch "$dir/.prepared"
            fi
        ) 9>"$work/kernels/.lock" || return 1
        local modules="$dir/root/lib/modules/$version"
        [ -d "$modules" ] || modules="$dir/root/usr/lib/modules/$version"
        printf '%s\t%s\n' "$dir/root/boot/vmlinuz-$version" "$modules"
        return 0
    fi
    if [ -n "$source_row" ]; then
        local version; version=$(cut -f2 <<<"$source_row")
        local tarball
        tarball="$work/kernels/$(basename "$(cut -f4 <<<"$source_row")")"
        fetch "$(cut -f4 <<<"$source_row")" "$(cut -f5 <<<"$source_row")" "$tarball" || return 1
        (
            flock 9
            if [ ! -f "$dir/.prepared" ]; then
                for tool in gcc flex bison bc; do
                    command -v "$tool" >/dev/null 2>&1 || { say "building $name needs $tool"; exit 1; }
                done
                rm -rf "$dir/src" && mkdir -p "$dir/src" && tar -xJf "$tarball" -C "$dir/src" --strip-components=1 || exit 1
                # the options the rows need, built in, so the guest loads no module
                ( cd "$dir/src" && nice "$vng" --build \
                    --configitem CONFIG_BPF_SYSCALL=y --configitem CONFIG_BPF_JIT=y \
                    --configitem CONFIG_XDP_SOCKETS=y --configitem CONFIG_VETH=y \
                    --configitem CONFIG_NET_NS=y --configitem CONFIG_IPV6=y \
                    --configitem CONFIG_PACKET=y ) > "$dir/build.log" 2>&1 || { say "build of $name failed: $dir/build.log"; exit 1; }
                touch "$dir/.prepared"
            fi
        ) 9>"$work/kernels/.lock" || return 1
        printf '%s\t%s\n' "$dir/src/arch/x86/boot/bzImage" "-"
        return 0
    fi
    say "kernels.tsv has no image or source row for $name"
    return 1
}

# ---------------------------------------------------------------------------
# build on the host, unprivileged: the BPF objects with the exact command in
# server_xdp/Makefile, the helpers with test/xdp_vm/Makefile

build_failed=0
xdp_src="$run/build/server_xdp"
mkdir -p "$xdp_src"
# the same flattened set the library embeds (cmake/XdpSources.cmake)
while IFS= read -r source; do
    cp "$repo/$source" "$xdp_src/" || build_failed=1
done < <(grep -oE '(server_xdp|sdk)/[A-Za-z0-9_.]+' "$repo/cmake/XdpSources.cmake")
make -C "$xdp_src" -s server_xdp.o > "$run/build/server_xdp.log" 2>&1 || build_failed=1
[ -f "$xdp_src/server_xdp.o" ] && cp "$xdp_src/server_xdp.o" "$run/build/objects/"

# every other BPF program in the checkout: any C file declaring SEC("..."),
# compiled with the same clang line
bpf_line=$(sed -n 's/^\t\(clang .*-target bpf.*\)$/\1/p' "$repo/server_xdp/Makefile" | head -1)
while IFS= read -r source; do
    [ "$source" = "$repo/server_xdp/server_xdp.c" ] && continue
    name=$(basename "$source" .c)
    line=${bpf_line//server_xdp.c/$source}
    line=${line//server_xdp.o/$run/build/objects/$name.o}
    line=${line//\$(shell \$(CC) -dumpmachine)/$(cc -dumpmachine)}
    ( cd "$(dirname "$source")" && eval "$line -I$repo/sdk" ) >> "$run/build/server_xdp.log" 2>&1 || build_failed=1
done < <(grep -rl --include='*.c' 'SEC("' "$repo" 2>/dev/null | grep -v -e '/build/' -e '/build-' -e '/test/xdp_vm/' | sort)

# the judge applies the standard to the address families the program
# receives: IPv6 only once server_xdp.c reads the IPv6 ethertype
receive_ipv6=0
grep -Eq 'htons\( *ETH_P_IPV6 *\)' "$repo/server_xdp/server_xdp.c" && receive_ipv6=1
make -C "$here" -s OUT="$run/build/bin" RECEIVE_IPV6="$receive_ipv6" > "$run/build/helpers.log" 2>&1 || build_failed=1

if [ "$build_failed" != 0 ]; then
    cat "$run/build/server_xdp.log" "$run/build/helpers.log"
    say "the host build failed; nothing was booted"
    exit 1
fi
say "built: $(find "$run/build/objects" -mindepth 1 -maxdepth 1 -printf '%f\n' | LC_ALL=C sort | tr '\n' ' ')"

# the committed corpus and manifest are the judge's own output for this
# program: crafting again must give the same frames and the same rows, or the
# manifest describes a different program and no row could mean anything
mkdir -p "$run/build/crafted"
"$run/build/bin/xdp_vm_replay" craft "$run/build/crafted" > "$run/build/crafted/rows.tsv" || die "xdp_vm_replay craft failed"
grep '^crafted_' "$repo/test/xdp_pcaps/manifest.tsv" > "$run/build/crafted/committed.tsv"
corpus_drift=0
diff -u "$run/build/crafted/committed.tsv" "$run/build/crafted/rows.tsv" > "$run/build/crafted/manifest.diff" || corpus_drift=1
for pcap in "$run/build/crafted"/*.pcap; do
    cmp -s "$pcap" "$repo/test/xdp_pcaps/$(basename "$pcap")" || { say "corpus: $(basename "$pcap") differs from the committed capture"; corpus_drift=1; }
done
if [ "$corpus_drift" != 0 ]; then
    cat "$run/build/crafted/manifest.diff"
    say "corpus: the committed manifest is not the judge's output for this program (receive_ipv6=$receive_ipv6); regenerate it with xdp_vm_replay craft; nothing was booted"
    exit 1
fi
say "corpus: committed manifest and captures match the judge (receive_ipv6=$receive_ipv6)"

# ---------------------------------------------------------------------------
# the kernels

kernel_names=$(grep -v '^#' "$here/kernels.tsv" | awk -F'\t' '$3 != "tool" { print $1 }' | uniq)
[ -n "$only_kernel" ] && kernel_names=$only_kernel

tab=$'\t'
rows="verifier_loads test_run_replay_matches_manifest native_attach_on_veth af_xdp_copy_bind_receives attached_replay_matches_manifest detach_leaves_no_program"
refused=0

for kernel in $kernel_names; do
    out="$run/$kernel"
    mkdir -p "$out"
    : > "$out/rows.tsv"
    if ! prepared=$(prepare "$kernel"); then
        say "$prepared"
        for r in $rows; do printf '%s\t%s\t%s\n' "$kernel" "$r" "FAIL refused or unprepared, never booted" >> "$out/rows.tsv"; done
        refused=2
        continue
    fi
    prepared=$(tail -1 <<<"$prepared")
    image=$(cut -f1 <<<"$prepared"); modules=$(cut -f2 <<<"$prepared")
    say "boot: $kernel ($image)"
    guest_args="$mode $kernel $out $run/build $repo $modules"
    timeout 1800 nice "$vng" --run "$image" --user root --memory 2G --cpus 2 "${kvm_flags[@]}" \
        --rwdir "$out" --exec "$here/guest.sh $guest_args" > "$out/console.log" 2>&1
    status=$?
    if [ "$mode" = rows ]; then
        for r in $rows; do
            grep -q "^$kernel$tab$r$tab" "$out/rows.tsv" || printf '%s\t%s\t%s\n' "$kernel" "$r" "FAIL never reported (vng exit $status; see $out/console.log)" >> "$out/rows.tsv"
        done
    fi
done

finish_snapshot || true

if [ "$mode" = record ]; then
    say "recorded captures, with manifest rows written from STANDARD.md:"
    for kernel in $kernel_names; do
        for pcap in "$run/$kernel"/recorded_*.pcap; do
            [ -f "$pcap" ] && "$run/build/bin/xdp_vm_replay" classify "$pcap"
        done
    done
    exit 0
fi

# host_untouched, judged once per run and reported against every kernel
finish_snapshot || true
for kernel in $kernel_names; do
    printf '%s\t%s\t%s\n' "$kernel" host_untouched "$host" >> "$run/$kernel/rows.tsv"
done

# the matrix
say ""
say "matrix ($run_id, acceleration $accel):"
failed=0
for kernel in $kernel_names; do
    while IFS=$'\t' read -r k r verdict; do
        printf '  %-18s %-36s %s\n' "$k" "$r" "$verdict"
        case "$verdict" in PASS*) ;; *) failed=1 ;; esac
    done < "$run/$kernel/rows.tsv"
    [ -f "$run/$kernel/verifier.txt" ] && grep '^verified_insns' "$run/$kernel/verifier.txt" | sed "s/^/  $kernel /"
done
say "  parity with the host tier: not yet available (the host build of the filter has not landed on this base)"
[ "$refused" != 0 ] && exit 2
exit $failed
