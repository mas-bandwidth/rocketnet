--------------------------- MODULE MCRNDestroyWipe ---------------------------
(***************************************************************************)
(* The small instance of RNDestroyWipe: two mappings over six pages, the   *)
(* server keys on page 1, the ring's key array on page 2, the mappings'    *)
(* entries on pages 3 and 4 and their clients' slots on pages 5 and 6.     *)
(* MCRNDestroyWipe.cfg is the committed run; the Broken configurations are *)
(* the reversed witnesses.                                                 *)
(***************************************************************************)
EXTENDS RNDestroyWipe

MCEntryPage == [ m \in {1, 2} |-> m + 2 ]
MCSlotPage == [ m \in {1, 2} |-> m + 4 ]
=============================================================================
