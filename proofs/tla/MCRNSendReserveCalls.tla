------------------------ MODULE MCRNSendReserveCalls ------------------------
(***************************************************************************)
(* The small instance of RNSendReserveCalls: two clients, a reserve of     *)
(* three in a queue of five.  The calls never end, so Bound stops a        *)
(* behaviour at three refusals and at seven frames staged since the budget *)
(* opened (the queue, refilled once by the transport).                     *)
(* MCRNSendReserveCalls.cfg is the code and passes.  The Broken configs    *)
(* are the reversed witnesses and must fail: RoomOnly and AbortReturns on  *)
(* ReserveKept, NoRefresh on NeverStarved.  Those four claim one frame at  *)
(* a time (Chunk 1).  MCRNSendReserveCallsChunk.cfg is the code with       *)
(* chunks of three in a queue of eight and passes;                         *)
(* MCRNSendReserveCallsBrokenTailLeak.cfg, the same instance, must fail on *)
(* NeverStarved.                                                           *)
(***************************************************************************)
EXTENDS RNSendReserveCalls

Bound == refused <= 3 /\ started + ownUsed <= MaxFree + 2
=============================================================================
