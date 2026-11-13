# Package M4 (channel-slots): notes for R and for C

Branch `models/v1.1.x/channel-slots`, on base `integration/v1.1.54`
(4f2960e5f). The models, cfgs, READMEs and the records rows of RNChannel,
RNChannelTypes, RNChannelQueues, RNChannelQueuesLayout, RNScheduler,
RNHeldOwnership, RNOrderingWrap, RNOpenAddress and RNSlots are changed on the
branch. What follows belongs to other packages.

## For R: roadmap entries (sexp text for docs/roadmap.sexp; not applied here)

```
  (item "rn-message-open-model" :group "proof-debt" :area "proofs"
   :title "RNMessageOpen.tla is cited but does not exist"
   :text "sdk/rocketnet_message_object.h, sdk/rocketnet_message_server.cpp and tests/wide_fill_cases.cpp cite proofs/tla/RNMessageOpen.tla for the one open start per connection (open.pending, rn_message_open_set and rn_message_open_close, the per-line opens count and last index as atomics, fill threads opening and closing on different connections at once). No such module exists; the citations are re-pointed to RNSlots StartPending and FinishPending (per connection since MM048) and RNFillPhase. A model of the open start with the fill threads' concurrent opens and closes and the atomics, bound to the code by a trace, is owed."
   :date "2026-11-13" :origin "models audit MM049; package M4 channel-slots")

  (item "rn-channel-queues-trace" :group "proof-debt" :area "proofs"
   :title "RNChannelQueues and RNChannelQueuesLayout have no trace binding"
   :text "RNChannelQueues now models two channel FIFOs, CONFIG's push to the front, the pop and the rejoin-if-more of rn_ordered_write, with a reversed witness for each invariant, and RNChannelQueuesLayout carries the same at the links. Neither is bound to the code by a trace check (the rule of 2026-11-07); until one is, neither layer locks."
   :date "2026-11-13" :origin "models audit MM038-MM041; package M4 channel-slots")
```

## For R: missing READMEs

- `proofs/tla/README-RNChannelQueues.md` does not exist. Its content is in
  the module header and the `rn-channel-queues` and
  `rn-channel-queues-unreliable` COVERAGE notes on this branch.
- `proofs/tla/README-RNHeldOwnership.md` does not exist. Content: the module
  header and the `held-ownership` COVERAGE note.

## For R: README.md and docs/TLA-INVENTORY.md lines

- `docs/TLA-INVENTORY.md:47` (ordering index width), the clause "a packet is
  deferred only when its records' deliveries do not fit" overclaims since
  MM044. New text for that clause: "a packet is deferred only when `Bound`
  does not fit, the channel's sequenced hold counted whenever its release can
  take a slot (the reservation is conservative by up to that hold;
  `NoOverReservation`)".
- `docs/TLA-INVENTORY.md:45` (message receive views): unchanged in meaning;
  optionally add "an update with a view held takes it back and continues".
- `proofs/tla/README.md`: the group table has no line for `rnslots`,
  `rnchanneltypes`, `rnchannelqueues` or `rnheldownership` (only
  `rnchannelqueueslayout` and `rnorderingwrap`). If the table is meant to be
  complete, suggested lines:
  - `rnslots`: Connection-tagged batches, region lifetime and object updates;
    an update with views held takes them back and continues; one pending
    start per connection (README-RNSlots.md).
  - `rnchannelqueues`: Two channel FIFOs and the ready list as sequences:
    CONFIG at the front once, pop and rejoin-if-more, the unreliable FIFO's
    expiry (module header).

## For R: other packages' docs

- `docs/OUTGOING-BUILD.md:16` says "One application reservation may be
  pending per object". The code allows one per connection
  (`rn_message_object_start`, `open.pending`, sdk/rocketnet_message_server.cpp
  around 1487-1497). New text: "One application reservation may be pending
  per connection; starts on different connections are independent, so fill
  threads each hold one."
- `proofs/tla/README-RNMessageViews.md:11` names `UpdateRequiresReleasedViews`
  as RNSlots checking the views as a set; the name is kept and now means no
  view is carried into an update (the update takes held views back). No edit
  needed unless that README describes the update as refused.

## For C: comment-only re-citations in sdk/ and tests/

MM049, RNMessageOpen.tla does not exist:

- `sdk/rocketnet_message_object.h:231`: "(proofs/tla/RNMessageOpen.tla)" ->
  "(proofs/tla/RNSlots.tla StartPending, FinishPending: one pending start per
  connection)".
- `sdk/rocketnet_message_server.cpp:540`: "proofs/tla/RNMessageOpen.tla Open
  and Close: a connection's one open start," -> "proofs/tla/RNSlots.tla
  StartPending and FinishPending: a connection's one open start (the
  concurrent opens and the atomics are not modelled; roadmap
  rn-message-open-model),".
