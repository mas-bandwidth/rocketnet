------------------------- MODULE MCRNTransportSendStaging -------------------------
(***************************************************************************)
(* The small instance of RNTransportSendStaging: four frames, staging for    *)
(* three, a disconnect burst of up to two.  MCRNTransportSendStaging.cfg is  *)
(* the committed run, with the rule on;                                   *)
(* MCRNTransportSendStagingBrokenDisconnect.cfg restores the defect and must *)
(* fail. MCRNTransportSendStagingBrokenRefusesSend.cfg restores publication  *)
(* of both XDP generations and violates NoRefusedSend. README-RNTransportSendStaging.md gives the traces TLC printed and  *)
(* the check against the code by hand.                                    *)
(***************************************************************************)
EXTENDS RNTransportSendStaging
=============================================================================
