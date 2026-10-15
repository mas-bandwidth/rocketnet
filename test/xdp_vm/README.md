# The XDP kernel tier

XDP is a whole-machine mode: attaching a program or binding an AF_XDP socket
changes the machine for everything else running on it. This tier never does
either on the machine that runs it. Every run boots its own VM per kernel,
and everything that attaches happens inside the guest, against the guest's
own kernel.

Linux only. It is a functional tier: it is not part of `./bin/test`, it is
not built by CMake, and it never runs on macOS or Windows.

## The one command

    test/xdp_vm/run.sh

Run it from a checkout as an ordinary user. It needs no root and no sudo.
`--kernel NAME` runs one column of the matrix, `--preflight` checks the
toolchain and stops, `--record` records the handshake captures (below).
`XDP_VM_WORK` names the work directory (default `build/xdp_vm`): fetched
kernels under `kernels/`, each run under `runs/<id>/`, with the console log,
the verifier log and every row's evidence per kernel.

Exit 0: every row passed on every kernel. Exit 1: a row failed. Exit 2: the
toolchain is missing or a pinned artifact was refused.

## The matrix

`kernels.tsv` pins each kernel by version, source URL and sha256:

- the game server machine's kernel, `7.0.0-31-generic`, as its distribution
  packages (the image and the module tree, extracted without root);
- the newest longterm kernel listed on kernel.org when the matrix was last
  pinned, `6.18.57`, built from its tarball with `vng --build` in the work
  directory, with the options the rows need built in.

An artifact whose sha256 differs from the pinned value is moved aside as
`.refused` and its kernel is never booted. The pinned virtme-ng release is
installed from its pinned wheel into a virtualenv in the work directory when
the machine has no `vng`. These are the only downloads the tier makes.

## The rows

Each row prints PASS or FAIL per kernel, with its evidence.

| Row | What it proves |
|---|---|
| `verifier_loads` | Every BPF program in the checkout (each C file declaring `SEC("...")`), built with the exact clang line in `server_xdp/Makefile`, loads through this kernel's verifier. Prints `verified_insns` per program; on a refusal, the verifier log tail. |
| `test_run_replay_matches_manifest` | Every frame of every capture in `test/xdp_pcaps/` runs through `BPF_PROG_TEST_RUN` (the XSKMAP retyped to a DEVMAP, as `test_xdp_filter` does) and returns the verdict and moves the one counter `manifest.tsv` names. |
| `native_attach_on_veth` | The program attaches to the receive end of a veth pair between two network namespaces in `XDP_MODE_NATIVE`, and `ip -d link` shows driver-mode `xdp`. A native refusal is a failed row carrying the kernel's message; there is no fall to SKB mode. |
| `af_xdp_copy_bind_receives` | An AF_XDP socket binds in `XDP_COPY` mode to queue 0 of that veth, sits in `server_xdp_socket_map`, and receives frames. |
| `attached_replay_matches_manifest` | Every corpus frame, sent from the client namespace through an AF_PACKET socket, is judged by the attached program: the frames the manifest redirects arrive on the AF_XDP socket byte-identical and in order, nothing else arrives, and the stats map counters equal the manifest's totals. |
| `detach_leaves_no_program` | After detach and socket close, the veth carries no XDP program, nothing is pinned under the guest's `/sys/fs/bpf`, and no XDP program stays loaded. |
| `host_untouched` | The host snapshot taken before the run (`ip -d link`, `ip netns`, `bpftool prog show`, `bpftool net`, `ls -laR /sys/fs/bpf`, `sysctl net.core net.ipv4`) is byte-identical after it. |

`host_untouched` is judged once per run. The snapshot is taken as the user
who runs it: where that user cannot list BPF programs, the bpftool sections
record the refusal itself, so a change in them still shows. Anything else on
the machine that changes a link or a sysctl during the run fails the row
too; `runs/<id>/host_diff.txt` names the bytes.

## What it never touches

The guest sees the host's root filesystem read-only, with its writes in an
overlay, and the run's own directory as the only writable share. It gets no
host network device. The host side of `run.sh` builds objects and helpers,
fetches pinned artifacts and boots QEMU; it creates no link, namespace,
sysctl, bpffs pin or attached program, and runs nothing as root. Two runs at
once share nothing but the fetched, read-only kernel images; the fetch, the
extraction and the kernel build hold a lock, so two runs never prepare the
same kernel at once.

## The pieces

- `run.sh`: preflight, the host snapshot, the pinned fetches, the host
  builds, one VM per kernel, the matrix.
- `guest.sh`: the rows, inside the guest.
- `xdp_vm_attach.c`: the kernel side: `verify`, `testrun`, and `serve`
  (attach, AF_XDP copy bind, config map, receive, counters, detach).
- `xdp_vm_replay.c`: the packet side: `craft` the negative corpus, `classify`
  a capture against the standard, `send` a corpus through AF_PACKET,
  `capture` traffic, `compare` what arrived with the manifest.
- `Makefile`: builds the two helpers, linking libbpf and libxdp.
- `kernels.tsv`: the pinned matrix.

## The corpus

`test/xdp_pcaps/` holds the captures and `manifest.tsv`. The manifest's
verdicts and counters come from `STANDARD.md` (The Prefilter Header,
Structural checks, the Packet sizes table) through the judge in
`xdp_vm_replay.c`, which does not use the filter's own
`ROCKETNET_WIRE_SERVER_PACKET_VALID`, so the manifest judges the program
rather than copying it.

The judge applies the standard to the address families the program
receives. `server_xdp.c` receives IPv4 only, so every IPv6 frame, the
well-formed ones included, is `XDP_PASS` with no counter: it is the kernel
stack's business. `run.sh` builds the judge with `RECEIVE_IPV6=1` once
`server_xdp.c` tests for the IPv6 ethertype, crafts the corpus again into the
run directory, and boots nothing when the committed captures or the
manifest's crafted rows differ from that output; the fix is to regenerate
them with `xdp_vm_replay craft test/xdp_pcaps`.

- `crafted_ipv4.pcap`, `crafted_ipv6.pcap`, `crafted_other.pcap`: one or more
  frames per counter and per pass branch (other port, wrong address, IPv4
  options, not UDP, IPv6 extension headers and fragment, ICMPv6, VLAN, ARP,
  truncated headers), each breaking exactly one rule. Regenerate with
  `xdp_vm_replay craft test/xdp_pcaps`; the bytes are the same on every run.
- `recorded_ipv4.pcap`, `recorded_ipv6.pcap`, once recorded: the development-key client and
  server on the portable driver completing a handshake and exchanging
  payloads, captured on the server's veth inside the guest. Produce them with
  `make && test/xdp_vm/run.sh --record --kernel ubuntu-7.0.0-31`; the run
  prints each recorded frame's manifest row from `classify`, to be added to
  `manifest.tsv` with the captures.
