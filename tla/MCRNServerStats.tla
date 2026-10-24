------------------------------ MODULE MCRNServerStats ------------------------------
(***************************************************************************)
(* The small instances of RNServerStats: a ring of 6 records (a window of  *)
(* 3), the windows sliding step by step and by jumps. MCRNServerStats.cfg  *)
(* is the sent side (in-order writes with gaps, acks in any order, spans   *)
(* from sequence bounds); MCRNServerStatsReceived.cfg is the received side *)
(* (any sequence at any time, spans from the time-ordered list).           *)
(* MCRNServerStatsBrokenEntering.cfg and MCRNServerStatsBrokenSeqTimes.cfg *)
(* are the reversed witnesses; README-RNServerStats.md has their traces.   *)
(***************************************************************************)
EXTENDS RNServerStats
=============================================================================
