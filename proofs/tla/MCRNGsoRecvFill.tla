--------------------------- MODULE MCRNGsoRecvFill ---------------------------
(***************************************************************************)
(* The small instance of RNGsoRecvFill: a buffer of four slots, GRO runs   *)
(* of one or two segments, two messages a call, five messages in all and   *)
(* a socket of three. MCRNGsoRecvFill.cfg runs the code's rule;            *)
(* MCRNGsoRecvFillBrokenFullBatch.cfg asks for a whole batch while one     *)
(* slot is free and must fail NoOverflow.                                  *)
(***************************************************************************)
EXTENDS RNGsoRecvFill
=============================================================================
