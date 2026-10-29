----------------------------- MODULE MCRNBlocks -----------------------------
(***************************************************************************)
(* The small instance of RNBlocks: W = 4, F = 2 units (the stride), B = 8  *)
(* units, two messages: one single record of 2 units and one block of 5    *)
(* units, three fragments of 2, 2 and 1 units at consecutive numbers 1..3  *)
(* and ring offsets 2, 4, 6.  The link loses any copy ("arbitrary"),       *)
(* duplicates and reorders, capacity 2.  MCRNBlocks.cfg is the committed  *)
(* run with placement by index; MCRNBlocksBrokenPlaceByArrival.cfg is the  *)
(* reversed witness, PlaceByIndex = FALSE, which must fail                 *)
(* DeliveredEqualsSent (README-RNBlocks.md has the trace, replayed by      *)
(* hand).                                                                  *)
(***************************************************************************)
EXTENDS RNBlocks

MCUnits == <<2, 5>>
=============================================================================
