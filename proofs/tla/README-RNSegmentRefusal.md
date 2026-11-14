# RNSegmentRefusal: a refused segmented send stays with its destination

`RNSegmentRefusal.tla` models how the server's Linux transports answer a
UDP segmented send the kernel refuses for one destination: the gso
transport (`rocketnet_server_gso_flush` in
`sdk/rocketnet_server_gso_linux.cpp`) and the io_uring transport
(`rocketnet_uring_queue_worker_send_one` and
`rocketnet_uring_queue_worker_send_cqe` in
`sdk/rocketnet_uring_queue_worker.cpp`, with
`rocketnet_uring_queue_send_complete` and `rocketnet_uring_queue_send_plain`
in `sdk/rocketnet_uring_queue_send.cpp`), both through the table in
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
| `Queue` | each destination's queue (`rocketnet_send_queue_of`): the io_uring worker, or the gso lane, its sends go through |
| `FallbackScope` | what a fallback turns off: `"queue"`, the io_uring worker's own `config.segment`; `"transport"`, the gso tier's one `FEATURE_SEGMENT` bit (`rocketnet_server_gso_send_bank` clears it with `fetch_and`), so a lane that reaches the threshold turns segmentation off for every lane |
| `segOn` | each queue's segment flag (the worker's `config.segment`; the gso `FEATURE_SEGMENT` bit, one for every lane) |
| `gathered` | the queue's held bank was gathered with segmentation on. On io_uring the gather cuts runs at `max_segments`, which `rocketnet_uring_queue_send_plain` sets to 1 for later gathers only, and `rocketnet_uring_queue_worker_send_one` checks only the table, so the held bank's runs still leave segmented after the queue's fallback. On gso each flush pass reads `FEATURE_SEGMENT` once, so a lane whose pass read it before another lane cleared it still sends segmented; a lane's own fallback ends its pass, and its next pass is plain. With one gso lane the model allows a stale run the code does not send, which only widens what the invariants are checked over |
| `table`, `Slot` | each queue's `rocketnet_segment_refused_t.destination` at its layout and `rocketnet_segment_refused_entry`; `Remembered(d)` is `rocketnet_segment_refused_contains` |
| `count` | each queue's `segment_refused.destinations`: a refusal counts, saturating at `Threshold`, only when the table does not hold its destination (`rocketnet_segment_refused_add` tests `rocketnet_segment_refused_contains` first); `Mode = "everyanswer"` is the code before, which counted every call |
| `InFlight`, `inflight` | how many of one destination's segmented runs can be out before the first answer (2 in every case of the code). One destination has several runs when its packets in a bank pass `ROCKETNET_GSO_MAX_SEGMENTS` or the 65000-byte cap. The io_uring worker submits the held bank's runs while fewer than `ROCKETNET_URING_MAX_RUNS_IN_KERNEL` are in the kernel; the gso flush builds a whole `sendmmsg` batch, checking the table as it builds, before it sends, and after a refusal resends that run plain and sends on from the next message, which was built segmented |
| `PlainFails`, `plainDropped` | the plain resend after a refusal can fail with a hard error and drop its packet, counted `SEND_DROPPED` (`rocketnet_server_gso_resend_plain`; on io_uring the refused run comes back one run a packet, and a refused one-packet run is a failed send) |
| `everRefused` | not code state: the distinct destinations that refused, the measure `KernelFallback` reads |
| `accepted` | the transport's `segment_accepted` (`rocketnet_uring_queue_send_t`; `rocketnet_server_gso_t`): a segmented send was accepted since create, on any queue; a refusal reads it |
| `qAccepted` | each queue's own `segment_refused.accepted`, all a refusal read before (`AcceptedRule = "queue"` only) |
| `offAccepted` | not code state: a queue turned segmentation off after a segmented send was accepted, the measure `NoFallbackAfterAccepted` reads |
| `segAfterOff` | not code state: a run left segmented while its queue's flag was off |
| `Threshold` | `ROCKETNET_SEGMENT_CONSECUTIVE_REFUSAL_THRESHOLD` |
| `Refusing`, `KernelSegments` | the routes, outside the code |
| `streak` | the old gso count of refusals since the last accepted segmented send (`Mode = "consecutive"` only) |

## Actions

| Action | Code |
|---|---|
| `Run(d)` | the worker sends `d`'s next run, from the bank it holds or a bank it gathers now: segmented when that bank was gathered with segmentation on and `d` is not in the table (`rocketnet_segment_refused_contains`, checked at send), plain otherwise. A segmented run to a destination that accepts is accepted and sets `accepted`; to one that refuses it goes into the kernel (`inflight`) |
| `Refuse(d)` | the worker handles a refused answer for one of `d`'s runs: it reads `accepted`, `rocketnet_segment_refused_add` counts `d` if the table does not hold it and remembers it, the run is resent plain (`rocketnet_server_gso_resend_plain`; `rocketnet_uring_queue_send_complete` hands the run back one packet a run), and segmentation turns off for `OffScope(q)` when `count` reaches `Threshold` and none was ever accepted |
| `Done` | the bound on runs, every refused run answered (environment action) |

