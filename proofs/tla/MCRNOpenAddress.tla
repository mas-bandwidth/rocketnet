-------------------------- MODULE MCRNOpenAddress ---------------------------
(***************************************************************************)
(* The small instances of RNOpenAddress: four keys, every hash function, *)
(* so clusters wrap the array end.  MCRNOpenAddress.cfg is the clear-first *)
(* erase with the index full (stronger than the code, whose indexes are    *)
(* twice capacity); MCRNOpenAddressTwiceCapacity.cfg the clear-first erase *)
(* at the code's load factor, four keys in eight cells;                    *)
(* MCRNOpenAddressClearLast.cfg the clear-last                             *)
(* erase at N - 1 live; MCRNOpenAddressHeldCount.cfg the held index's      *)
(* shape, W = 2 in 2W cells, with its separate counter.  Reversed          *)
(* witnesses: ...BrokenNoWrapTest.cfg (Findable),                          *)
(* ...BrokenClearLastFull.cfg (EraseEnds), ...BrokenHeldCountInsert.cfg    *)
(* and ...BrokenHeldCountErase.cfg (CountExact).                           *)
(***************************************************************************)
EXTENDS RNOpenAddress
=============================================================================
