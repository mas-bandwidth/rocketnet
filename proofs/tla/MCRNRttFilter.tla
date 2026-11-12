------------------------------ MODULE MCRNRttFilter ------------------------------
(***************************************************************************)
(* The small instances of RNRttFilter: the probe time is 3 ticks (2 in   *)
(* the liveness run), the path's round trip 0 then 1, outages of up to 4  *)
(* ticks.  MCRNRttFilter.cfg and MCRNRttFilterLive.cfg are the code;  *)
(* the Broken configs the reversed witnesses (v1.1.0's silence on any gap, *)
(* and no filter); MCRNRttFilterFirstAck.cfg and                         *)
(* MCRNRttFilterUnowed.cfg are recorded expected failures of the code: the *)
(* first acknowledgement of a connection, and a sample from a copy whose   *)
(* number was already acknowledged; MCRNRttFilterStandard.cfg is m9.1.    *)
(***************************************************************************)
EXTENDS RNRttFilter
=============================================================================
