----------------------------- MODULE MCRNAckPhase -----------------------------
(***************************************************************************)
(* The small instance of RNAckPhase: two clients, two threads, three      *)
(* session packets over two updates, one reset, room for two deferred     *)
(* blocks so the third spills.  MCRNAckPhase.cfg is the committed run, the *)
(* phase as the code makes it.  The reversed witnesses each restore one   *)
(* defect and must fail:                                                   *)
(*   MCRNAckPhaseBrokenTwoAppliers.cfg     no one-thread check (ArrivalOrder) *)
(*   MCRNAckPhaseBrokenGatherByClient.cfg  gathers by client (PublicOrder) *)
(*   MCRNAckPhaseBrokenSweepBeforeGather.cfg sends first (NoSendBeforeApply) *)
(*   MCRNAckPhaseBrokenResetNoFlush.cfg    reset leaves blocks (PublicOrder) *)
(*   MCRNAckPhaseBrokenSpillNoFlush.cfg    spill leaves blocks (PublicOrder) *)
(*   MCRNAckPhaseBrokenCountsBeforeApply.cfg sums first (CountsExact)      *)
(*   MCRNAckPhaseBrokenDrainTaken.cfg      drain takes again (MessageOnce) *)
(*   MCRNAckPhaseBrokenTakeTwice.cfg       second call takes again (MessageOnce) *)
(* README-RNAckPhase.md gives the traces TLC printed and the check against *)
(* the code by hand.                                                       *)
(***************************************************************************)
EXTENDS RNAckPhase

CONSTANTS c1, c2, t1, t2

MCClientOrder == << c1, c2 >>
=============================================================================
