------------------------ MODULE MCRNClientReceiveQuit ------------------------
(***************************************************************************)
(* The instances of RNClientReceiveQuit.  The model is finite as written: *)
(* one thread, one stop, one destroy.  MCRNClientReceiveQuit.cfg (linux), *)
(* MCRNClientReceiveQuitMac.cfg and MCRNClientReceiveQuitWindows.cfg are  *)
(* the passing controls with the wait's timeout fair;                     *)
(* MCRNClientReceiveQuitBrokenTimeoutNeverFires.cfg lets the timeout      *)
(* never fire and must fail DestroyCompletes.                             *)
(***************************************************************************)
EXTENDS RNClientReceiveQuit
=============================================================================
