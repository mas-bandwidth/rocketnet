------------------------- MODULE MCRNOutgoingBuild -------------------------
(***************************************************************************)
(* The small instance of RNOutgoingBuild: six messages, two of each kind  *)
(* (unreliable, sequenced, reliable), interleaved in send order, two       *)
(* transport frames, one ack made due and a one-packet update allowance.  *)
(* The shared packet quota resets only at BeginUpdate.  The reversed      *)
(* witnesses beside MCRNOutgoingBuild.cfg: MCRNOutgoingBuildReversed.cfg  *)
(* (the pre-change copy step, NoCopy), MCRNOutgoingBuildOwnQuota.cfg      *)
(* (in-place packets on an allowance of their own, PacketQuota) and       *)
(* MCRNOutgoingBuildNoReserve.cfg (no packet kept for due reliable work,  *)
(* ReliableSentWithinOneUpdate).                                          *)
(***************************************************************************)
EXTENDS RNOutgoingBuild

MCKind == << "U", "S", "R", "U", "S", "R" >>
MCFits == << TRUE, TRUE, TRUE, TRUE, TRUE, TRUE >>
CopyStepDesign   == FALSE
CopyStepReversed == TRUE
=============================================================================
