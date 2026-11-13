------------------------ MODULE MCSessionSendSequence ------------------------
(***************************************************************************)
(* The small instance that checks SessionSendSequence: sequences 1..5 and  *)
(* two fills that can each hold one taken sequence, overlapped in every    *)
(* order with one control send and one disconnect burst of two.            *)
(*                                                                         *)
(* MCSessionSendSequence.cfg is the rule and passes every invariant.       *)
(* MCSessionSendSequenceBrokenReadIncrement.cfg is the control send before *)
(* 2026-10-29 (read, seal, then increment) and must fail SequencesUnique.  *)
(* MCSessionSendSequenceBrokenBaseline.cfg reads the ack baseline as the   *)
(* next sequence less one and must fail BaselineIsHighestSealed.           *)
(* MCSessionSendSequenceBrokenBurstOne.cfg takes one sequence for the      *)
(* whole disconnect burst and must fail SequencesUnique.                   *)
(* README-SessionSendSequence.md gives the traces, replayed against the    *)
(* code by hand.                                                           *)
(***************************************************************************)
EXTENDS SessionSendSequence

MCSpec == Spec
=============================================================================
