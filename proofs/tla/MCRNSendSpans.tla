---------------------------- MODULE MCRNSendSpans ----------------------------
(***************************************************************************)
(* The small instance of RNSendSpans: SR = 96 (six 16-byte units),        *)
(* message lengths 1, 32, 48 and 96 bytes (1 so that rounding has          *)
(* something to do, 96 the maximal message, the whole ring, as SR = B at   *)
(* the defaults), and four send descriptors, so four spans can be live    *)
(* and freed in any order.                                                 *)
(* MCRNSendSpans.cfg is the committed run with every rule on;             *)
(* MCRNSendSpansRefuse.cfg and MCRNSendSpansAbort.cfg are the controls of  *)
(* the refusal and the abort.  Each MCRNSendSpansBroken*.cfg (CASES.tsv    *)
(* group rnsendspans) flips one rule's constant and must violate the       *)
(* property it names.                                                      *)
(***************************************************************************)
EXTENDS RNSendSpans
=============================================================================
