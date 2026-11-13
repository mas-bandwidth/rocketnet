------------------------- MODULE MCRNAckApplyPhase -------------------------
(***************************************************************************)
(* The small instance of RNAckApplyPhase: two clients, the main thread and *)
(* two game threads, two blocks a client an update, a queue of three       *)
(* entries and a range store of two ranges, so the early gather runs both  *)
(* ways. MCRNAckApplyPhase.cfg is the committed run, the deferred apply as *)
(* the code makes it; each MCRNAckApplyPhaseBroken*.cfg turns one rule off *)
(* and must fail the invariant its first lines name.                       *)
(***************************************************************************)
EXTENDS RNAckApplyPhase
=============================================================================
