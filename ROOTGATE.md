# Privileged Linux gate

The privileged Linux gate validates the XDP object with `test_xdp_filter` in
the project VM environment. It loads the program through `BPF_PROG_TEST_RUN`
and checks verifier acceptance plus the program's packet verdicts. It does not
attach a program to a host interface and does not create a host veth or network
namespace.

Run it only on an allocated Linux bench using the repository's VM procedure in
[docs/LINUX.md](docs/LINUX.md). Record the exact source head, kernel, command,
exit code and complete output in the job receipt. Verify afterward that the
worker left no attached XDP program or temporary interface. A source review or
an unprivileged unit run is not a substitute for this gate.