- `sdk/rocketnet_message_server.cpp:1487`: "one start pending per connection
  (proofs/tla/RNMessageOpen.tla): a" -> "one start pending per connection
  (proofs/tla/RNSlots.tla StartPending): a".
- `tests/wide_fill_cases.cpp:9`: "(proofs/tla/RNMessageOpen.tla)" ->
  "(proofs/tla/RNSlots.tla StartPending)".
- `tests/wide_fill_cases.cpp:738`: "one open message per client
  (RNMessageOpen)" -> "one open message per client (RNSlots StartPending)".

MM039, RNChannelQueues Rotate:

- `sdk/rocketnet_channel.cpp:1937` and the "Rotate" citation near 1953: no
  edit needed. RNChannelQueues `Rotate` is now remove-the-ready-head, pop its
  FIFO head, rejoin the back if more is queued, which is what the code there
  does (the pop is `rn_channel_queue_pop`, the rejoin test at 1970).

MM044, `NeverDeferWithRoom` renamed `NoOverReservation`:

- `sdk/rocketnet_channel.cpp:2105`: "(proofs/tla/RNOrderingWrap.tla Receive,
  NeverDeferWithRoom)" -> "(proofs/tla/RNOrderingWrap.tla Receive,
  NoOverReservation)".
- `tests/test_unit_channel_ordered.cpp:1219-1220`: "The reservation is exact,
  never more (proofs/tla/RNOrderingWrap.tla NeverDeferWithRoom): with exactly
  the deliveries a packet needs left, it is taken." -> "The reservation is
  never more than the record's bound (proofs/tla/RNOrderingWrap.tla
  NoOverReservation): with exactly the deliveries a packet needs left, it is
  taken."
- `tests/test_unit_channel_reliable_sequenced.cpp:255`:
  "(proofs/tla/RNOrderingWrap.tla NeverDeferWithRoom)" ->
  "(proofs/tla/RNOrderingWrap.tla NoOverReservation)".

MM047 and MM050, RNSlots release and update:

- `sdk/rocketnet_message_server.cpp:1018`: "BeginUpdate: enters the
  transport phase once every view from the last update is released." ->
  "BeginUpdate: with every view released, the last update's deliveries
  expire and the transport phase begins."
- `sdk/rocketnet_message_server.cpp:1019`: "RejectUnreleasedUpdate: a view
  still held sets the not-released result this update returns." ->
  "RejectUnreleasedUpdate: a view still held is counted, taken back, and sets
  the not-released result; the update continues."
- `sdk/rocketnet_message_server.cpp:1106`: "(RNSlots Release's charge)" ->
  "(RNSlots BeginUpdate: the region's charge returns here, released or
  not)".
- `sdk/rocketnet_endpoint.cpp:1079`: "(RNSlots Release's charge)." ->
  "(RNSlots BeginUpdate: the region's charge returns here, released or
  not)."
- `sdk/rocketnet_message_server.cpp:2390`: "Release: a held view returns, and
  the region's charge for that message returns with it." -> "Release: a held
  view returns (marked released, the held count down); the region's charge
  returns at the next update's begin (RNSlots BeginUpdate)."
- `tests/test_unit_message_objects.cpp:1194-1196`: "RNSlots Release and
  RejectUnreleasedUpdate: release returns the room; update with a held view
  asserts in a debug build and is the named error in release;" -> "RNSlots
  Release, BeginUpdate and RejectUnreleasedUpdate: release returns the view
  and the update returns the room; an update with a held view asserts in a
  debug build, takes the view back and returns the named error in release;".
- `tests/test_unit_message_objects.cpp:2934`: "proofs/tla/RNSlots.tla's five
  reversed witnesses" -> "five of proofs/tla/RNSlots.tla's reversed
  witnesses".
- `tests/test_unit_message_objects.cpp:3095`: "ignore release
  (EveryReliableDelivered):" -> "no expiry at the update
  (EveryReliableDelivered, MCRNSlotsBrokenNoExpireLive.cfg):".
- `tests/test_unit_message_objects.cpp:3134-3135`: "update with held views
  (UpdateRequiresReleasedViews): a record delivered and held; the next update
  is refused by name, never run as if released" -> "update with held views
  (UpdateRequiresReleasedViews): a record delivered and held; the next update
  returns the named error and takes the view back, never carrying it".

## Cross-family rows

None. RNSequencingWrap `OpenEpoch` is named (not changed) as the model that
reaches RNChannelTypes' `DrainStale` branch at the half range;
RNSequencedHoldPool is named (not changed) for code-1 holds in RNHeldOwnership.
