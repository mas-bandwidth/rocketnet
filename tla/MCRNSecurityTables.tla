------------------------- MODULE MCRNSecurityTables -------------------------
(***************************************************************************)
(* The small instance of RNSecurityTables: two addresses, two tokens,      *)
(* one of them expiring at tick 3, tables at the production ratios         *)
(* reduced (history 2, admitted 2, two client slots), a verify budget      *)
(* of two per update.  MCRNSecurityTables.cfg is the fixed-code run;       *)
(* README-RNSecurityTables.md lists each reversed witness as edits to      *)
(* that file.                                                              *)
(***************************************************************************)
EXTENDS RNSecurityTables

MCExpire == [tk \in Tokens |-> IF tk = "te" THEN 3 ELSE 0]
MCCid == [tk \in Tokens |-> CASE tk = "te" -> 2 [] tk = "tf" -> 3 [] OTHER -> 1]
=============================================================================
