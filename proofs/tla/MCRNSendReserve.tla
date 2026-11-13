-------------------------- MODULE MCRNSendReserve --------------------------
(***************************************************************************)
(* The small instance of RNSendReserve: two fillers, a reserve of two in a *)
(* queue of four, one own frame, two claims per filler, two retires, two   *)
(* phases.  MCRNSendReserve.cfg is the code with one-frame claims;         *)
(* MCRNSendReserveChunk.cfg (chunks of two, the guard at two fillers, a    *)
(* reserve of one in a queue of six) and MCRNSendReserveChunkNoGuard.cfg   *)
(* (chunks of two, no guard) are the code with chunk claims.  All three    *)
(* pass.  The Broken configs are the reversed witnesses and must fail:     *)
(* RoomOnly, SplitClaim, RoomFirst and OwnCountFirst on ReserveKept;       *)
(* NoRefresh, NoReread, IndexTest and NoFallback on NeverStarved;          *)
(* NoTailReturn on TailsNamed.                                             *)
(***************************************************************************)
EXTENDS RNSendReserve
=============================================================================
