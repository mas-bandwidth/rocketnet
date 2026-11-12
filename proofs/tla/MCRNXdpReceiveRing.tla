------------------------ MODULE MCRNXdpReceiveRing ------------------------
(***************************************************************************)
(* The small instance of RNXdpReceiveRing.  MCRNXdpReceiveRing.cfg is the  *)
(* design.  The reversed witness                                           *)
(* MCRNXdpReceiveRingBrokenPeekPastStaging.cfg sets                        *)
(* PeekStagingRoom <- PeekStagingRoomBroken (the peek asks for the pending *)
(* return's room alone, not the staging buffer's) and must fail            *)
(* StagingInBounds.  The reversed witness                                  *)
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

PeekStagingRoomBroken == FALSE

RefusedRefilledBroken == TRUE
=============================================================================
