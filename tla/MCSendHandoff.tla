------------------------------ MODULE MCSendHandoff ------------------------------
(***************************************************************************)
(* The small instance of SendHandoff.  MCSendHandoff.cfg is the committed *)
(* run with every rule on; MCSendHandoffBrokenReopenFront.cfg and         *)
(* MCSendHandoffBrokenStalePointer.cfg each restore one defect and must   *)
(* fail.                                                                  *)
(***************************************************************************)
EXTENDS SendHandoff
=============================================================================
