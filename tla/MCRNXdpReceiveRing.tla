------------------------ MODULE MCRNXdpReceiveRing ------------------------
(***************************************************************************)
(* The small instance of RNXdpReceiveRing.  MCRNXdpReceiveRing.cfg is the  *)
(* design.  The reversed witness MCRNXdpReceiveRingBrokenNoCancel.cfg sets *)
(* CancelUnconsumed <- CancelUnconsumedBroken (the pass releases what it   *)
(* took and leaves the rest peeked, as the worker did before the cancel)   *)
(* and must fail FrameConservation.                                        *)
(***************************************************************************)
EXTENDS RNXdpReceiveRing

CONSTANTS F1, F2, F3

CancelUnconsumedBroken == FALSE
=============================================================================