The code's table is direct-mapped, and the model carries it at its layout:
a refusing destination that maps to an occupied entry displaces the one
there, which pays a refusal again on its next run, so `RefusalsBounded` is
checked only where every destination has its own entry (`SlotDistinct`).
A refusal counts toward the fallback only when the table does not hold its
destination: a displaced destination counts again, and the later answers
for one destination's runs that were out with its first do not. So `count`
is never below the distinct destinations that refused, and with every
destination in its own entry never above them. The table matches whole
addresses, so a destination that never refused is never in it.

Refuse is one step: the worker reads `segment_accepted`, counts and
decides. The flag is relaxed with no ordering between workers, so
"accepted" is what the refusing worker sees at its read. An accepted run's
answer is taken at its send (`Run`); for io_uring that is earlier than the
reap that sets the flag, and the window it leaves is that same read
(docs/TRANSPORT-INTERNALS.md documents it).

## Properties

- `OtherDestinationUnaffected`: once a segmented send was accepted, no
  destination whose route accepts segmentation sends a run plain.
- `NoFallbackAfterAccepted`: once a segmented send was accepted on any queue,
  no queue's refusals turn its segmentation off (t2.4 rule 7 is the
  transport's: "none was ever accepted"), as the refusing worker sees the
  flag at its read. The code reads `segment_accepted`, decides, and (gso)
  clears the bit in separate steps with no ordering between workers, so an
  acceptance not yet reaped, or not yet visible to the refusing worker,
  does not count; the model's one `Refuse` step is that read (see above).
  A fallback taken before any was accepted stays, as the rule allows,
  which is why the two-queue controls do not check
  `OtherDestinationUnaffected`.
- `NothingDropped`: a refusal never drops its run; it is resent plain. A
  plain resend that then fails drops that packet, counted, as any plain
  send's hard error does; that is `plainDropped`, not this property.
- `RefusalsBounded`: each destination pays at most `InFlight` refusals, one
  for each of its runs that was in the kernel when the first answer came
  (no two destinations sharing an entry).
- `KernelFallback`: when `Threshold` distinct destinations of a queue refused
  and none was accepted (a kernel that cannot segment), that queue's
  segmentation is off, however the destinations share entries.

`MCRNSegmentRefusal.tla` adds the predicates the Reach cases fail:
`MCRefusedOnce` (no destination pays a second refusal), `NoSegmentedAfterOff`,
`NoPlainDrop`, and the property `NoFallbackOnFewerDests`: a queue turns
plain only once `Threshold` distinct destinations of it refused, the
contract the threshold comment in `sdk/rocketnet_server_gso.h` states. It
holds because a destination counts once while the table holds it, and is
checked on every case with each destination in its own entry and the
fallback scoped to the queue or one queue; the count before
(`Mode = "everyanswer"`) fails it.

## Cases

Group `rnsegmentrefusal` in `CASES.tsv`; the records are in `RUNS.tsv`,
measured on a 16-cpu linux-amd64 bench (Java 21.0.12.1, the pinned JAR,
two workers). Every case has `Threshold = 2` and six runs. The gso shape is
`FallbackScope = "transport"`, the io_uring shape `FallbackScope = "queue"`;
every case has `InFlight = 2` except BrokenTransport, BrokenTransportDrop,
BrokenConsecutive, BrokenFillCount and BrokenQueueAccepted, which have 1
(the defect each reverses needs no more).

