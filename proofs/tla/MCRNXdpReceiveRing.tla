------------------------ MODULE MCRNXdpReceiveRing ------------------------
(***************************************************************************)
(* The small instance of RNXdpReceiveRing.  MCRNXdpReceiveRing.cfg is the  *)
(* design: three receive banks, main's held bank and read cursor, a packet *)
(* budget carried across calls, an RX ring of two, a peek of up to two, a  *)
(* bank of two packets and a fill ring of twice the frames                *)
(* (ROCKETNET_XDP_FILL_RING_SIZE).  The reversed witness                   *)
(* MCRNXdpReceiveRingBrokenFillOfTheFrames.cfg sets FillSize to the frames *)
(* (a fill ring of exactly the frames, as the server had before            *)
(* ROCKETNET_XDP_FILL_RING_SIZE) and must fail                             *)
(* FillNeverEmptyWhileFramesFree.  The reversed witness                    *)
(* MCRNXdpReceiveRingBrokenRefusedRefilled.cfg sets                        *)
(* RefusedRefilled <- RefusedRefilledBroken (a refused descriptor naming a *)
(* send frame goes back to the fill ring like any dropped frame, as the    *)
(* worker did before rocketnet_xdp_fill_return) and must fail              *)
(* FillHoldsOnlyReceiveFrames.  The reversed witness                       *)
(* MCRNXdpReceiveRingBrokenDrainStale.cfg sets DrainStale <-               *)
(* DrainStaleBroken (a claim leaves the most recently staged packet in the *)
(* worker's next bank, as the pre-fix worker publish deferred it) and must *)
(* fail StagedBeforeClaimInView.                                           *)
(***************************************************************************)
EXTENDS RNXdpReceiveRing

CONSTANTS F1, F2, F3

RefusedRefilledBroken == TRUE

DrainStaleBroken == TRUE
=============================================================================
