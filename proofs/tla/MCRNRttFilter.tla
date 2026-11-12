------------------------------ MODULE MCRNRttFilter ------------------------------
(***************************************************************************)
(* The small instances of RNRttFilter: the probe time is 3 ticks (2 in   *)
(* the liveness run), the path's round trip 0 then 1, outages of up to 4  *)
(* ticks.  MCRNRttFilter.cfg, Live, FirstAck and Unowed are the code; the  *)
(* Broken configs the reversed witnesses: v1.1.1's first-ack test and      *)
(* sampling (BrokenFirstAck, BrokenUnowed), v1.1.0's silence on any gap,   *)
(* and no filter.                                                           *)
(***************************************************************************)
EXTENDS RNRttFilter
=============================================================================
