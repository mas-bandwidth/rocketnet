-------------------------- MODULE MCRNSendReserve --------------------------
(***************************************************************************)
(* The small instance of RNSendReserve: two threads, a reserve of two in a *)
(* queue of four, one own frame, two frames per thread, two retires, two   *)
(* openings.  MCRNSendReserve.cfg is the code and passes.  The Broken      *)
(* configs are the reversed witnesses and must fail: RoomOnly, SplitClaim, *)
(* RoomFirst and OwnCountFirst on ReserveKept; NoRefresh, NoReread,        *)
(* IndexTest and NoRetry on NeverStarved.                                  *)
(***************************************************************************)
EXTENDS RNSendReserve
=============================================================================
