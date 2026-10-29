-------------------------- MODULE MCRNOpenAddress ---------------------------
(***************************************************************************)
(* The small instances of RNOpenAddress: four keys, every hash function, *)
(* so clusters wrap the array end.  MCRNOpenAddress.cfg is the clear-first *)
(* erase with the index full; MCRNOpenAddressClearLast.cfg the clear-last  *)
(* erase at N - 1 live.  Reversed witnesses: ...BrokenNoWrapTest.cfg       *)
(* (Findable) and ...BrokenClearLastFull.cfg (EraseEnds).                  *)
(***************************************************************************)
EXTENDS RNOpenAddress
=============================================================================
