------------------------ MODULE MCRNSendGatherLayout ------------------------
(***************************************************************************)
(* The small instance of RNSendGatherLayout at the code's table size: four  *)
(* slots, eight buckets (the power of two at least twice the slots), three *)
(* destinations whose home buckets are the last two, so the probe wraps,   *)
(* sizes 0 (aborted), 2 (short) and 3 (full), runs of at most three       *)
(* packets and seven bytes, so both caps cut runs.  Every assignment of a  *)
(* destination and a size to each slot is an initial state.               *)
(***************************************************************************)
EXTENDS RNSendGatherLayout

MCHash(d) == IF d = 1 THEN 7 ELSE 6

=============================================================================
