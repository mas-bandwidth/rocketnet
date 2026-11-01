--------------------------- MODULE MCRNGsoHandoff ---------------------------
(***************************************************************************)
(* The small instance of RNGsoHandoff: three receive buffers and three    *)
(* transmit banks of two packets, four datagrams and four staged packets. *)
(* MCRNGsoHandoff.cfg runs with every rule on;                            *)
(* MCRNGsoHandoffBrokenPublishWhileHeld.cfg restores the publish-while-   *)
(* held defect and must fail NoPublishWhileHeld;                          *)
(* MCRNGsoHandoffBrokenFullDrops.cfg drops a full socket's rest and must  *)
(* fail SentOrHeld; MCRNGsoHandoffBrokenCarryBehind.cfg queues the rest   *)
(* behind newer banks and must fail TxInOrder.                            *)
(***************************************************************************)
EXTENDS RNGsoHandoff
=============================================================================
