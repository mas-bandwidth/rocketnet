------------------------- MODULE MCRNFilterEpochs -------------------------
(***************************************************************************)
(* The small instance of RNFilterEpochs: N = 4 stands for 255, a table of  *)
(* three, nine installs (two wraps). MCRNFilterEpochs.cfg is the rule.     *)
(* The reversed witnesses override one rule switch each:                   *)
(* MCRNFilterEpochsBrokenAcceptZero.cfg sets AcceptZero <- AcceptZeroBroken *)
(* (epoch 0 falls back to the unkeyed constant) and must fail              *)
(* NeverKeyedAtZero; MCRNFilterEpochsBrokenWrapToZero.cfg sets             *)
(* WrapTo <- WrapToZero (the generation after N is 0) and must fail        *)
(* WrapSkipsZero.                                                          *)
(***************************************************************************)
EXTENDS RNFilterEpochs

AcceptZeroBroken == TRUE
WrapToZero       == 0
=============================================================================
