# RNSegmentRefusal: a refused segmented send stays with its destination

`RNSegmentRefusal.tla` models how the server's segmenting transports answer a
UDP segmented send the kernel refuses for one destination: the gso
transport (`rocketnet_server_gso_flush` in
`sdk/rocketnet_server_gso_linux.cpp`), the io_uring transport
(`rocketnet_server_uring_submit_phase` and `rocketnet_server_uring_reap_cqe`
in `sdk/rocketnet_server_uring_linux.cpp`) and the Windows iocp transport
(`rocketnet_iocp_send_bank` and `rocketnet_iocp_post_send_run` in
`sdk/rocketnet_server_iocp_windows.cpp`, USO), all through the table in
`sdk/rocketnet_segment_refused.h`. It states rule 7 of
[t2.4](../../standard/TRANSPORT.md#t2.4). The `segment-refusal-gso`,
`segment-refusal-uring` and `segment-refusal-iocp` rows in `COVERAGE.tsv` track the coverage.

## Why

The kernel accepts the `UDP_SEGMENT` probe at create, yet a route can still
refuse a segmented send for its destination: an IPsec policy (`EIO`), or a
device without segmentation offload behind the route (`EINVAL`,
`EOPNOTSUPP`). Before this rule the io_uring transport dropped the refused
run and turned segmentation off for every destination at the first
refusal, and the gso transport resent the run plain but turned segmentation
off for every destination after eight refusals with no accepted segmented
send between them. Either way one client's route took segmentation from
every other client for the server's life. The Windows iocp transport kept
that behaviour until v1.1.8: a `WSAEINVAL` or `WSAEOPNOTSUPP` from the run's
`WSASendMsg` resent the run per datagram but cleared USO for every
destination at the first refusal. From v1.1.8 the three transports share
one table (`sdk/rocketnet_segment_refused.h`) and one rule; the table's
entry hash is keyed with a random seed drawn at each transport's create.

## The state

| Model | Code |
|---|---|
| `segOn` | `ROCKETNET_SERVER_URING_FEATURE_SEGMENT` in `features`; the gso tier while `disable_gso` has not been set; `ROCKETNET_IOCP_FEATURE_USO` in the iocp transport's `features` |
| `remembered` | the destinations in `rocketnet_segment_refused_t`, read by `rocketnet_segment_refused_contains` |
| `accepted` | `segment_refused.accepted`: a segmented send was accepted since create |
| `Threshold` | `ROCKETNET_SEGMENT_CONSECUTIVE_REFUSAL_THRESHOLD` |
| `Refusing`, `KernelSegments` | the routes, outside the code |
| `streak` | the old gso count of refusals since the last accepted segmented send (`Mode = "consecutive"` only) |

## Actions

| Action | Code |
|---|---|
| `Run(d)` | a run to `d` built segmented while segmentation is on and `d` is not in the table, its accepted send setting `accepted`; otherwise built plain |
| `Refuse(d)` | the kernel refuses `d`'s segmented run: `rocketnet_segment_refused_add` remembers `d`, the run is resent plain (`rocketnet_server_gso_resend_plain`, `rocketnet_server_uring_resend_plain`; iocp: `rocketnet_iocp_post_send_run` returns false and `rocketnet_iocp_send_bank` posts each frame), and segmentation turns off for the transport only when `Threshold` destinations refused and none was ever accepted |
| `Done` | the bound on runs (environment action) |

The code's table is direct-mapped: a refusing destination that maps to an
occupied entry displaces the one there, which pays one refusal again on
its next run. That changes only `RefusedOnce` and is not modelled. The
table matches whole addresses, so a destination that never refused is
never in it.

## Properties

- `OtherDestinationUnaffected`: once a segmented send was accepted, no
  destination whose route accepts segmentation sends a run plain.
- `NothingDropped`: a refused run is resent, never dropped.
- `RefusedOnce`: each destination pays at most one refusal.
- `KernelFallback`: when `Threshold` destinations refused and none was
  accepted (a kernel that cannot segment), segmentation is off.

## Cases

Group `rnsegmentrefusal` in `CASES.tsv`; the records are in `RUNS.tsv`,
measured on a 16-cpu linux-amd64 bench (Java 21.0.12.1, the pinned JAR,
two workers). Every case has `Threshold = 2` and six runs.

| Case | Change | Expected | Exit | Distinct | Seconds |
|---|---|---|---:|---:|---:|
| `MCRNSegmentRefusal.cfg` | the code; `g` accepts, `b` refuses | pass | 0 | 18 | 1.5 |
| `MCRNSegmentRefusalKernel.cfg` | the code on a kernel that refuses all three destinations | pass | 0 | 34 | 1.3 |
| `MCRNSegmentRefusalBrokenTransport.cfg` | `Mode = "transport"`: the io_uring code before | `OtherDestinationUnaffected` | 12 | 9 | 1.4 |
| `MCRNSegmentRefusalBrokenConsecutive.cfg` | `Mode = "consecutive"`: the gso code before | `OtherDestinationUnaffected` | 12 | 19 | 1.4 |
| `MCRNSegmentRefusalBrokenFirst.cfg` | `Mode = "first"`: the iocp code before v1.1.8 | `OtherDestinationUnaffected` | 12 | FIRST_DISTINCT | FIRST_SECONDS |

## Counterexamples, checked by hand against the code

**BrokenTransport**: `g`'s run is accepted segmented; `b`'s segmented run
is refused, and segmentation turns off for the transport with the run
dropped; `g`'s next run leaves plain. In the io_uring code before, the
refusal branch of `rocketnet_server_uring_reap_cqe` cleared
`ROCKETNET_SERVER_URING_FEATURE_SEGMENT` and counted the run
`SEND_DROPPED`; on a bench with an ESP security association toward one
client the other client had no segmented send after the first `EIO`.

**BrokenConsecutive**: `g`'s run is accepted segmented; `b` refuses twice
with no accepted segmented send between (`g` sending one packet an update
builds no segmented run), the count reaches the threshold, and `g`'s next
run leaves plain. In the gso code before, `segment_consecutive_refusals`
was reset only by an accepted segmented send; on the same bench, with the
good client at one packet an update, segmentation was off for every client
after eight refusals.
