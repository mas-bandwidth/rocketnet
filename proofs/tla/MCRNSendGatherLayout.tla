------------------------ MODULE MCRNSendGatherLayout ------------------------
(***************************************************************************)
(* The small instance of RNSendGatherLayout at the code's table size: four  *)
(* slots, eight buckets (the power of two at least twice the slots), three *)
(* destinations whose home buckets are the last two, so the probe wraps,   *)
(* sizes 0 (aborted), 1 (short) and 3 (full), runs of at most three       *)
(* packets and seven bytes, so each cap cuts a run the other would let    *)
(* go on: four packets of one byte stop at the segment cap with four      *)
(* bytes, three of three bytes stop at the byte cap with two packets.     *)
(* Every assignment of a destination and a size to each slot is an        *)
(* initial state.                                                          *)
(***************************************************************************)
EXTENDS RNSendGatherLayout

MCHash(d) == IF d = 1 THEN 7 ELSE 6

\* the gather as its caller sees it (RNSendGatherCall, the spec the trace
\* check binds the code to): wherever the layout model ends, its order and
\* runs are that module's for the same bank
Call == INSTANCE RNSendGatherCall

MatchesCall == AtEnd(order = Call!GatherOrder /\ runs = Call!GatherRuns)

=============================================================================
