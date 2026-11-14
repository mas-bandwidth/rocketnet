------------------------- MODULE MCRNAckApplyPhase -------------------------
(***************************************************************************)
(* The small instance of RNAckApplyPhase: two clients, the main thread and *)
(* two game threads, a queue of three entries and a range store of two     *)
(* ranges, any block arriving again. MCRNAckApplyPhase.cfg is the          *)
(* committed run, the deferred apply as the code makes it, with two blocks *)
(* a client of one range each (the range store forces the early gather);   *)
(* MCRNAckApplyPhaseOneBlock.cfg is the second, one block a client with no *)
(* ranges or one (the full queue forces it). Each                          *)
(* MCRNAckApplyPhaseBroken*.cfg turns one rule off and must fail the       *)
(* invariant its first lines name.                                         *)
(***************************************************************************)
EXTENDS RNAckApplyPhase
=============================================================================
