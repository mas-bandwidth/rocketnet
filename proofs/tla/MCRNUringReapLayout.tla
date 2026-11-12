-------------------- MODULE MCRNUringReapLayout --------------------
(***************************************************************************)
(* The small instance of RNUringReapLayout: a completion ring of four,    *)
(* two queues of two provided buffers each and a hold ring of two per     *)
(* queue, a budget of three entries per call, six receives and two sends. *)
(* MCRNUringReapLayout.cfg (one datagram per receive, a bank of two) and  *)
(* MCRNUringReapLayoutCoalesced.cfg (two per receive, a bank of three)    *)
(* are the passing controls; each MCRNUringReapLayoutBroken*.cfg must     *)
(* violate the property it names (README-RNUringReapLayout.md).           *)
(***************************************************************************)
EXTENDS RNUringReapLayout

MCSpec == Spec

=============================================================================
