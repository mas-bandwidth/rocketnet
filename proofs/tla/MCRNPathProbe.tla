---------------------------- MODULE MCRNPathProbe ----------------------------
(***************************************************************************)
(* The small instances of RNPathProbe: sizes 1 .. 3 (3 the ceiling), the   *)
(* three-step client set {3, 2, 1}, every path capacity 0 .. 3 each way    *)
(* chosen at Init (so one run covers every path), one loss burst and one  *)
(* attacker send.  MCRNPathProbe.cfg is the passing control;              *)
(* MCRNPathProbeServerList.cfg gives the server a list without the        *)
(* client's middle size, MCRNPathProbeOneEntry.cfg is the default          *)
(* one-entry set (no probing), MCRNPathProbeLate.cfg runs the code's own   *)
(* timing (Timed = FALSE: late replies, safety only) and                   *)
(* MCRNPathProbeLossRow.cfg mixes three bursts at K = 2.                   *)
(* Each MCRNPathProbeBroken*.cfg must violate the property it names.       *)
(***************************************************************************)
EXTENDS RNPathProbe

=============================================================================
