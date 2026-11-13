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
- `MCRNXdpReplayInFlight.cfg`: passing control, MaxInFlight = 2 (a genuine
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

- sdk/rocketnet_xdp_replay_sink.h:53-55, current text cites
  "proofs/tla/RNXdpReplay.tla HoldGreetingUntilMarked, markOwed". New text:
  "a new session's zero mark the buffer refused: its key is not held
  anywhere, so the next publish holds every keep-alive (proofs/tla/RNXdpReplay.tla
  HoldGreetingUntilMarked, ZeroMarkUnqueued); the walk owes the mark and asks
  again".
- sdk/rocketnet_server_socket_linux.cpp:3247-3249, current text "drop each
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

  (item "xdp-replay-slot-reuse-model" :group "proof-debt" :area "proofs"
   :title "Replay greeting and freshness keyed by slot"
   :text "RNXdpReplay identifies a slot with its address, and RNServerReplaySlots keys by slot but has no greeting or freshness property. A slot reused at another address while its old forget is owed (the mark pass skips it; handed up as code-wrong, MM162) is checked by no model. With the code fix, extend a slot-keyed model with NewSessionNeverDroppedByOldMark and the greeting hold, and let the xdp-replay walk reuse slots."
   :date "2026-11-13" :origin "models review MM162; proofs/tla/RNXdpReplay.tla; proofs/tla/RNServerReplaySlots.tla; sdk/rocketnet_server_replay_marks.cpp")
```
