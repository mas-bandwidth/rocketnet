# Notes for package R from M11 (xdp)

Branch models/v1.1.x/xdp. Everything here belongs to R to place; M11 edited
none of the files named below except where it says so.

## proofs/tla/README.md group lines

- `rnxdpgatewayrefresh`: add the failed resolve. Suggested text: "The XDP
  server's gateway refresh on the platform monotonic clock, its netlink
  resolve of the known gateway's neighbour entry (which may fail: the table
  keeps its MAC), and the handoff of the gateway MAC to the fill threads, a
  MAC in two halves (RNXdpGatewayRefresh.tla)."
- `rnxdpreplay`: unchanged text is still true; R may add "a reordering
  control (MaxInFlight = 2)".

## TLA inventory lines (new cases and properties)

- `MCRNXdpSendDrainBrokenNoClaim.cfg`: reversed witness, NeverAbandonDraining
  (a claimed staging buffer left out of the drain's idle test). All
  RNXdpSendDrain cfgs now set Modulus = 4 and MaxFrames = 6, so both ring
  indices wrap; new model variables `claimed` and `hardError`, environment
  action HardError.
- `MCRNXdpGatewayRefreshBrokenFailureWritesTable.cfg`: reversed witness,
  StagedMacUsable (a failed resolve's unusable answer written). New
  invariant StagedMacUsable; FreshAfterPeriod now exempts packets staged
  while the last resolve failed; new variables `failures`, `resolveFailing`.
- `MCRNXdpReceiveRingPendBinds.cfg` (passing control, the pending term binds)
  and `MCRNXdpReceiveRingBrokenPeekPastPending.cfg` (reversed witness,
  PendingInBounds); new rule switch PeekPendingRoom.
- `MCRNXdpReplayReorder.cfg`: passing control, MaxInFlight = 2 (a genuine
  packet overtaken). New invariant ForgetQueuedAfterWalk, in every passing
  control; MCRNXdpReplayBrokenNoForget.cfg now fails it instead of
  ReleasedSlotForgetsItsReplyMac. New definition ZeroMarkUnqueued in Commit
  (the code's hold_all_greetings).

## For M14 (tools/tla_trace_check.py, left as merged)

- xdp-replay (MM165): the walk's own bookkeeping cannot be declared as
  ghosts in the LAYERS row, because the checker requires a ghost to be read
  by no Next step and RNXdpReplay's Next reads session, sent, greeted, net,
  the failure counts and the reply ghosts. M11 declared them in the harness
  header (tests/tla_trace_xdp_replay.cpp) and in the COVERAGE row
  xdp-replay-trace instead: session, nextSession, sent, reached,
  seenBeforeRelease, greeted, net, dropped, forgetFailures, markFailures,
  held, heldReplies, heard, sentTo, refusedHeld, pendingMacForget, and the
  sess fields of kmap and pendingMark. If M14 adds a "walk-kept" (not ghost)
  field to the LAYERS row, this is its list.
- xdp-gateway-refresh (MM172): the row's untraced text is still right. The
  trace now fails about half the due resolves whatever the entry holds, and
  writes MaxFailures = 1000000 and FailureKeepsTable = TRUE into its cfg.

## RUNS.tsv repair outside M11's rows (cross-family)

The line merge of fix/v1.1.x/hostile-input-cost a4f131e97 into the base
doubled records in proofs/tla/RUNS.tsv: MCRNMsgxAddressIndex*,
MCRNMsgxSendRuns*, MCRNMsgxSocketTable*, MCSessionAcks* and their trace
self-checks appeared twice, and `tlacheck groups --stale` named
rnmsgxsendruns, rnmsgxsockettable and sessionacks stale. M11 rebuilt the
file from the three sides (each case's record from the side that changed it
against its merge base: 36 from a4f131e97, 11 from beb94b30d, the rest from
the base; no case changed on two sides), commit "RUNS.tsv: repair the line
merge". No record was measured or edited by hand; after it `groups --stale`
prints []. A later merge of the same heads should take the same records.

## Package C: comment-only re-citations

- sdk/rocketnet_xdp_replay_sink.h (rocketnet_xdp_replay_sink_mark, the comment inside it), current text cites
  "proofs/tla/RNXdpReplay.tla HoldGreetingUntilMarked, markOwed". New text:
  "a new session's zero mark the buffer refused: its key is not held
  anywhere, so the next publish holds every keep-alive (proofs/tla/RNXdpReplay.tla
  HoldGreetingUntilMarked, ZeroMarkUnqueued); the walk owes the mark and asks
  again".
- sdk/rocketnet_server_socket_linux.cpp (the comment above rocketnet_server_socket_hold_greetings), current text "drop each
  keep-alive to a session whose first mark (a zero) the kernel refused, or
  that the buffer could not queue". New text: "drop each keep-alive to a
  session whose first mark (a zero) the kernel refused, and every keep-alive
  after a commit whose walk had a zero mark the buffer could not queue
  (ZeroMarkUnqueued)".

## Roadmap entries (sexp text, for docs/roadmap.sexp)

```
  (item "xdp-fill-return-ownership" :group "proof-debt" :area "proofs"
   :title "XDP receive ring: a defective descriptor naming a held receive frame"
   :text "rocketnet_xdp_fill_return checks the receive region only, so a driver or kernel defect naming a receive frame already on the fill ring or in the RX ring is handed back again and sits on the fill ring twice. RNXdpReceiveRing leaves it out (KernelMisdeliver names only a send frame) and the trace never posts one. Decide whether an ownership check is worth its cost; model it if it is added."
   :date "2026-11-13" :origin "models review MM174; proofs/tla/RNXdpReceiveRing.tla; sdk/rocketnet_xdp_frame.h")
```

MM162 needs no roadmap entry: it is closed by
fix/v1.1.x/hostile-input-cost db2dd94c1, now merged into this branch, whose
skip asks the sink for a zero mark at the new address and whose
RNServerReplaySlots carries MarkWhileSkipped and GreetedSlotHasEntry.

## The merge of db2dd94c1 (settled here, by the module's owner)

Both sides added MCRNXdpReplayInFlight.cfg. db2dd94c1's (two addresses,
MaxSeq 2) keeps the name; this branch's reordering control (one address,
MaxSeq 4, a genuine packet overtaken until it falls behind the raised
mark, which MaxSeq 2 never reaches) is renamed MCRNXdpReplayReorder.cfg.
KernelFilter keeps db2dd94c1's no-entry drop and this branch's MA001
correction: `e = NoEntry \/ e.hw - p.seq >= KernelWindow` (db2dd94c1 had
`e.hw # 0 /\` before the window test, which treated a zero mark as
dropping nothing by fiat). QueueRefusable(accepted) is db2dd94c1's. Every
passing control now checks both LiveSessionNeverDroppedForNoEntry and
ForgetQueuedAfterWalk. The trace harness keeps this branch's connect
guard (no all-greeted condition, MM165) with db2dd94c1's comment. The two
COVERAGE rows xdp-replay-high-water the line merge left are one row; it
carries MA001's text (a zero mark is an entry in both, drops nothing below
2^31, the DEV sequence seam left out with the wrap), so the COVERAGE line
that was R's for MA001 is done in that row. The same row's "keep-alive,
payload, disconnect, ack, fragment" is MA004, R's.

For the inventory: MCRNXdpReplayReorder.cfg is a new passing control
(rnxdpreplay); MCRNXdpReplayInFlight.cfg's line is db2dd94c1's.
