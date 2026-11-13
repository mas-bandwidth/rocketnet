--------------------------- MODULE MCRNSendStore ---------------------------
(***************************************************************************)
(* The small instance of RNSendStore: four chunks, three senders, a tag of *)
(* three values, so it wraps on a chunk's third take.  MCRNSendStore.cfg   *)
(* is the passing control, both rules on; MCRNSendStoreBrokenNoGuard.cfg   *)
(* turns the double-free guard off and must violate NoLeak;                *)
(* MCRNSendStoreBrokenGenZero.cfg lets the tag wrap to 0 and must violate  *)
(* HeldGenerationPositive (README-RNSendStore.md).                         *)
(***************************************************************************)
EXTENDS RNSendStore
=============================================================================
