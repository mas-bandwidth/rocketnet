------------------------- MODULE MCRNOutgoingBuild -------------------------
(***************************************************************************)
(* The small instance of RNOutgoingBuild: six messages, two of each kind   *)
(* (unreliable, sequenced, reliable), interleaved in send order, two       *)
(* transport frames and two acks made due.  MCRNOutgoingBuild.cfg is the   *)
(* design.  The reversed witness MCRNOutgoingBuildReversed.cfg sets        *)
(* CopyStep <- CopyStepReversed, the build before this change (a fitting   *)
(* unreliable message written into its region record and copied into the  *)
(* frame at send_packets), and must fail NoCopy.                           *)
(***************************************************************************)
EXTENDS RNOutgoingBuild

MCKind == << "U", "S", "R", "U", "S", "R" >>
MCFits == << TRUE, TRUE, TRUE, TRUE, TRUE, TRUE >>
CopyStepDesign   == FALSE
CopyStepReversed == TRUE
=============================================================================
