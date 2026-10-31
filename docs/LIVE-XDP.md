# Live AF_XDP verification

AF_XDP is a Linux server transport selected with
`server_config.transport = "xdp"` or `ROCKETNET_SERVER_TRANSPORT=xdp`.
The transport follows the same public message, status, address-family and
real-time contracts as every other server transport. Its frames use the
layout constants in `sdk/rocketnet_constants.h`, and its XDP program accepts
only packets that pass the protocol's fixed header and UDP destination checks.

A deployment must provision a compatible Linux kernel, libbpf/libxdp, an
interface and queues that the process may bind, and enough locked memory for
the configured server capacity. Create or start reports a specific status when
those requirements are unavailable; a pinned `xdp` selection never silently
falls back. An ordered candidate list may name another transport explicitly.
See [LINUX.md](LINUX.md) and [TRANSPORTS.md](TRANSPORTS.md) for setup and
selection.

The short gates for this release are:

- build the XDP object and user-space transport;
- run the unit suite, including frame parsing and transport selection;
- run `test_xdp_filter` inside the project Linux VM procedure, never by
  attaching an unreviewed program to the host interface;
- run the ordinary functional transport cases supported by the allocated
  bench.

Earlier multi-host delivery and soak results are indexed by
[Testing](TESTING.md) and the [LAN proof record](runs/lan-1000-proof.md).
Those records apply to their recorded source revisions, workloads and copy-mode
or zero-copy tier. They do not qualify a new build, a different NIC, or the
100-microsecond main-call target. Further stress, throughput and profiling
campaigns require their own deployment-specific evidence.

## Shutdown evidence

The client `--matrix` command reports orderly disconnect cycles only. Each cycle
requests disconnect, observes completion, then destroys the client; its silent
cycle count stays zero. Skipping an explicit disconnect before destroy was never
valid evidence of silent client loss. Use a separate process-kill experiment, such
as the functional hard-killed-client test, to qualify that behavior. The synthetic
`--self-test-result` output tests the report parser and is not a live traffic run.
