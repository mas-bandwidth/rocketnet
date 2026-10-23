--------------------------- MODULE MCRNServerCrypto ---------------------------
(***************************************************************************)
(* Small instance of RNServerCrypto. MCRNServerCrypto.cfg is the rule     *)
(* (NeverWaits, overwrite off). Two reversed witnesses:                   *)
(*   MCRNServerCryptoBrokenWait.cfg       GameWait      WaitedStaysClear  *)
(*   MCRNServerCryptoBrokenOverwrite.cfg  Overwrite     Accounted         *)
(***************************************************************************)
EXTENDS RNServerCrypto
=============================================================================
