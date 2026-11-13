------------------------- MODULE MCRNReceiveHandoff -------------------------
(***************************************************************************)
(* The small instance of RNReceiveHandoff: two-datagram buffers, five     *)
(* datagrams and budgets of one and six, so the worker fills a buffer,    *)
(* stalls and moves, and main's calls meet every role change, a busy      *)
(* mutex and a budget cut that leaves a tail unread.                      *)
(* MCRNReceiveHandoff.cfg must pass; MCRNReceiveHandoffBrokenReadOnlyLeft  *)
(* .cfg sets ReadOnlyLeft (the old ready-slot rule) and must fail          *)
(* DeliveredByNextCall.                                                    *)
(***************************************************************************)
EXTENDS RNReceiveHandoff
=============================================================================
