--------------------------- MODULE MCRNGsoRecvFill ---------------------------
(***************************************************************************)
(* The small instance of RNGsoRecvFill: a buffer of four slots, room made  *)
(* for two segments a message, two messages a call, five messages in all   *)
(* and a socket of three.                                                  *)
(*   MCRNGsoRecvFill.cfg: runs of one or two segments, the code's rule;    *)
(*     NoOverflow and DropOnlyPastSeg hold.                                *)
(*   MCRNGsoRecvFillLocalRun.cfg: runs of up to three segments (a local    *)
(*     sender's run past Seg); DropOnlyPastSeg holds.                      *)
(*   MCRNGsoRecvFillReachLocalRunDrop.cfg: the same instance must fail     *)
(*     NoOverflow: the drop the code counts is reachable.                  *)
(*   MCRNGsoRecvFillBrokenFullBatch.cfg: a whole batch while one slot is   *)
(*     free; must fail NoOverflow.                                         *)
(*   MCRNGsoRecvFillBrokenOneFits.cfg: a whole batch once one message      *)
(*     fits, runs of up to three; must fail DropOnlyPastSeg.               *)
(***************************************************************************)
EXTENDS RNGsoRecvFill
=============================================================================
