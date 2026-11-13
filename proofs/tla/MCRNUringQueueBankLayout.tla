------------------- MODULE MCRNUringQueueBankLayout -------------------
(***************************************************************************)
(* The small instance of RNUringQueueBankLayout: two queues of two banks   *)
(* of three slots, two fill threads, three destinations (two on queue 0,   *)
(* one on queue 1), three starts per thread per fill, so a queue's claims  *)
(* run past its bank and are refused.  MCRNUringQueueBankLayout.cfg is the *)
(* passing control; each MCRNUringQueueBankLayoutBroken*.cfg must violate  *)
(* the invariant it names (README-RNUringQueueBankLayout.md).              *)
(***************************************************************************)
EXTENDS RNUringQueueBankLayout

\* destinations 1 and 2 hash to queue 0, destination 3 to queue 1
MCQueueOf(d) == IF d = 3 THEN 1 ELSE 0

\* a caller's pointer one byte before, at or after the one start returned
MCSkew == {-1, 0, 1}

MCSpec == Spec

=============================================================================
