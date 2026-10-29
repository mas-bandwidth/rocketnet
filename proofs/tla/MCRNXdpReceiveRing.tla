------------------------ MODULE MCRNXdpReceiveRing ------------------------
(***************************************************************************)
(* The small instance of RNXdpReceiveRing.  MCRNXdpReceiveRing.cfg is the  *)
(* design.  The reversed witness MCRNXdpReceiveRingBrokenNoCancel.cfg sets *)
(* CancelUnconsumed <- CancelUnconsumedBroken (the pass releases what it   *)
(* took and leaves the rest peeked, as the worker did before the cancel)   *)
(* and must fail FrameConservation.  The reversed witness                  *)
(* MCRNXdpReceiveRingBrokenFillOfTheFrames.cfg sets FillSize to the frames *)
(* (a fill ring of exactly the frames, as the server had before            *)
(* ROCKETNET_XDP_FILL_RING_SIZE) and must fail                             *)
(* FillNeverEmptyWhileFramesFree.  The reversed witness                    *)
(* MCRNXdpReceiveRingBrokenRefusedRefilled.cfg sets                        *)
(* RefusedRefilled <- RefusedRefilledBroken (a refused descriptor naming a *)
(* send frame goes back to the fill ring like any dropped frame, as the    *)
(* worker did before rocketnet_xdp_fill_return) and must fail              *)
(* FillHoldsOnlyReceiveFrames.                                             *)
(***************************************************************************)
EXTENDS RNXdpReceiveRing

CONSTANTS F1, F2, F3

CancelUnconsumedBroken == FALSE

RefusedRefilledBroken == TRUE
=============================================================================
