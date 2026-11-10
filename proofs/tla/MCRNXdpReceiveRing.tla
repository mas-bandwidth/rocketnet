------------------------ MODULE MCRNXdpReceiveRing ------------------------
(***************************************************************************)
(* The small instance of RNXdpReceiveRing.  MCRNXdpReceiveRing.cfg is the  *)
(* design with threads that may stop for any length of time: three receive *)
(* banks, main's held bank and read cursor, a packet budget carried across *)
(* calls, an RX ring of two, a peek of up to two (so the worker looks at   *)
(* main's flag between two descriptors), a bank of two packets, two        *)
(* further tries for the mutex after a miss and a fill ring of twice the   *)
(* frames (ROCKETNET_XDP_FILL_RING_SIZE).  MCRNXdpReceiveRingBounded.cfg   *)
(* is the design with threads that do not stop, an RX ring of three and    *)
(* five packets: the bound on a missed claim (ClaimWithinSpin), the bound  *)
(* on how long the worker is kept off the mutex (WantedOnlyInSpin,         *)
(* NoDropWhilePaused), and liveness with weak fairness alone (SpecWeak).   *)
(*                                                                         *)
(* Each reversed witness restores one broken rule and must fail the        *)
(* property named with it:                                                 *)
(*   MCRNXdpReceiveRingBrokenFillOfTheFrames.cfg sets FillSize to the      *)
(*     frames (a fill ring of exactly the frames, as the server had before *)
(*     ROCKETNET_XDP_FILL_RING_SIZE): FillNeverEmptyWhileFramesFree.       *)
(*   MCRNXdpReceiveRingBrokenRefusedRefilled.cfg (a refused descriptor     *)
(*     naming a send frame goes back to the fill ring like any dropped     *)
(*     frame, as the worker did before rocketnet_xdp_fill_return):         *)
(*     FillHoldsOnlyReceiveFrames.                                         *)
(*   MCRNXdpReceiveRingBrokenDrainStale.cfg (a claim leaves the most       *)
(*     recently staged packet in the worker's next bank, as the pre-fix    *)
(*     worker publish deferred it): StagedBeforeClaimInView.               *)
(*   MCRNXdpReceiveRingBrokenSwapInCall.cfg (a swap while the call still   *)
(*     reads the bank): ViewStable.                                        *)
(*   MCRNXdpReceiveRingBrokenDoubleSwap.cfg (a second swap hands the bank  *)
(*     just claimed back unread): StagedBeforeClaimInView.                 *)
(*   MCRNXdpReceiveRingBrokenSwapUnread.cfg (a swap of a bank with a       *)
(*     retained tail): StagedBeforeClaimInView.                            *)
(*   MCRNXdpReceiveRingBrokenKeepCursor.cfg (a swap that does not reset    *)
(*     the read cursor): ConsumedInOrder.                                  *)
(*   MCRNXdpReceiveRingBrokenMissNoRetry.cfg (a missed claim that is never *)
(*     retried, the lost swap): StagedEventuallyConsumed.                  *)
(*   MCRNXdpReceiveRingBrokenMissDropsBank.cfg (a missed claim that gives  *)
(*     up main's bank): BankOneOwner.                                      *)
(*   MCRNXdpReceiveRingBrokenStaleBank.cfg (the worker stages into the     *)
(*     bank it read before it let go for main): HeldBankStable.            *)
(*   MCRNXdpReceiveRingBrokenStaleRoom.cfg (the worker ignores the swap    *)
(*     count, so a bank it saw full stays full to it): ReceiveNeverStalls. *)
(*   MCRNXdpReceiveRingBrokenSpareKept.cfg (the first claim takes a spare  *)
(*     and leaves it in the spare ring): BankOneOwner.                     *)
(*   MCRNXdpReceiveRingBrokenNoYield.cfg (the worker never reads main's    *)
(*     flag and holds the mutex for its whole staging loop, the unbounded  *)
(*     miss of the handoff before the flag): ClaimWithinSpin.              *)
(*   MCRNXdpReceiveRingBrokenGrabWhileWanted.cfg (the worker lets go and   *)
(*     takes the mutex again under main's raised flag): ClaimWithinSpin.   *)
(*   MCRNXdpReceiveRingBrokenStickyWant.cfg (main leaves its flag raised   *)
(*     when its tries run out): WantedOnlyInSpin.                          *)
(*   MCRNXdpReceiveRingBrokenLateClear.cfg (main leaves its flag raised    *)
(*     until the call ends, and the RX ring fills behind the worker):      *)
(*     NoDropWhilePaused.                                                  *)
(***************************************************************************)
EXTENDS RNXdpReceiveRing

CONSTANTS F1, F2, F3

RefusedRefilledBroken == TRUE

DrainStaleBroken == TRUE

SwapInCallBroken == TRUE

DoubleSwapBroken == TRUE

SwapUnreadBroken == TRUE

KeepCursorBroken == TRUE

MissNoRetryBroken == TRUE

MissDropsBankBroken == TRUE

StaleBankBroken == TRUE

StaleRoomBroken == TRUE

SpareKeptBroken == TRUE

NoYieldBroken == TRUE

GrabWhileWantedBroken == TRUE

StickyWantBroken == TRUE

LateClearBroken == TRUE
=============================================================================
