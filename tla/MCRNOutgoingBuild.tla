------------------------- MODULE MCRNOutgoingBuild -------------------------
(***************************************************************************)
(* The small instance of RNOutgoingBuild: six messages, two of each kind  *)
(* (unreliable, sequenced, reliable), interleaved in send order, two       *)
(* transport frames of three units each, declared maxima of two, two, one, *)
(* two, one, one (so records pack, a short finish leaves room for the      *)
(* next, and a frame fills), one ack made due and a one-packet update      *)
(* allowance.  The shared packet quota resets only at BeginUpdate.  The    *)
(* reversed witnesses beside MCRNOutgoingBuild.cfg:                        *)
(* MCRNOutgoingBuildReversed.cfg (the pre-change copy step, NoCopy),       *)
(* MCRNOutgoingBuildOwnQuota.cfg (in-place packets on an allowance of      *)
(* their own, PacketQuota), MCRNOutgoingBuildNoReserve.cfg (no packet kept *)
(* for due reliable work, ReliableSentWithinOneUpdate),                    *)
(* MCRNOutgoingBuildBrokenMovesPriorRecord.cfg (a finish moves committed   *)
(* records, PriorRecordsUnmoved) and                                       *)
(* MCRNOutgoingBuildBrokenReuseCommitted.cfg (a reservation overlays       *)
(* committed bytes, BytesUnchangedInPlace).                                *)
(***************************************************************************)
EXTENDS RNOutgoingBuild

MCKind == << "U", "S", "R", "U", "S", "R" >>
MCFits == << TRUE, TRUE, TRUE, TRUE, TRUE, TRUE >>
MCDecl == << 2, 2, 1, 2, 1, 1 >>
CopyStepDesign   == FALSE
CopyStepReversed == TRUE
=============================================================================
