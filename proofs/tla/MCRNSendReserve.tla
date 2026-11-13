-------------------------- MODULE MCRNSendReserve --------------------------
(***************************************************************************)
(* The small instances of RNSendReserve: two threads, two starts each, two *)
(* openings; each cfg's header gives its sizes.  MCRNSendReserve.cfg,      *)
(* MCRNSendReserveChunk.cfg, MCRNSendReserveSharedLine.cfg and             *)
(* MCRNSendReserveChunkNoGuard.cfg are the code and pass.                  *)
(* MCRNSendReserveCodeTailsNamed.cfg is the code and fails TailsNamed (a   *)
(* code defect handed up).  The Broken configs are the reversed witnesses  *)
(* and must fail: RoomOnly, SplitClaim, RoomFirst and OwnCountFirst on     *)
(* ReserveKept; NoRefresh, NoReread, IndexTest, NoRetry, NoFallback,       *)
(* BatchCount and NoTailReturn on NeverStarved.                            *)
(***************************************************************************)
EXTENDS RNSendReserve
=============================================================================
