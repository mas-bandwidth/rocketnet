------------------------- MODULE MCRNIocpSendLanes -------------------------
(***************************************************************************)
(* The small instances of RNIocpSendLanes.  MCRNIocpSendLanes.cfg: two     *)
(* lanes of two operations, three destinations (two in lane 0), six        *)
(* starts; MCRNIocpSendLanesThree.cfg: three lanes of two.  Reversed       *)
(* witnesses: MCRNIocpSendLanesBrokenAnyLane.cfg (no destination hash,     *)
(* InOrder), MCRNIocpSendLanesBrokenReleaseMod.cfg (release to i % Lanes,  *)
(* OwnLane) and MCRNIocpSendLanesBrokenIndexBits.cfg (one index bit for    *)
(* four operations, HandleUnique).                                         *)
(***************************************************************************)
EXTENDS RNIocpSendLanes, TLC

CONSTANTS d1, d2, d3

DestLaneTwo == ( d1 :> 0 ) @@ ( d2 :> 1 ) @@ ( d3 :> 0 )
DestLaneThree == ( d1 :> 0 ) @@ ( d2 :> 1 ) @@ ( d3 :> 2 )
=============================================================================
