------------------------------ MODULE MCRNRttFilter ------------------------------
(***************************************************************************)
(* The small instances of RNRttFilter: one tick is a third of a second   *)
(* (Probe = 3), the path's round trip 0 then 1 or 2 ticks, outages of up  *)
(* to 4 ticks.  MCRNRttFilter.cfg and MCRNRttFilterLive.cfg are the code;  *)
(* the Broken configs the reversed witnesses (v1.1.0's silence on any gap, *)
(* and no filter); MCRNRttFilterFirstAck.cfg is the first acknowledgement  *)
(* of a connection.                                                         *)
(***************************************************************************)
EXTENDS RNRttFilter
=============================================================================
