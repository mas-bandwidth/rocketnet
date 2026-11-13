------------------- MODULE MCRNUringQueueBankLayout -------------------
(***************************************************************************)
(* The small instance of RNUringQueueBankLayout: two queues of two banks   *)
(* of three slots, two fill threads, three destinations (2 and 4 on queue  *)
(* 0, 3 on queue 1), three starts per thread per fill, so a queue's claims *)
(* run past its bank and are refused. MCRNUringQueueBankLayout.cfg is the  *)
(* passing control; each MCRNUringQueueBankLayoutBroken*.cfg must violate  *)
(* the invariant it names (README-RNUringQueueBankLayout.md).              *)
(***************************************************************************)
EXTENDS RNUringQueueBankLayout

MCSpec == Spec

=============================================================================
