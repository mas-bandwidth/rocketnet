---------------------------- MODULE RNXdpSendDrain ----------------------------
(***************************************************************************)
(* One XDP queue's TX side across the server stop                          *)
(* (sdk/rocketnet_server_socket_linux.cpp: the worker loop, send_idle and  *)
(* begin_shutdown; sdk/rocketnet_server_socket.h:                          *)
(* rocketnet_xdp_send_stall_pass and rocketnet_xdp_send_queue_released).   *)
(*                                                                         *)
(* The worker claims a staging buffer and copies its frames into UMEM     *)
(* frames one at a time (Load), into a batch it reserves on the TX ring    *)
(* all or none.  A claimed buffer may hold more frames than the batch      *)
(* takes, so it stays claimed (claimed) across a submit until its last     *)
(* frame is copied and it is recycled.  The ring's producer and consumer   *)
(* are indices taken modulo Modulus (the code's 32-bit indices) over       *)
(* RingSize entries.  The NIC takes one descriptor at a time (Consume); it *)
(* may stop taking them for good with no errno to say so (Die: carrier     *)
(* lost on a zero-copy queue, a copy-mode device queue that stays          *)
(* stopped), or with a hard TX wakeup errno (HardError: ENETDOWN, ENXIO,   *)
(* EBADF, rocketnet_xdp_send_wakeup_hard_error).  A hard errno is taken    *)
(* here as the code's comment states it, a ring that cannot drain: a hard  *)
(* errno that a later successful wakeup clears                             *)
(* (rocketnet_xdp_send_wakeup_succeeded) while the stop drain has already  *)
(* given the queue up is left out.                                         *)
(*                                                                         *)
(* Each worker pass (Pass) submits the batch when the ring has room, then  *)
(* runs the stall watch: pending is a claimed buffer, the batch, or        *)
(* descriptors past the consumer (the code's num_batch_packets > 0,        *)
(* send_worker_index >= 0 or on_ring != 0); during a stop (draining, set   *)
(* by BeginShutdown) the watch starts timing at the first pass that finds  *)
(* frames pending and the consumer where it was (still = 1: since is now,  *)
(* no time elapsed) and counts one tick a pass after that (still - 1       *)
(* ticks elapsed), up to Bound ticks, and Bound ticks elapsed sets         *)
(* stalled; the consumer moving or nothing pending stops the timing;       *)
(* outside a stop nothing is timed.  Time is counted in passes: a pass is  *)
(* one tick of the platform clock the worker reads (the code's bound is    *)
(* ROCKETNET_XDP_SEND_STALL_SECONDS of that clock, compared as             *)
(* now - since >= bound).  The main thread's stop drain (SendIdle)         *)
(* releases the queue when the worker reported a hard errno or a stall,   *)
(* or when nothing is pending and no buffer is claimed (handoff_idle:      *)
(* free_count is every buffer but the producer's, and send_pending is      *)
(* false).                                                                 *)
(*                                                                         *)
(* Reversed witnesses, one constant: Defect = "norelease" is the drain     *)
(* before this model, deaf to a stall, and a ring that never drains keeps  *)
(* the stop from finishing (StopFinishes).  "noreset" never restarts the   *)
(* count when the consumer moves, and a ring that drains slowly is given   *)
(* up on (NeverAbandonDraining).  "noring" is the stop drain that looked   *)
(* at the handoff but not at the TX ring, and cuts off what the ring still *)
(* holds (NeverAbandonDraining).  "noclaim" leaves a claimed buffer out of *)
(* both the worker's pending and the drain's idle test: the drain reads    *)
(* send_pending false from the last pass while the worker copies a buffer  *)
(* it has just claimed, and cuts it off (NeverAbandonDraining); the code   *)
(* counts it in both (send_worker_index >= 0, and free_count in            *)
(* handoff_idle).  Its failure comes from the drain's half: Pass never    *)
(* runs with a claim and nothing in the batch or on the ring (the guard    *)
(* below), so the worker's claim term cannot change pending here, and the  *)
(* code's send_worker_index >= 0 matters only in that guarded-out pass.    *)
(* A configuration witness: a Bound no longer than the                     *)
(* ticks a healthy NIC's longest pause lets elapse once the watch is       *)
(* timing (SlowGap - 1: the pass that starts the timing is the first of    *)
(* the SlowGap passes) gives up on a healthy ring (NeverAbandonDraining),  *)
(* which is why the bound must sit far above the longest pause a live NIC  *)
(* takes.                                                                  *)
(* The indices are free running (RingSize < Modulus), as libxdp's are, so  *)
(* the consumer cannot move a whole Modulus between two passes; the        *)
(* configurations load more frames than Modulus, so both indices wrap.     *)
(***************************************************************************)
EXTENDS Naturals

CONSTANTS
    RingSize,       \* TX ring entries (ROCKETNET_XDP_SEND_QUEUE_SIZE)
    Modulus,        \* the index range (2^32 in the code)
    BatchMax,       \* frames the worker holds before the ring takes them (ROCKETNET_XDP_SEND_BATCH_SIZE)
    MaxFrames,      \* exploration bound: frames loaded over the run
    Bound,          \* ROCKETNET_XDP_SEND_STALL_SECONDS, in ticks
    SlowGap,        \* the most passes a healthy NIC lets go by with descriptors on its ring before it takes one
    Defect

ASSUME Defect \in {"none", "norelease", "noreset", "noring", "noclaim"}
ASSUME RingSize \in Nat \ {0} /\ Modulus \in Nat \ {0} /\ RingSize < Modulus
ASSUME BatchMax \in 1..RingSize /\ MaxFrames \in Nat \ {0}
ASSUME Bound \in Nat \ {0} /\ SlowGap \in Nat \ {0}

Phases == {"RUNNING", "DRAINING", "RELEASED"}

VARIABLES
    phase,          \* RUNNING; DRAINING after begin_shutdown; RELEASED once send_idle gave the queue up or found it idle
    prod,           \* the TX ring's producer index, modulo Modulus
    cons,           \* the TX ring's consumer index, modulo Modulus
    batch,          \* frames in the worker's batch
    claimed,        \* the worker holds a claimed staging buffer with frames still to copy (send_worker_index >= 0)
    loaded,         \* ghost: frames loaded so far
    watchCons,      \* rocketnet_xdp_send_stall_t consumer
    still,          \* rocketnet_xdp_send_stall_t timing and since: 0 when not timing, else 1 + the ticks elapsed
    stalled,        \* send_stalled
    pendingFlag,    \* send_pending as the last pass (or the last recycle) published it
    hardError,      \* send_hard_error: the last TX wakeup failed with a hard errno
    dead,           \* environment: the NIC takes no more descriptors
    gap,            \* environment: passes since the NIC last took a descriptor that left descriptors on its ring
    cutLive         \* ghost: the queue was released while a live NIC still had frames to take

vars == <<phase, prod, cons, batch, claimed, loaded, watchCons, still, stalled, pendingFlag,
          hardError, dead, gap, cutLive>>

OnRing == (prod + Modulus - cons) % Modulus

Pending == claimed \/ batch > 0 \/ OnRing > 0

Init ==
    /\ phase = "RUNNING"
    /\ prod = 0 /\ cons = 0
    /\ batch = 0 /\ claimed = FALSE /\ loaded = 0
    /\ watchCons = 0 /\ still = 0
    /\ stalled = FALSE /\ pendingFlag = FALSE
    /\ hardError = FALSE /\ dead = FALSE /\ gap = 0
    /\ cutLive = FALSE

\* The worker copies one frame of the buffer it holds (claiming a buffer the
\* main thread handed it first, when it holds none) into its batch; before
\* RELEASED the main thread may still be handing it work. The buffer stays
\* claimed while frames remain to copy; with its last frame it is recycled,
\* which publishes send_pending true.
Load ==
    /\ phase # "RELEASED"
    /\ batch < BatchMax
    /\ loaded < MaxFrames
    /\ batch' = batch + 1
    /\ loaded' = loaded + 1
    /\ \E more \in BOOLEAN :
          /\ (more => loaded + 1 < MaxFrames)
          /\ claimed' = more
          /\ pendingFlag' = IF more THEN pendingFlag ELSE TRUE
    /\ UNCHANGED <<phase, prod, cons, watchCons, still, stalled, hardError, dead, gap, cutLive>>

\* Environment: the NIC takes the descriptor at the consumer.
Consume ==
    /\ ~dead
    /\ OnRing > 0
    /\ cons' = (cons + 1) % Modulus
    /\ gap' = 0
    /\ UNCHANGED <<phase, prod, batch, claimed, loaded, watchCons, still, stalled, pendingFlag,
                   hardError, dead, cutLive>>

\* Environment: the NIC stops taking descriptors, with no errno to say so.
Die ==
    /\ ~dead
    /\ dead' = TRUE
    /\ UNCHANGED <<phase, prod, cons, batch, claimed, loaded, watchCons, still, stalled, pendingFlag,
                   hardError, gap, cutLive>>

\* Environment: the NIC stops taking descriptors and the worker's TX wakeup
\* fails with a hard errno (rocketnet_xdp_send_wakeup_failed stores it in
\* send_hard_error).
HardError ==
    /\ ~hardError
    /\ hardError' = TRUE
    /\ dead' = TRUE
    /\ UNCHANGED <<phase, prod, cons, batch, claimed, loaded, watchCons, still, stalled, pendingFlag,
                   gap, cutLive>>

\* rocketnet_server_socket_begin_shutdown: the transport's stop began.
BeginShutdown ==
    /\ phase = "RUNNING"
    /\ phase' = "DRAINING"
    /\ UNCHANGED <<prod, cons, batch, claimed, loaded, watchCons, still, stalled, pendingFlag,
                   hardError, dead, gap, cutLive>>

\* One worker pass: the batch goes on the ring when it all fits, then the
\* stall watch (rocketnet_xdp_send_stall_pass) runs on the consumer it reads
\* and the pending it computes, and send_pending is published. A pass may
\* find a buffer claimed and nothing copied while the NIC holds descriptors
\* (the UMEM frame pool empty, INVALID_FRAME, until the completion ring
\* hands frames back); with the TX ring empty the pass copies first, as the
\* code's pass copies when its batch is empty (the frames then sit in a
\* completion ring the kernel is still filling, within the NIC's pause:
\* left out). A healthy NIC takes a descriptor before SlowGap passes go by
\* with descriptors on its ring.
Pass ==
    /\ ~(claimed /\ batch = 0 /\ OnRing = 0)
    /\ dead \/ OnRing = 0 \/ gap < SlowGap
    /\ LET fits == batch > 0 /\ OnRing + batch <= RingSize
           p == IF fits THEN (prod + batch) % Modulus ELSE prod
           b == IF fits THEN 0 ELSE batch
           pending == (claimed /\ Defect # "noclaim") \/ b > 0 \/ (p + Modulus - cons) % Modulus > 0
           moved == cons # watchCons /\ Defect # "noreset"
           s == IF ~pending \/ moved \/ phase # "DRAINING" THEN 0
                ELSE IF still <= Bound THEN still + 1 ELSE still
       IN /\ prod' = p
          /\ batch' = b
          /\ pendingFlag' = pending
          /\ watchCons' = IF ~pending \/ moved THEN cons ELSE watchCons
          /\ still' = s
          /\ stalled' = IF ~pending \/ moved THEN FALSE
                        ELSE stalled \/ (s = Bound + 1 /\ phase = "DRAINING")
          /\ gap' = IF (p + Modulus - cons) % Modulus > 0 /\ ~dead THEN gap + 1 ELSE 0
    /\ UNCHANGED <<phase, cons, claimed, loaded, hardError, dead, cutLive>>

\* The main thread's stop drain (rocketnet_server_socket_send_idle through
\* rocketnet_xdp_send_queue_released): the queue is given up when the worker
\* reported a hard errno or found it stalled, or is idle when nothing is
\* pending and no buffer is claimed.
Released ==
    CASE Defect = "norelease" -> ~pendingFlag /\ ~claimed
      [] Defect = "noring" -> hardError \/ stalled \/ (batch = 0 /\ ~claimed)
      [] Defect = "noclaim" -> hardError \/ stalled \/ ~pendingFlag
      [] OTHER -> hardError \/ stalled \/ (~pendingFlag /\ ~claimed)
SendIdle ==
    /\ phase = "DRAINING"
    /\ Released
    /\ phase' = "RELEASED"
    /\ cutLive' = (cutLive \/ (~dead /\ Pending))
    /\ UNCHANGED <<prod, cons, batch, claimed, loaded, watchCons, still, stalled, pendingFlag,
                   hardError, dead, gap>>

Next ==
    \/ Load
    \/ Consume
    \/ Die
    \/ HardError
    \/ BeginShutdown
    \/ Pass
    \/ SendIdle
    \/ phase = "RELEASED" /\ UNCHANGED vars

Spec == Init /\ [][Next]_vars

\* The worker keeps passing and copying the buffer it holds, the main thread
\* keeps polling the stop, and a live NIC keeps taking descriptors; a dead
\* one takes none, and nothing forces Die or HardError.
FairSpec == Spec /\ WF_vars(Pass) /\ WF_vars(claimed /\ Load) /\ WF_vars(SendIdle) /\ WF_vars(Consume)

TypeOK ==
    /\ phase \in Phases
    /\ prod \in 0..Modulus - 1 /\ cons \in 0..Modulus - 1
    /\ OnRing <= RingSize
    /\ batch \in 0..BatchMax /\ claimed \in BOOLEAN /\ loaded \in 0..MaxFrames
    /\ watchCons \in 0..Modulus - 1 /\ still \in 0..Bound + 1
    /\ stalled \in BOOLEAN /\ pendingFlag \in BOOLEAN
    /\ hardError \in BOOLEAN /\ dead \in BOOLEAN /\ gap \in 0..SlowGap
    /\ cutLive \in BOOLEAN

\* A queue whose NIC still takes descriptors is never given up on while it
\* holds frames (a claimed buffer, a batch or descriptors on the ring): the
\* stop waits for a ring that drains, however slowly within SlowGap.
NeverAbandonDraining == ~cutLive

\* Once the stop has begun the queue is released, even when the NIC never
\* takes another descriptor.
StopFinishes == phase = "DRAINING" ~> phase = "RELEASED"
=============================================================================
