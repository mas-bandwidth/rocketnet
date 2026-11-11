------------------- MODULE MCRNCookieReplyLimits -------------------
(***************************************************************************)
(* The small instance of RNCookieReplyLimits: three sources, max_clients  *)
(* 2, a bitmap of 4 bits in two words of 2, hash values 0 .. 7 before the *)
(* mask (every key, so every collision pattern), a work budget of 1.      *)
(* MCRNCookieReplyLimits.cfg is the passing control; each                 *)
(* MCRNCookieReplyLimitsBroken*.cfg must violate the invariant it names.  *)
(***************************************************************************)
EXTENDS RNCookieReplyLimits

MCSpec == Spec

=============================================================================
