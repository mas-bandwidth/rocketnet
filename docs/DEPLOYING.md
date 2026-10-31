# Server transport deployment

Choose a transport with [TRANSPORTS.md](TRANSPORTS.md), then use the
[Linux](LINUX.md), [macOS](MACOS.md), or [Windows](WINDOWS.md) host guide.
This reference retains the native-server build and XDP provisioning procedure.
Commands run from the repository root. Provisioning and successful startup do
not establish throughput or latency qualification; see [Testing](TESTING.md).

## Server transports

Transport selection is runtime: one build carries every transport its
platform has. Linux production servers have xdp (privileged), gso (SO_REUSEPORT +
sendmmsg/recvmmsg + UDP GSO/GRO, non-root) and io_uring; macOS production
servers have msgx; Windows production servers have iocp and rio. Portable
servers and clients are also available on every platform. Production server
auto selection ranks portable after the native transports. Loopback is on every platform and is selected
only by name. The transport walks an ordered candidate
list when client connect or server start activates I/O. Public create parses
and resolves the configured list before that activation. The list rides the config's transport field or the
per-endpoint `ROCKETNET_SERVER_TRANSPORT` and `ROCKETNET_CLIENT_TRANSPORT`
environment variables (precedence: default < struct < env):

    ./bin/functional                                # auto: the platform's recommended ladder
                                                #   (production linux: xdp,gso,io_uring,portable — falls on refusal)
    ROCKETNET_SERVER_TRANSPORT=portable ROCKETNET_CLIENT_TRANSPORT=portable ./bin/functional
                                                #   portable server is available in both build modes
                                                # a one-element list is a pin: that transport
                                                #   or loud failure, never a silent fall
    ROCKETNET_SERVER_TRANSPORT=gso,portable ./bin/functional  # exactly these, exactly this order

Names known but absent on a platform — or not applicable to an endpoint, like
`io_uring` for a client — are skipped with a log line, so one fleet-wide value deploys
everywhere; a list whose every name is skipped falls to that endpoint's own auto
order, loudly, rather than bricking an endpoint the value was not addressing.
An explicit server pin of `portable` selects only portable in either build.
Unknown names refuse loudly at create. The granted transport is reported by
the transport name query on each client and server, and the startup log line. Queue-count bounds are per transport, refused and never
clamped: the public range is 1–1024 (zero selects a transport default),
while gso and io_uring accept at most 32. XDP also checks its memory and host
geometry before starting. The GSO transport still probes UDP_SEGMENT and UDP_GRO at
create and degrades to plain sendmmsg/recvmmsg batching where a kernel lacks them —
that degradation is internal to the transport, below the ladder; likewise the Darwin
batch pump's live-refusal pause. Select the portable server in either
build with `ROCKETNET_SERVER_TRANSPORT=portable`; select the client's portable
side with `ROCKETNET_CLIENT_TRANSPORT=portable` in either build.
Run `./bin/functional`
against each rung. The unit
suite, `./bin/test`, does not start a transport, so a transport pin does not change
what it asserts.

The SDK test suite temporarily clears config overrides derived from both
config tables, then restores them on return. `ROCKETNET_SERVER_TRANSPORT` and
`ROCKETNET_CLIENT_TRANSPORT` remain intentional test pins;
`ROCKETNET_TEST_VERBOSE` keeps its diagnostic meaning. Passing runs print test names only; a
test a platform does not build is not registered there. Failures retain the
captured diagnostic log.

### Windows IOCP server

Native transport authors: [Transport internals](TRANSPORT-INTERNALS.md) describes registration,
capabilities, ownership and the independently built extension example.

Select `transport = "iocp"` in the existing server config, or set
`ROCKETNET_SERVER_TRANSPORT=iocp`. The transport uses overlapped UDP with completion batches;
send buffers are recycled only after completion, and stop cancels and drains
outstanding operations. IPv6 binds accept both address families. Windows clients independently select `iocp` or `portable`; a server pin does
not change the client selection.

