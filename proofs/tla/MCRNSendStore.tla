--------------------------- MODULE MCRNSendStore ---------------------------
(***************************************************************************)
(* The small instance of RNSendStore: four chunks, three senders.          *)
(* MCRNSendStore.cfg is the passing control, the guard on;                 *)
(* MCRNSendStoreBrokenNoGuard.cfg turns the double-free guard off and must *)
(* violate NoLeak (README-RNSendStore.md).                                 *)
(***************************************************************************)
EXTENDS RNSendStore
=============================================================================
