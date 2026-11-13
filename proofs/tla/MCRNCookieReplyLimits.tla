------------------- MODULE MCRNCookieReplyLimits -------------------
(***************************************************************************)
(* The small instance of RNCookieReplyLimits: three sources, two requests *)
(* processed per update, a factor of 2 (16 in the code), so a bitmap of 4 *)
(* bits in two words of 2, hash values 0 .. 7 before the mask (every key, *)
(* so every collision pattern), a work budget of 1.                       *)
(* MCRNCookieReplyLimits.cfg is the passing control; each                 *)
(* MCRNCookieReplyLimitsBroken*.cfg must violate the invariant it names.  *)
(***************************************************************************)
EXTENDS RNCookieReplyLimits

MCSpec == Spec

\* MCRNCookieReplyLimitsUnwritable.cfg: source 3's address bytes cannot be
\* written, so it has no index (Unwritable)
MCUnwritable == {3}
\* MCRNCookieReplyLimitsUnlimited.cfg: no bitmap (Limited)
MCUnlimited == FALSE

=============================================================================
