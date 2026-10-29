---------------------------- MODULE MCRNLazyPool ----------------------------
(***************************************************************************)
(* The small instance of RNLazyPool: three entries, three owners, any      *)
(* number of resets.  MCRNLazyPool.cfg is the committed run with both      *)
(* rules on; MCRNLazyPoolBrokenNoGuard.cfg and                             *)
(* MCRNLazyPoolBrokenKeepFresh.cfg are the reversed witnesses.             *)
(***************************************************************************)
EXTENDS RNLazyPool
=============================================================================
