-------------------------- MODULE MCServerStopSpread --------------------------
(***************************************************************************)
(* The small instances of ServerStopSpread: two slots, four connections,   *)
(* bursts of two packets, a staging of three frames and a ring of four     *)
(* entries (twice the two a running server keeps waiting).  A stop owes up *)
(* to eight packets, more than the staging holds, so it spreads across     *)
(* steps.  MCServerStopSpread.cfg: every invariant.                        *)
(* MCServerStopSpreadLive.cfg: the stop finishes under fairness.  Reversed *)
(* witnesses: MCServerStopSpreadBrokenSingleShot.cfg                       *)
(* (FullBurstBeforeStopped), MCServerStopSpreadBrokenRingSmall.cfg and     *)
(* MCServerStopSpreadBrokenSteadyCapOffByOne.cfg (RingHolds) and           *)
(* MCServerStopSpreadBrokenUnreachableStalls.cfg (StopFinishes).           *)
(***************************************************************************)
EXTENDS ServerStopSpread
=============================================================================
