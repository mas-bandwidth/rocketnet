-------------------------- MODULE MCRNSendReserve --------------------------
(***************************************************************************)
(* The small instance of RNSendReserve: two fillers, a reserve of two in a *)
(* queue of four, one own frame, two frames per filler, two retires, two   *)
(* phases.  MCRNSendReserve.cfg is the code and passes.  The Broken        *)
(* configs are the reversed witnesses and must fail: RoomOnly, SplitClaim, *)
(* RoomFirst and OwnCountFirst on ReserveKept; NoRefresh, NoReread and     *)
(* IndexTest on NeverStarved.                                              *)
(***************************************************************************)
EXTENDS RNSendReserve
=============================================================================
