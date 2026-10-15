# test/

Test-only vendored dependencies. Nothing here ships in the sdk; the sdk
directory remains the complete drop-in set.

- `serialize.h` — mas-bandwidth/serialize 1.15, vendored so the serialize
  read of rocketnet.h (the C++ stream surface) is compiled and exercised by
  `bin/test_serialize`. Refresh it when the serialize pin moves.

## Running the suite

`./bin/test` prints test names and nothing else, because the suite captures
the library's log into a ring and dumps it only when a check fails — set
`ROCKETNET_TEST_VERBOSE=1` to skip the capture and watch the log live.

## The XDP kernel tier

`test/xdp_vm/` is the functional tier for the XDP program: `test/xdp_vm/run.sh`
boots a fresh VM per pinned kernel and, inside the guest only, loads every BPF
program through that kernel's verifier, replays the `test/xdp_pcaps/` corpus
through `BPF_PROG_TEST_RUN`, attaches the program to a veth in native mode,
binds an AF_XDP socket in copy mode, replays the corpus through the attached
program, and detaches. The host is snapshotted before and after and must be
byte-identical. Linux only, no root on the host, never part of `./bin/test`.
See `test/xdp_vm/README.md`.