| Case | Change | Expected | Exit | Distinct | Seconds |
|---|---|---|---:|---:|---:|
| `MCRNSegmentRefusal.cfg` | gso; `g` accepts, `b` refuses | pass | 0 | 76 | 0.7 |
| `MCRNSegmentRefusalKernel.cfg` | gso on a kernel that refuses all three destinations | pass | 0 | 1953 | 0.9 |
| `MCRNSegmentRefusalShared.cfg` | gso on a kernel that refuses both destinations, which share one entry | pass | 0 | 789 | 0.8 |
| `MCRNSegmentRefusalQueuesGso.cfg` | gso on two lanes: `d1` (lane 0) accepts; on lane 1 `d2` and `d3` refuse and `d4` accepts | pass | 0 | 1354 | 1.1 |
| `MCRNSegmentRefusalQueues.cfg` | io_uring on the same two queues | pass | 0 | 1098 | 1.2 |
| `MCRNSegmentRefusalQueuesKernel.cfg` | io_uring on two queues and a kernel that refuses every destination | pass | 0 | 6018 | 1.5 |
| `MCRNSegmentRefusalBrokenTransport.cfg` | `Mode = "transport"`: the io_uring code before | `OtherDestinationUnaffected` | 12 | 17 | 0.7 |
| `MCRNSegmentRefusalBrokenTransportDrop.cfg` | `Mode = "transport"`: the io_uring code before | `NothingDropped` | 12 | 6 | 0.7 |
| `MCRNSegmentRefusalBrokenConsecutive.cfg` | `Mode = "consecutive"`: the gso code before | `OtherDestinationUnaffected` | 12 | 39 | 0.7 |
| `MCRNSegmentRefusalBrokenFillCount.cfg` | `Mode = "fillcount"`: the table before, counting only a refusal that filled an empty entry | `KernelFallback` | 12 | 12 | 0.7 |
| `MCRNSegmentRefusalBrokenQueueAccepted.cfg` | `AcceptedRule = "queue"`: the io_uring code before, the accepted flag each queue's alone | `NoFallbackAfterAccepted` | 12 | 111 | 0.8 |
| `MCRNSegmentRefusalReachSecondRefusal.cfg` | io_uring, `g` and `b` on one queue | `MCRefusedOnce` | 12 | 20 | 0.7 |
| `MCRNSegmentRefusalReachOneDestFallback.cfg` | io_uring, `g` and `b` on one queue: `b`'s two runs in the kernel together count once (a passing case, named for the fallback it shows cannot happen) | pass | 0 | 47 | 0.7 |
| `MCRNSegmentRefusalBrokenEveryAnswer.cfg` | `Mode = "everyanswer"`: the count before 7c8280b2, every refused answer counted | `NoFallbackOnFewerDests` | 12 | 20 | 0.7 |
| `MCRNSegmentRefusalReachStaleSegmented.cfg` | io_uring on a kernel that refuses all three destinations | `NoSegmentedAfterOff` | 12 | 111 | 0.7 |
| `MCRNSegmentRefusalReachPlainDrop.cfg` | gso; `g` accepts, `b` refuses | `NoPlainDrop` | 12 | 8 | 0.7 |

## Counterexamples, checked by hand against the code

**BrokenTransport**: `g`'s run is accepted segmented; `b`'s segmented run
is refused, and segmentation turns off for the transport with the run
dropped; `g`'s next run leaves plain. In the io_uring code before, the
refusal branch of the reap cleared the transport's segment feature and
counted the run `SEND_DROPPED`; on a bench with an ESP security association
toward one client the other client had no segmented send after the first
`EIO`.

**BrokenTransportDrop**: `b`'s segmented run is refused and dropped, the
same code before, read against `NothingDropped`.

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

**BrokenQueueAccepted**: `d1`'s segmented run is accepted on queue 0;
`d2` and `d3`, on queue 1, refuse, and queue 1's own flag says none was
accepted, so with two refusers (the threshold) queue 1 stops segmenting
after the transport had shown it can segment. In the io_uring code before,
`rocketnet_segment_refused_add` read only the worker's `refused.accepted`;
`test_uring_queue_worker_accepted_on_one_queue_keeps_every_queue_segmenting`
is that case on the code, red there (`GSO_FALLBACKS` 1).

**ReachSecondRefusal**: two of `b`'s runs go into the kernel before either
answer, and both come back refused: `b` pays two refusals (and counts
once). In the code,
`rocketnet_uring_queue_worker_send_one` checks
`rocketnet_segment_refused_contains` at submit, before the first refusal is
reaped, and each refused completion reaches
`rocketnet_uring_queue_worker_send_cqe`.

**BrokenEveryAnswer**: two of `b`'s runs go into the kernel, both come back
refused, and each answer counts, so `count` reaches the threshold with only
`b` refused and nothing accepted and the queue turns plain. In
`rocketnet_segment_refused_add` before 7c8280b2 the count moved on every
call; `rocketnet_uring_queue_worker_send_one` checks the table only at
submit, so both runs were out before the first refusal was reaped
(`test_uring_queue_worker_one_destination_refusing_many_runs_keeps_segmentation`
in `tests/test_unit_uring_queue_worker.cpp` is that case on the code, red
before 7c8280b2). The
gso flush had the same over-count for one destination's runs cut at the
segment or byte cap inside one batch, there for every lane.

**ReachStaleSegmented**: refusals reach the threshold and the queue's
segmentation turns off; another destination's run, from the bank held
since before, still leaves segmented, is refused and resent plain. In the
io_uring code the held bank's runs were cut at the old `max_segments`, and
`rocketnet_uring_queue_worker_send_one` submits a run of more than one
packet segmented unless its destination is in the table.

**ReachPlainDrop**: `b`'s segmented run is refused and its plain resend
fails with a hard error: the packet is dropped and counted, as
`rocketnet_server_gso_resend_plain` does, while the refusal itself dropped
nothing (`NothingDropped` holds in the same trace).
