--------------------------- MODULE MCRNMsgxSendRuns ---------------------------
(***************************************************************************)
(* The checked instances of RNMsgxSendRuns: a bank of four, two slots and  *)
(* the shared socket, generations 0 and 1, two clients.  Slot 0 is open at *)
(* generation 1 and slot 1 at generation 0, so a stamp at (0, 0) or (1, 1) *)
(* is stale and the other two resolve.                                      *)
(***************************************************************************)
EXTENDS RNMsgxSendRuns

MCOpenGen(s) == IF s = 0 THEN 1 ELSE 0

=============================================================================
