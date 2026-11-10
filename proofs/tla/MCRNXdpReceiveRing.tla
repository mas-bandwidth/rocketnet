------------------------ MODULE MCRNXdpReceiveRing ------------------------
(***************************************************************************)
(* The small instance of RNXdpReceiveRing.  MCRNXdpReceiveRing.cfg is the  *)
(* design: three receive banks, main's held bank and read cursor, a packet *)
(* budget carried across calls, an RX ring of two, a peek of up to two, a  *)
(* bank of two packets, one descriptor per hold of the receive mutex (so a *)
(* pass of two has a batch boundary where main's swap can come in) and a   *)
(* fill ring of twice the frames (ROCKETNET_XDP_FILL_RING_SIZE).           *)
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
(*   MCRNXdpReceiveRingBrokenMissNoRetry.cfg (a missed try that is never   *)
(*     retried, the lost swap): StagedEventuallyConsumed.                  *)
(*   MCRNXdpReceiveRingBrokenMissDropsBank.cfg (a missed try that gives up *)
(*     main's bank): BankOneOwner.                                         *)
(*   MCRNXdpReceiveRingBrokenStaleBank.cfg (the worker stages into the     *)
(*     bank it read before a batch boundary): HeldBankStable.              *)
(*   MCRNXdpReceiveRingBrokenStaleRoom.cfg (the worker ignores the swap    *)
(*     count, so a bank it saw full stays full to it): ReceiveNeverStalls. *)
(*   MCRNXdpReceiveRingBrokenSpareKept.cfg (the first claim takes a spare  *)
(*     and leaves it in the spare ring): BankOneOwner.                     *)
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
=============================================================================
