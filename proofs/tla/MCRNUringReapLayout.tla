-------------------- MODULE MCRNUringReapLayout --------------------
(***************************************************************************)
(* The small instance of RNUringReapLayout: a completion ring of four,    *)
(* three provided buffers and a hold ring of three, a budget of three     *)
(* entries per call, six receives and two sends.                          *)
(* MCRNUringReapLayout.cfg (one datagram per receive, a bank of two) and  *)
(* MCRNUringReapLayoutCoalesced.cfg (two per receive, a bank of three)    *)
(* are the passing controls; each MCRNUringReapLayoutBroken*.cfg must     *)
(* violate the property it names (README-RNUringReapLayout.md).           *)
(***************************************************************************)
EXTENDS RNUringReapLayout

MCSpec == Spec

=============================================================================
