------------------------------ MODULE MCRNServerStats ------------------------------
(***************************************************************************)
(* The small instances of RNServerStats: a ring of 6 records (a window of  *)
(* 3), the windows moving by steps and by jumps. MCRNServerStats.cfg is    *)
(* the sent side (in-order writes with gaps, acks in any order, spans from *)
(* sequence bounds); MCRNServerStatsReordered.cfg adds writes below the    *)
(* high-water mark; MCRNServerStatsReceived.cfg is the received side (any  *)
(* sequence at any time, spans from the time-ordered list);               *)
(* MCRNServerStatsAlias.cfg is the received side over a ring of 4, where a *)
(* late write can land more than a ring behind the high-water mark.        *)
(* MCRNServerStatsBrokenEntering.cfg, MCRNServerStatsBrokenCrossing.cfg,   *)
(* MCRNServerStatsBrokenSeqTimes.cfg and MCRNServerStatsBrokenAlias.cfg    *)
(* are the reversed witnesses; README-RNServerStats.md has their traces.   *)
(***************************************************************************)
EXTENDS RNServerStats
=============================================================================
