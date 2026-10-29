# Privileged Linux gate

The privileged Linux gate validates the XDP object with `test_xdp_filter` in
the project VM environment. It loads the program through `BPF_PROG_TEST_RUN`
and checks verifier acceptance plus the program's packet verdicts. It does not
attach a program to a host interface and does not create a host veth or network
namespace.

The nightly Linux job in `.github/workflows/nightly-soak.yml` builds the
embedded `server_xdp.o` and `test_xdp_filter` and runs
`bash tools/run_xdp_filter_guest.sh --artifact-dir build/xdp-filter-guest`
inside a four-CPU `virtme-ng` (`vng`) guest of the runner's matching kernel
(`$HOME/vm-kernels/vmlinuz-$(uname -r)` with `irqaffinity=0 isolcpus=1-3 nohz_full=1-3`).
Qualification requires all 18 registered cases to print `ok` with zero skips and
the exact final line `test_xdp_filter: 18 cases passed`:

- `ipv4_rocketnet_packet_redirects`
- `ipv6_rocketnet_packet_redirects`
- `ipv6_extension_headers`
- `ipv4_options`
- `ipv4_fragments`
- `ipv6_fragments`
- `ipv6_wrong_address_passes`
- `ipv6_short_header_drops`
- `ipv6_too_large_drops`
- `ipv6_icmpv6_passes`
- `ipv6_same_drops_as_ipv4`
- `config_layout_matches_header`
- `replay_drops_behind_the_window`
- `replay_never_writes_the_map`
- `replay_key_layout_matches_header`
- `xdp_on_link_reply_reaches_the_client`
- `xdp_create_fails_part_way_tears_down`
- `xsk_bind_falls_back_to_copy`

`tools/run_xdp_filter_guest.sh` fails closed when any guest prerequisite
(`vng`, `$HOME/vm-kernels/vmlinuz-$(uname -r)`, `/dev/kvm`, binary, or BPF
object) is absent, when any case skips or fails, when fewer or extra cases run,
when a named case is missing, when the exit status is non-zero, when the exact
`test_xdp_filter: 18 cases passed` summary line is absent (even if the binary
returns 0 after skips), or when any leftover interface, namespace, XDP program,
BPF pin, or process remains. It writes `head.txt`, `kernel.txt`,
`object.sha256`, `command.txt`, `guest.log`, `verifier.txt`, `cases.txt`,
`summary.txt`, and `cleanup.txt` to the artifact directory.

Run it only on an allocated Linux bench using `bash tools/run_xdp_filter_guest.sh`
and the repository's VM procedure in [docs/LINUX.md](LINUX.md). Record the
exact source head, kernel, command, exit code and complete output in the job
receipt. Verify afterward that the worker left no attached XDP program or
temporary interface. A source review or an unprivileged unit run is not a
substitute for this gate.
