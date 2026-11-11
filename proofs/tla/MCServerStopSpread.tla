-------------------------- MODULE MCServerStopSpread --------------------------
(***************************************************************************)
(* The small instances of ServerStopSpread: two slots, four connections,   *)
(* bursts of two packets, a staging of three frames and a ring of four     *)
(* entries (twice the two a running server keeps waiting).  A stop owes up *)
(* to eight packets, more than the staging holds, so it spreads across     *)
(* steps.  MCServerStopSpread.cfg: every invariant.                        *)
(* MCServerStopSpreadLive.cfg: the stop finishes under fairness, with and  *)
(* without the transport's room.  Reversed witnesses:                      *)
(* MCServerStopSpreadBrokenSingleShot.cfg (FullBurstBeforeStopped),        *)
(* MCServerStopSpreadBrokenStopInFlight.cfg (NoSendCutAtStop),             *)
(* MCServerStopSpreadBrokenRingSmall.cfg and                               *)
(* MCServerStopSpreadBrokenSteadyCapOffByOne.cfg (RingHolds),              *)
(* MCServerStopSpreadBrokenUnreachableStalls.cfg (StopFinishes),           *)
(* MCServerStopSpreadBrokenNoAbandon.cfg (StopFinishesWithoutRoom) and     *)
(* MCServerStopSpreadBrokenNoReread.cfg (NoDropWhileRoomCanReturn).        *)
(***************************************************************************)
EXTENDS ServerStopSpread
=============================================================================
