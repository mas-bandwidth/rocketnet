--------------------------- MODULE MCRNWriterFinish ---------------------------
(***************************************************************************)
(* The small instance of RNWriterFinish: two writers, two clients, an     *)
(* update capacity of two frames, five starts. MCRNWriterFinish.cfg is the *)
(* committed run with the writer's finish as the code makes it on a       *)
(* staged transport, MCRNWriterFinishBudget.cfg the same with a send      *)
(* budget, MCRNWriterFinishBuffered.cfg on a buffered transport. Each      *)
(* MCRNWriterFinishBroken*.cfg is a reversed witness and must fail on the  *)
(* property its CASES.tsv row names.                                      *)
(***************************************************************************)
EXTENDS RNWriterFinish

\* the reversed witness MCRNWriterFinishBrokenWalkDrops.cfg: KeepLeft <- BrokenKeepLeft
BrokenKeepLeft == FALSE
=============================================================================
