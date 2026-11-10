-------------------------- MODULE MCRNOpenAddress ---------------------------
(***************************************************************************)
(* The small instances of RNOpenAddress: four keys, every hash function, *)
(* so clusters wrap the array end.  MCRNOpenAddress.cfg is the clear-first *)
(* erase with the index full (stronger than the code, whose indexes are    *)
(* twice capacity); MCRNOpenAddressTwiceCapacity.cfg the clear-first erase *)
(* at the code's load factor, four keys in eight cells;                    *)
(* MCRNOpenAddressClearLast.cfg the clear-last                             *)
(* erase at N - 1 live.  Reversed witnesses: ...BrokenNoWrapTest.cfg       *)
(* (Findable) and ...BrokenClearLastFull.cfg (EraseEnds).                  *)
(***************************************************************************)
EXTENDS RNOpenAddress
=============================================================================
