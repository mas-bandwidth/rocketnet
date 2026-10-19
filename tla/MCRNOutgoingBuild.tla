------------------------- MODULE MCRNOutgoingBuild -------------------------
(***************************************************************************)
(* The small instance of RNOutgoingBuild: six messages, two of each kind  *)
(* (unreliable, sequenced, reliable), interleaved in send order, two       *)
(* transport frames, two acks made due and a one-packet update allowance. *)
(* The shared packet quota resets only at BeginUpdate.                    *)
(* MCRNOutgoingBuildReversed.cfg restores the pre-change copy step.        *)
(***************************************************************************)
EXTENDS RNOutgoingBuild

MCKind == << "U", "S", "R", "U", "S", "R" >>
MCFits == << TRUE, TRUE, TRUE, TRUE, TRUE, TRUE >>
CopyStepDesign   == FALSE
CopyStepReversed == TRUE
=============================================================================
