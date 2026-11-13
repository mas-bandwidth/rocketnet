# Notes for package R from M9 (gso-segmentation)

Branch `models/v1.1.x/gso-segmentation`. Rows of the 2026-11-13 models review
(MODELS-MERGED.md, P9). This branch edited the shared record files only in its
own modules' rows (CASES.tsv, RUNS.tsv, COVERAGE.tsv, BUDGETS.tsv). Everything
below is for R (or, where marked, for C or the docs package) to apply.

## Roadmap (docs/roadmap.sexp), MM136

```
  (item "gso-handoff-trace-layer" :group "review-v1-2" :area "proofs"
   :title "RNGsoHandoff has no trace layer"
   :text "RNGsoHandoff.tla, the gso worker's abstract hand-off (the send FIFO and its final producer at stop, the retries after shutdown, the receive buffer exchange), is checked by TLC at small sizes only: tests/tla_trace_gso.cpp binds RNGsoQueueBankLayout, the send staging layout, and no walk binds RNGsoHandoff to sdk/rocketnet_server_gso_linux.cpp. The review's MM135 (the retry count's reset on a partly sent attempt) is the kind of difference a trace would have shown. Enforcing it: a layer in tests/tla_trace_gso.cpp (or its own file) driving publish_send, take_send, send_step with a test socket that refuses on command, return_send, stop and begin_shutdown, writing RNGsoHandoff's variables after each call, with a plant that resets the retry count only on a new bank."
   :date "2026-11-13" :origin "Models review 2026-11-13, MM136 (O s2:F-GH-2)")
```

## proofs/tla/README.md

- Line 204, group `rnbankfifolayout`: the gso transmit banks now have their
  own control with the stop's final transfer. Suggested text: "The
  hand-written bank queues of the portable, gso (with the stop's final
  transfer of the producer), xdp, iocp and rio server transports, the buffer
  handoff and the iocp client at their layout (README-RNBankFifoLayout.md)."
- No group row names `rngsorecvfill`, `rngsoqueuebanklayout` or
  `rnmsgxsockettable` in the selected table; none is owed.

## Missing README

- `RNGsoRecvFill.tla` has no `README-RNGsoRecvFill.md`; its COVERAGE row
  (server-gso-recv-fill) and the module header carry the description. Owed
  under R's "missing READMEs".

## docs/TLA-INVENTORY.md

- Line 56 already lists `RNGsoRecvFill` and the bank queue modules; no new
  module, no change owed.

## Comment re-citations for package C (sdk/, comment lines only; each line number pinned to a commit of this branch)

1. `sdk/rocketnet_server_gso_linux.cpp:1098-1108` at `18be25f05`, the drain's comment. Two
   statements are no longer what the model checks (MM139, MM140):
   "so nothing read is dropped for want of room" and "Read: one message of a
   call". New text for 1103-1108:
   "Each call asks for at most the messages whose every segment fits, at
   ROCKETNET_GSO_RECV_MAX_SEGMENTS a message (rocketnet_server_gso_recv_vlen),
   fixed before the call, and stages every message it returns: nothing read is
   dropped for want of room unless a message carries more segments than that
   (a local sender's segmented send), and then only the segments past it are
   dropped, counted in RECEIVE_DROPPED_OVERFLOW.
   proofs/tla/RNGsoRecvFill.tla Read: one recvmmsg call, vlen read once before
   it (Vlen is rocketnet_server_gso_recv_vlen) and every returned message's
   segments staged from the fill index with no recount of the room;
   DropOnlyPastSeg; Arrive is the kernel queueing a GRO run on the socket
   between calls."
2. `sdk/rocketnet_server_gso.h:102-106` at `18be25f05`, rocketnet_server_gso_recv_vlen's
   comment: "so that every segment of every message it returns has a slot"
   holds only for messages of at most ROCKETNET_GSO_RECV_MAX_SEGMENTS
   segments (the comment above it, 94-98, says a local sender's longer run is
   counted). New text: "the messages one recvmmsg of a drain may ask for, so
   that every segment of every message of up to ROCKETNET_GSO_RECV_MAX_SEGMENTS
   segments it returns has a slot: ... never read only to be dropped
   (proofs/tla/RNGsoRecvFill.tla Vlen, DropOnlyPastSeg)."
3. `sdk/rocketnet_server_gso_linux.cpp:605` at `18be25f05`, take_send: "proofs/tla/RNBankFifoLayout.tla
   Take: the ready head advances % ROCKETNET_GSO_TX_BANKS." Suggested: "...;
   its second branch is the final one here, the producer taken at stop and
   left -1 (FinalLast)." (MM142; the action name is unchanged, so the
   citation check passes without it.)
4. `sdk/rocketnet_server_gso_linux.cpp:544-545` at `18be25f05`, publish_send:
   "proofs/tla/RNGsoQueueBankLayout.tla Flush: the producer turns over only onto a
   free bank, and the turn (the packet id's high part) counts it." Suggested
   addition: "A contended try-acquire, a stopping transport or no free bank
   keeps the producer (Flush's pick of -1)." (MM141; optional, the citation
   passes without it.)

5. `sdk/rocketnet_server_gso.h:153` at `18be25f05` says "the kernel's
   UDP_MAX_SEGMENTS is 64"; line 96 of the same file (and the
   server-gso-recv-fill COVERAGE row) say 128. Suggested: correct 153 to 128
   (found by M9's cold read; comment only).
6. Not for C, for R: `proofs/tla/MCRNBankFifoLayoutLifoExposed.cfg` (its header
   comment) still
   names the iocp and rio receive banks, which README-RNBankFifoLayout.md says
   no transport uses now. Left as is here, since editing it re-runs the case
   for a comment older than this branch; for R to fold into a later pass.

## For the docs package (docs/TRANSPORT-INTERNALS.md)

- MM143's `docs/TRANSPORT-INTERNALS.md:581-584` part (the segmentation
  refusal called transport-wide). MM143 is held on this branch (see below);
  the docs package takes the text when the RNSegmentRefusal correction lands.

## Rows held on this branch (the reviewed heads that touch these modules are being re-merged)

- RNGsoHandoff (msgx-receive-handoff-latency touches it): MM135, MM137,
  MM138. MM136 is the roadmap entry above.
- RNSegmentRefusal (uring-receive-rearm touches it): MM143, MM144, MM145.
