--------------------------- MODULE MCRNGsoRecvWalk ---------------------------
(***************************************************************************)
(* The small instance of RNGsoRecvWalk: four queues of two-packet buffers *)
(* and a budget of three, so two full queues are past the budget and one  *)
(* is not, as in the code (two full ROCKETNET_GSO_RECV_QUEUE_SIZE buffers  *)
(* exhaust the per-update budget).  MCRNGsoRecvWalk.cfg runs the rotating *)
(* walk and must pass; MCRNGsoRecvWalkBrokenStartAtZero.cfg starts every  *)
(* call at queue 0 and must fail NoQueueStarves.                          *)
(* The XDP walk at its small sizes: MCRNGsoRecvWalkXdp.cfg, three queues  *)
(* of two-packet buffers and a budget of two, and MCRNGsoRecvWalkXdpTwo.  *)
(* cfg, two such queues, must pass;                                       *)
(* MCRNGsoRecvWalkBrokenFillAtNextQueue.cfg, two such queues under the    *)
(* old XDP rule, must fail NoQueueStarves.                                *)
(***************************************************************************)
EXTENDS RNGsoRecvWalk

StartAtZeroBroken == TRUE

FillAtNextQueueBroken == TRUE
=============================================================================
