------------------------- MODULE MCRNIocpSendLanes -------------------------
(***************************************************************************)
(* The small instances of RNIocpSendLanes.  MCRNIocpSendLanes.cfg: two     *)
(* lanes of two operations, three destinations (two in lane 0: DestLaneAt  *)
(* {2, 5, 6} is 1 -> 0, 2 -> 1, 3 -> 0), six starts;                       *)
(* MCRNIocpSendLanesThree.cfg: three lanes of two, one destination each    *)
(* ({3, 7, 11}).  Reversed witnesses: MCRNIocpSendLanesBrokenAnyLane.cfg   *)
(* (no destination hash, InOrder), MCRNIocpSendLanesBrokenReleaseMod.cfg   *)
(* (release to i % Lanes, OwnLane) and                                     *)
(* MCRNIocpSendLanesBrokenIndexBits.cfg (one index bit for four            *)
(* operations, HandleUnique).                                              *)
(***************************************************************************)
EXTENDS RNIocpSendLanes, TLC
=============================================================================
