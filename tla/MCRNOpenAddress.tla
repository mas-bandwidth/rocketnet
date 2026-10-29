-------------------------- MODULE MCRNOpenAddress ---------------------------
(***************************************************************************)
(* The small instance of RNOpenAddress: three keys in five cells, every  *)
(* hash function, so clusters wrap the array end.  MCRNOpenAddress.cfg is *)
(* the committed run; MCRNOpenAddressBrokenNoWrapTest.cfg is the reversed *)
(* witness, the committed file with NoWrapTest TRUE.                       *)
(***************************************************************************)
EXTENDS RNOpenAddress
=============================================================================
