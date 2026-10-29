------------------------- MODULE MCRNIndexRingArray -------------------------
(***************************************************************************)
(* The small instance of RNIndexRingArray: three slots in a ring of four  *)
(* entries, tickets enough to fill, wrap and compact it several times.    *)
(* MCRNIndexRingArray.cfg is the committed run;                            *)
(* MCRNIndexRingArrayBrokenCompactToZero.cfg is the reversed witness, the *)
(* committed file with CompactToZero TRUE.  Tickets are bounded, so a run *)
(* ends in terminal states once they are spent.                            *)
(***************************************************************************)
EXTENDS RNIndexRingArray
=============================================================================
