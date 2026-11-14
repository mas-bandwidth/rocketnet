------------------------- MODULE MCRNSegmentRefusal -------------------------
(***************************************************************************)
(* The small instances of RNSegmentRefusal, a threshold of two and six    *)
(* runs.  MCRNSegmentRefusal.cfg is the code with one destination that    *)
(* accepts segmentation and one whose route refuses it;                   *)
(* MCRNSegmentRefusalKernel.cfg is a kernel that refuses every            *)
(* destination; MCRNSegmentRefusalShared.cfg is that kernel with two      *)
(* destinations sharing one table entry.  The Broken configs are the     *)
(* code before and must fail: Transport and Consecutive                   *)
(* OtherDestinationUnaffected, FillCount KernelFallback                    *)
(* (README-RNSegmentRefusal.md).                                           *)
(***************************************************************************)
EXTENDS RNSegmentRefusal

\* every destination its own entry
SlotDistinct == CHOOSE f \in [Dests -> 0..(Cardinality(Dests) - 1)] :
                    \A x, y \in Dests : x # y => f[x] # f[y]

\* every destination in entry 0
SlotShared == [d \in Dests |-> 0]

\* every destination on one queue
QueueOne == [d \in Dests |-> 0]

\* destination "d1" on queue 0, the others on queue 1
QueueSplit == [d \in Dests |-> IF d = "d1" THEN 0 ELSE 1]
=============================================================================
