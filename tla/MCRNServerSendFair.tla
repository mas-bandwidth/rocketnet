------------------------ MODULE MCRNServerSendFair ------------------------
(***************************************************************************)
(* Small instances of RNServerSendFair. MCRNServerSendFair.cfg: three      *)
(* connections, one frame per update, in-place starts on, every rule on;   *)
(* MCRNServerSendFairWalk.cfg: five connections, two frames, no in-place   *)
(* starts, the ceil(N / K) bound. MCRNServerSendFairBrokenFromZero.cfg and *)
(* MCRNServerSendFairBrokenInPlace.cfg are the reversed witnesses, each a  *)
(* committed file with one rule flipped.                                   *)
(***************************************************************************)
EXTENDS RNServerSendFair
=============================================================================
