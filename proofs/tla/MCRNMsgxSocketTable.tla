------------------------- MODULE MCRNMsgxSocketTable -------------------------
(***************************************************************************)
(* The checked instance of RNMsgxSocketTable: four addresses whose index   *)
(* homes collide and wrap (6, 7, 7, 0 in eight entries), and whose         *)
(* cooldown entries collide pairwise (two entries).                         *)
(***************************************************************************)
EXTENDS RNMsgxSocketTable

MCHome(a) == CASE a = 1 -> 6 [] a = 2 -> 7 [] a = 3 -> 7 [] OTHER -> 0

MCCoolOf(a) == (a - 1) % 2

\* the liveness configuration: no refused marks (RNMsgxSocketTable RefusedMarks)
MCNoRefusedMarks == FALSE

=============================================================================
