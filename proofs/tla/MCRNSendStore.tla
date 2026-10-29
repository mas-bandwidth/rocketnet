--------------------------- MODULE MCRNSendStore ---------------------------
(***************************************************************************)
(* The small instance of RNSendStore: four chunks, three senders, blocks   *)
(* of up to three chunks.  MCRNSendStore.cfg is the committed run, with    *)
(* both rules on.  README-RNSendStore.md gives the two reversed witnesses  *)
(* as one-line edits to the cfg, and the traces TLC printed.               *)
(***************************************************************************)
EXTENDS RNSendStore
=============================================================================
