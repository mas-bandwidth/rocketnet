# RNSegmentRefusal: a refused segmented send stays with its destination

`RNSegmentRefusal.tla` models how the server's Linux transports answer a
UDP segmented send the kernel refuses for one destination: the gso
transport (`rocketnet_server_gso_flush` in
`sdk/rocketnet_server_gso_linux.cpp`) and the io_uring transport
(`rocketnet_server_uring_submit_phase` and `rocketnet_server_uring_reap_cqe`
in `sdk/rocketnet_server_uring_linux.cpp`), both through the table in
`sdk/rocketnet_server_gso.h`. It states rule 7 of
[t2.4](../../standard/TRANSPORT.md#t2.4). The `segment-refusal-gso` and
`segment-refusal-uring` rows in `COVERAGE.tsv` track the coverage.

## Why

The kernel accepts the `UDP_SEGMENT` probe at create, yet a route can still
refuse a segmented send for its destination: an IPsec policy (`EIO`), or a
device without segmentation offload behind the route (`EINVAL`,
`EOPNOTSUPP`). Before this rule the io_uring transport dropped the refused
run and turned segmentation off for every destination at the first
refusal, and the gso transport resent the run plain but turned segmentation
off for every destination after eight refusals with no accepted segmented
send between them. Either way one client's route took segmentation from
every other client for the server's life.

## The state

| Model | Code |
|---|---|
| `segOn` | `ROCKETNET_SERVER_URING_FEATURE_SEGMENT` in `features`; the gso tier while `disable_gso` has not been set |
| `table`, `Slot` | `rocketnet_segment_refused_t.destination` at its layout and `rocketnet_segment_refused_entry`; `Remembered(d)` is `rocketnet_segment_refused_contains` |
| `count` | `segment_refused.destinations`: refusals of a destination the table did not hold, saturating at `Threshold` |
| `everRefused` | not code state: the distinct destinations that refused, the measure `KernelFallback` reads |
| `accepted` | `segment_refused.accepted`: a segmented send was accepted since create |
| `Threshold` | `ROCKETNET_SEGMENT_CONSECUTIVE_REFUSAL_THRESHOLD` |
| `Refusing`, `KernelSegments` | the routes, outside the code |
| `streak` | the old gso count of refusals since the last accepted segmented send (`Mode = "consecutive"` only) |

## Actions

| Action | Code |
|---|---|
| `Run(d)` | a run to `d` built segmented while segmentation is on and `d` is not in the table, its accepted send setting `accepted`; otherwise built plain |
| `Refuse(d)` | the kernel refuses `d`'s segmented run: `rocketnet_segment_refused_add` remembers `d`, the run is resent plain (`rocketnet_server_gso_resend_plain`, `rocketnet_server_uring_resend_plain`), and segmentation turns off for the transport only when `count` reaches `Threshold` and none was ever accepted |
| `Done` | the bound on runs (environment action) |

The code's table is direct-mapped, and the model carries it at its layout:
a refusing destination that maps to an occupied entry displaces the one
there, which pays one refusal again on its next run, so `RefusedOnce` is
checked only where every destination has its own entry (`SlotDistinct`).
Every refusal of a destination the table did not hold counts toward the
fallback, a displaced destination's again included, so `count` is never
below the distinct destinations that refused. The table matches whole
addresses, so a destination that never refused is never in it.

## Properties

- `OtherDestinationUnaffected`: once a segmented send was accepted, no
  destination whose route accepts segmentation sends a run plain.
- `NothingDropped`: a refused run is resent, never dropped.
- `RefusedOnce`: each destination pays at most one refusal (no two
  destinations sharing an entry).
- `KernelFallback`: when `Threshold` distinct destinations refused and none
  was accepted (a kernel that cannot segment), segmentation is off, however
  the destinations share entries.

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
| `MCRNSegmentRefusalShared.cfg` | the code on a kernel that refuses both destinations, which share one entry | pass | 0 | 23 | 1.0 |
| `MCRNSegmentRefusalBrokenFillCount.cfg` | `Mode = "fillcount"`: the table before, counting only a refusal that filled an empty entry | `KernelFallback` | 12 | 5 | 1.0 |
| `MCRNSegmentRefusalShared.cfg` | the code on a kernel that refuses both destinations, which share one entry | pass | | | |
| `MCRNSegmentRefusalBrokenFillCount.cfg` | `Mode = "fillcount"`: the table before, counting only a refusal that filled an empty entry | `KernelFallback` | | | |

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

**BrokenFillCount**: `d1` refuses and fills entry 0 (`count` 1); `d2`, in
the same entry, refuses and displaces it without counting (`count` still 1).
Two distinct destinations refused on a kernel that cannot segment, and
segmentation is still on. In `rocketnet_segment_refused_add` before, the
count moved only when `entry->type == ROCKETNET_ADDRESS_NONE`; with eight
refusers, two of them in one entry, the count stayed at seven and the
fallback never fired (`test_segment_refused_shared_entry_still_reaches_the_fallback`
in `tests/test_unit_uring_codec.cpp` is that case on the code).
