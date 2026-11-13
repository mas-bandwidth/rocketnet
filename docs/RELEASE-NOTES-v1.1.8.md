# RocketNet v1.1.8

Changes compared with the `v1.1.7` tag. A point release: no configuration
field, export, status value, packet layout or protocol version changes, and
v1.0.0, v1.1.0, v1.1.1, v1.1.2, v1.1.3, v1.1.4, v1.1.6, v1.1.7 and v1.1.8
endpoints interoperate in both directions.

- Tag: `v1.1.8`, on the merged `main` commit.
- Release: [GitHub release](https://github.com/mas-bandwidth/rocketnet/releases/tag/v1.1.8).

## Since v1.1.7

### Library

No behaviour on the wire changes. Files that the wire-change rule tracks by
path were edited, so this note lists every edit.

- The client protocol no longer keeps `session_nonce`. The challenge's commit
  wrote it, nothing read it, and the destroy-time wipe erased it. The client
  now holds only the four derived keys, as the server does.
- The channel layer drops two pieces of dead state: `rn_packet_item_t`'s
  `resend` flag, which was written by the ordered walk and read nowhere, and a
  retry in `rn_ordered_write`'s ready-list removal that could never succeed.
- The server converts a v4-mapped token address once, inside
  `rocketnet_server_address_is_advertised`, instead of twice.
- The client transport fills a missing I/O destination in one place, and
  `rocketnet_client_transport_packet_payload` drops a NULL check that the API
  guards above it already make.
- The bundled crypto's four build switches now carry the `ROCKETNET_` prefix:
  `ROCKETNET_X64`, `ROCKETNET_AVX`, `ROCKETNET_AVX2` and
  `ROCKETNET_CRYPTO_LOGS`. A build that set them under their old names must
  rename them.
- Comments now cite the RocketNet standard and the RFCs, not other libraries.

### Standard

- `standard/TRANSPORT.md` drops Appendix B, a comparison with an earlier protocol.
  The appendix stated no requirements, and every difference it listed remains
  specified in the section it named. RN-1 is otherwise unchanged.

### Tests and tools

- Every unit case must run at least one check, and every test executable
  refuses an argument it does not consume. A suite refuses a test name that
  it collects twice. `--list` names exactly the cases a run would start.
- The unit and functional suites can keep going past a failure and report
  every failing case. A case run alone with `--run` is held to its own time
  limit and memory bound, and reports its leaks under LeakSanitizer.
- The memory bound of a test plan now includes the SDK's own count of the
  bytes it allocates outside the allocator, such as AF_XDP rings.
- More unit cases cover:
  - every counter that no case had observed, each checked by an exact count;
  - a client on a host with no IPv6;
  - one XDP server serving both address families, and the XDP create's
    address refusals;
  - reused client slots and their receipts;
  - Windows sockets, job counts and RIO destroy.
- Loopback and XDP cases resend only a datagram the kernel counted as dropped;
  any other loss still fails.
- `rocketnet_fuzz_session_raw` is a new fuzz harness that drives the session
  with the client off its I/O decrypt copy. Every fuzz harness is held to a
  per-target floor of coverage and executed units.
- The differential fuzz target and the vendored third-party serializer header and its test
  are removed. RocketNet's tests stand on their own.
- `tools/replay_ci_steps.py` runs one job of a workflow's steps locally.
  `tools/mutation/run_mutants.py` exits 1 when a campaign judged nothing.
