# XDP max-size cases, IPv4 and IPv6, end to end in a virtme-ng guest

The recorded run of the two cases the `xdp portable` cell of ci.yml's
Transport family matrix step runs: the xdp server transport and the portable
client exchanging max-size packets both ways over a veth pair inside a network
namespace, as root inside a virtme-ng guest of the host's own kernel. Nothing
ran as root on the host, and no XDP program was attached and no AF_XDP socket
was bound on any host interface.

## Run

- Date (`date -u`): Sat Oct 24 02:20:17 AM UTC 2026
- Head: 3a04ed7a224b4567fc729b575b662fa275372dec (`3a04ed7a`, the land commit)
- Machine: `hostname` <XDP bench>, the dedicated XDP machine; `uname -r` 6.8.0-142-generic
- Guest kernel: <home>/vm-kernels/vmlinuz-6.8.0-142-generic (readable), the guest printed `uname -r` 6.8.0-142-generic
- /dev/kvm: writable (user in group kvm)
- virtme-ng: 1.40
- No NIC run was made, so no `ethtool -i` is recorded: the cases use a veth pair inside the guest.

## Commands

Build (Release, the Makefile's default):

    make -j4
    [100%] Built target test

Each case in its own guest, with the matrix step's vng arguments:

    timeout 120 vng --run ~/vm-kernels/vmlinuz-$(uname -r) --memory 1536M --cpus 2 \
      --append "irqaffinity=0 isolcpus=1 nohz_full=1" --rwdir "$PWD" -- \
      sh -c "uname -r; sudo ./bin/functional --run test_xdp_transport_max_size_both_ways_ipv4; echo case_rc=\$?"

    timeout 120 vng --run ~/vm-kernels/vmlinuz-$(uname -r) --memory 1536M --cpus 2 \
      --append "irqaffinity=0 isolcpus=1 nohz_full=1" --rwdir "$PWD" -- \
      sh -c "uname -r; sudo ./bin/functional --run test_xdp_transport_max_size_both_ways_ipv6; echo case_rc=\$?"

`./bin/functional --run <case>` fails with `test <case> cannot run on this host`
when the case's gate (rocketnet_test_xdp_veth_runs_here: root, and inside
virtme or opted in) is false, so a printed name and exit 0 is a real run.

## Output

test_xdp_transport_max_size_both_ways_ipv4 (vng exit 0):

    6.8.0-142-generic
    test_xdp_transport_max_size_both_ways_ipv4
    case_rc=0

test_xdp_transport_max_size_both_ways_ipv6 (vng exit 0):

    6.8.0-142-generic
    test_xdp_transport_max_size_both_ways_ipv6
    case_rc=0

## Results

| Case | Where | Result |
|---|---|---|
| test_xdp_transport_max_size_both_ways_ipv4 | root in a virtme-ng guest on the XDP bench, 6.8.0-142-generic | PASS, exit 0 |
| test_xdp_transport_max_size_both_ways_ipv6 | root in a virtme-ng guest on the XDP bench, 6.8.0-142-generic | PASS, exit 0 |
| ./bin/test (host, no root) | the XDP bench | PASS, `*** ALL TESTS PASSED ***`, exit 0 |

## ci.yml

A green ci.yml run on main whose Transport family matrix step printed
`PASS server=xdp client=portable` for both cases: none found. The workflow's
run list was not readable without gh (the API returns 404 to an
unauthenticated request) and no coordinator record names such a run.

## Verdict

Both XDP max-size cases, IPv4 and IPv6, pass end to end at 3a04ed7a on the XDP bench.
