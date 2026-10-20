------------------------- MODULE MCRNDriverSendStaging -------------------------
(***************************************************************************)
(* The small instance of RNDriverSendStaging: four frames, staging for    *)
(* three, a disconnect burst of up to two.  MCRNDriverSendStaging.cfg is  *)
(* the committed run, with the rule on;                                   *)
(* MCRNDriverSendStagingBrokenDisconnect.cfg restores the defect and must *)
(* fail. MCRNDriverSendStagingBrokenRefusesSend.cfg restores publication  *)
(* of both XDP generations and violates NoRefusedSend. README-RNDriverSendStaging.md gives the traces TLC printed and  *)
(* the check against the code by hand.                                    *)
(***************************************************************************)
EXTENDS RNDriverSendStaging
=============================================================================
