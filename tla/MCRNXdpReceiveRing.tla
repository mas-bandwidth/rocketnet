--------------------------- MODULE MCRNXdpReceiveRing ---------------------------
(***************************************************************************)
(* Small instance of RNXdpReceiveRing. MCRNXdpReceiveRing.cfg is the       *)
(* receive loop, which cancels an unconsumed peek.                         *)
(* MCRNXdpReceiveRingBrokenNoCancel.cfg sets                               *)
(* CancelUnconsumed <- CancelBroken and must fail FramePartition.          *)
(***************************************************************************)
EXTENDS RNXdpReceiveRing

CancelBroken == FALSE
=============================================================================
