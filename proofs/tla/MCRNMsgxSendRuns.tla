--------------------------- MODULE MCRNMsgxSendRuns ---------------------------
(***************************************************************************)
(* The checked instances of RNMsgxSendRuns: a bank of four, two slots and  *)
(* the shared socket, generations 0 and 1, two clients.  Slot 0 is open at *)
(* generation 1 and slot 1 at generation 0 (OpenGenAt = {1, 2}: s * 2 + g), *)
(* so a stamp at (0, 0) or (1, 1) is stale and the other two resolve.  The *)
(* bank is every bank Init admits (Bank = {}), one stamp per client unless *)
(* MultiStamp.                                                              *)
(***************************************************************************)
EXTENDS RNMsgxSendRuns

=============================================================================
