------------------------- MODULE MCRNMsgxSocketTable -------------------------
(***************************************************************************)
(* The checked instance of RNMsgxSocketTable: four addresses whose index   *)
(* homes collide and wrap (6, 7, 7, 0 in eight entries: HomeAt = {14, 23,  *)
(* 31, 32}, a * 8 + home), and whose cooldown entries collide pairwise     *)
(* (0, 1, 0, 1 in two entries: CoolAt = {2, 5, 6, 9}).                      *)
(***************************************************************************)
EXTENDS RNMsgxSocketTable

\* the liveness configuration: no refused marks (RNMsgxSocketTable RefusedMarks)
MCNoRefusedMarks == FALSE

\* the broken configuration MCRNMsgxSocketTableBrokenStaleReport.cfg: the
\* close loop leaves a dead report in the slot it frees (RNMsgxSocketTable
\* ClearReportAtFree)
MCKeepStaleReport == FALSE

\* the layout configuration: every registration succeeds (RNMsgxSocketTable
\* RegisterMayFail); the refused registration doubles its marks past the
\* 110 s budget, and the other passing configurations check it
MCRegisterAlways == FALSE

=============================================================================
