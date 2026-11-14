------------------------- MODULE MCRNSegmentRefusal -------------------------
(***************************************************************************)
(* The small instances of RNSegmentRefusal, a threshold of two and six    *)
(* runs.  The gso shape (FallbackScope "transport", InFlight 1):          *)
(* MCRNSegmentRefusal.cfg, one destination that accepts segmentation and  *)
(* one whose route refuses it; MCRNSegmentRefusalKernel.cfg, a kernel     *)
(* that refuses every destination; MCRNSegmentRefusalShared.cfg, that     *)
(* kernel with two destinations sharing one table entry;                  *)
(* MCRNSegmentRefusalQueuesGso.cfg, two lanes.  The io_uring shape         *)
(* (FallbackScope "queue", InFlight 2): MCRNSegmentRefusalUring.cfg,       *)
(* MCRNSegmentRefusalQueues.cfg and MCRNSegmentRefusalQueuesKernel.cfg.    *)
(* The Broken configs are the code before and must fail; the Reach        *)
(* configs show a case the passing ones allow is reached, and must fail    *)
(* the predicate below that says it is not (README-RNSegmentRefusal.md).   *)
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

\* reach predicates, each false in its Reach config's counterexample

\* no destination pays a second refusal (false once two of one
\* destination's runs were in the kernel together)
MCRefusedOnce == \A d \in Dests : refusals[d] <= 1

\* no run leaves segmented after its queue's flag turned off (false for a
\* run of a bank gathered before the fallback)
NoSegmentedAfterOff == ~segAfterOff

\* no plain resend after a refusal was dropped
NoPlainDrop == ~plainDropped

\* a queue is plain only once Threshold distinct destinations of that queue
\* refused: the threshold comment's contract, read with every destination
\* in its own entry (SlotDistinct) and FallbackScope "queue" or one queue
NoFallbackOnFewerDests ==
    \A q \in Queues :
        ~segOn[q] => Cardinality({d \in everRefused : Queue[d] = q}) >= Threshold
=============================================================================
