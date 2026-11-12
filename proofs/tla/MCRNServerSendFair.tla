------------------------- MODULE MCRNServerSendFair -------------------------
(***************************************************************************)
(* The small instances of RNServerSendFair.  MCRNServerSendFair.cfg and   *)
(* the Walk config are healthy clients only; the Dark and DarkTwo configs *)
(* add clients held by probe pacing under the v1.1.1 due test.  The       *)
(* Broken configs are the reversed witnesses: the rotation's two          *)
(* (FromZero, InPlace) and the v1.1.0 due test (DueIgnoresHold,           *)
(* DarkProbes).  The Short config is the code's open fault, recorded as a *)
(* failure: a client whose own budget refuses its due packet.             *)
(***************************************************************************)
EXTENDS RNServerSendFair
=============================================================================