The public IOCP adapter ignores `num_xdp_queues` and always starts one
completion worker and one transmit worker. The completion worker owns the
single socket's receive completions. At the default `ROCKETNET_MAX_CLIENTS`
there are 128 posted receives, 4096 receive staging slots per worker buffer,
and 2048 shared send frames. Its three command banks hold references to that
frame pool; they do not multiply it. Staging and send capacities scale with
`ROCKETNET_MAX_CLIENTS`; see [Maximum clients](MEMORY.md#maximum-clients). These are resource bounds,
not timing or packet-rate requirements. A send frame still owned by the OS is
unavailable to the next fill, even after an application flush.

Windows production server `auto` tries iocp, rio, then portable; development
builds try portable, iocp, then rio. Windows CI runs both configurations, plus
completion-lifetime, allocation-failure and shutdown tests with IOCP selected.
Compilation and loopback correctness do not establish NIC
line-rate performance; that requires measurements on suitable Windows hardware.
The current implementation uses one UDP socket. Its behavior under the
intended workload must be measured before considering RSS-sharded sockets or
another backend for higher throughput.

## Deploying the XDP transport: provision, then verify-and-bind

The library does not perform host provisioning. `rocketnet_server_start( server )`
with the xdp transport is verify-and-bind: a non-mutating pre-flight checks
that the machine is provisioned and refuses by name — each unmet condition logs
the knob, the wanted value, and the script that sets it — and the library's own
acts are the runtime resources (its embedded BPF program loaded and attached, AF_XDP binds,
the memlock rlimit). Provisioning uses `scripts/rocketnet-provision`, an operator
step; no application using XDP reconstructs it. The
script's header carries the authoritative
condition list; the pre-flight checks exactly those conditions.

Cold, on a fresh box, as root:

    # 1. BOOT TIER (once, then reboot): edit GRUB_CMDLINE_LINUX in /etc/default/grub
    #    to carry: nosmt irqaffinity=0 isolcpus=1-15 nohz_full=1-15
    #    then: update-grub && reboot
    #    (build a recent vendor NIC driver where the distro's is too old — for
    #    Intel 700 series, i40e from github.com/mas-bandwidth/ethernet-linux-i40e)

    # 2. RUNTIME TIER (idempotent, re-run any time):
    scripts/rocketnet-provision eth0 7

    # 3. Check without changing anything (what the library's pre-flight will say):
    scripts/rocketnet-provision eth0 7 verify

    # 4. Run the server: the xdp rung grants only on a verified machine, and the
    #    auto ladder falls to gso with the reason named where it is not.

    # 5. AFTER A CRASH (a server that died without destroy): each instance
    #    leaves a bpffs pin root named for the instance --
    #    /sys/fs/bpf/rocketnet_xdp_<token>_<pid>_<starttime>_<bootid> -- and
    #    the library never adopts or removes state it did not create this
    #    run, so crash leftovers are removed DELIBERATELY:
    scripts/rocketnet-provision sweep
    #    The sweep judges staleness by the metadata in each root's name
    #    (foreign boot id, dead pid, or recycled pid sweeps; a live matching
    #    instance is left; an unparseable name is left and named for manual
    #    judgment) and prints its container caveat before acting: with bpffs
    #    shared across pid namespaces, sweep from the namespace that owns
    #    the servers. Leftovers are inert -- a colliding create refuses
    #    loudly and names this sweep.

    # (scripts/rocketnet-provision selftest holds the script's IRQ-name
    #  matcher to the C test's case set AND drives the R3/R7 enforcement
    #  functions end to end, both modes, against stubs replaying captured
    #  xdp-tools output (R3) and real-shaped ethtool output (R7) -- every
    #  fail site in both functions has a driving
    #  case, and
    #  the WHOLE SCRIPT is driven door-to-fail by a full-flow verify case
    #  (there is no level above the whole script). it
    #  needs no root and touches only a private temp dir it creates and
    #  removes. CI runs it on every push, so neither the matcher halves nor
    #  the enforcement paths can drift silently.)

This configuration — isolated cores 1-15, seven queues, per-queue IRQ pinning,
the busy-poll pair — is the 10 Gbps point, as measured on a provisioned 10 GbE
Linux host; the magnitude belongs to that machine, and the claim attaches to a
verified-provisioned box, never to the library alone. The isolated-core mask, the
queue count and the IRQ pinning scale together with NIC bandwidth: one queue
wants one isolated core, and higher-bandwidth NICs need more cores (the script
takes the range as a parameter; 1-15 is the default, not a constant). `isolcpus`
is a boot parameter — nothing at runtime can perform it, which is why the script
only verifies the boot tier. The config's `num_os_cpus` names the first core the
xdp workers pin from: workers occupy `num_os_cpus + queue`, so it should point
at the isolated range.

Two honest limits of the IRQ-pinning check: drivers whose interrupt names do not
embed the interface name (mlx5-class) cannot be checked by name — both the
script and the pre-flight state that rather than passing it off as
nothing-to-pin — and drivers that add a misc interrupt carrying the bare
interface name (ixgbe-class) count one line beyond the queue count, so size the
isolated range for queues plus one there.

Honest labels: CI runs the committing xdp path (BPF program build/load/attach,
AF_XDP binds) only inside a virtme-ng guest, in the `xdp portable` cell of the
transport matrix, and `.github/workflows/nightly-soak.yml` runs
`bash tools/run_xdp_filter_guest.sh` inside a four-CPU virtme-ng guest on the
built `server_xdp.o` and `test_xdp_filter`, requiring all 18 `BPF_PROG_TEST_RUN`
and guest XDP cases to print `ok` with zero skips (`test_xdp_filter: 18 cases passed`;
see `ROOTGATE.md`). No CI leg attaches XDP to a real NIC, so that tier's claims
rest on a provisioned server. The pre-flight
supports both Ubuntu 22.04's stock libbpf 0.5 and newer libbpf/libxdp.
On 22.04 the build selects `bpf/xsk.h`, loads the embedded BPF object directly,
and uses the legacy XDP query/attach APIs. Attach refuses an occupied slot;
detach supplies the expected program fd so it cannot remove a replacement.
The modern libxdp path is unchanged. No process-global libbpf strict-mode
setting is changed. Installed CMake packages retain their selected provider.

### Ubuntu 22.04 build compatibility

Use the [Linux dependency instructions](BUILDING.md#linux), omitting
`libxdp-dev` on Ubuntu 22.04, and the ordinary CMake build. Configure
must report `XDP: using legacy AF_XDP APIs from libbpf (Ubuntu 22.04)`;
`XDP off` does not qualify an XDP build. The default build includes the embedded
BPF object, static/shared libraries, examples and XDP test executables. Run
`./bin/test_xdp_compat` for the legacy loader/attachment adapter contract checks
(no kernel operations), `./bin/test` for the unit suite, and
`python3 tools/check_package.py build` for relocated C/C++ consumers of both
libraries. Jammy's liburing 2.1 lacks multishot receive, so configure prints
`liburing found without multishot receive` and does not build the io_uring
transport: it is absent from the transport list, as in a build without
liburing, and the production ladder is `xdp -> gso -> portable`. XDP is
independent of it.

Compilation and linking in an Ubuntu 22.04 container qualify its toolchain and
userspace libraries only. The host supplies the container's kernel. Loading,
attaching, zero-copy support and performance on the original 5.15 kernel and
the deployment NIC require separate runtime qualification.
