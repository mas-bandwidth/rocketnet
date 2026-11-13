---------------------------- MODULE RNXdpSendDrain ----------------------------
(***************************************************************************)
(* One XDP queue's TX side across the server stop                          *)
(* (sdk/rocketnet_server_socket_linux.cpp: the worker loop, send_idle and  *)
(* begin_shutdown; sdk/rocketnet_server_socket.h:                          *)
(* rocketnet_xdp_send_stall_pass and rocketnet_xdp_send_queue_released).   *)
(*                                                                         *)
(* The worker holds a batch of frames (Load: a claimed staging buffer      *)
(* copied into UMEM frames) and reserves them on the TX ring all or none.  *)
(* The ring's producer and consumer are indices taken modulo Modulus (the  *)
(* code's 32-bit indices) over RingSize entries.  The NIC takes one        *)
(* descriptor at a time (Consume); it may stop taking them for good (Die:  *)
(* carrier lost on a zero-copy queue, a copy-mode device queue that stays  *)
(* stopped) with no errno to say so.  A healthy NIC pauses at most SlowGap *)
(* ticks of the worker's clock with frames waiting: at most SlowGap + 1    *)
(* passes in a row find frames pending and nothing taken, so two passes    *)
(* that see the consumer in the same place are at most SlowGap ticks       *)
(* apart.                                                                  *)
(*                                                                         *)
(* Each worker pass (Pass) submits the batch when the ring has room, then  *)
(* runs the stall watch as rocketnet_xdp_send_stall_pass does: pending is  *)
(* the batch or descriptors past the consumer; the consumer moving, or     *)
(* nothing pending, stops the timing and clears stalled.  During a stop    *)
(* (draining, from BeginShutdown on) the first pass that finds frames      *)
(* pending and the consumer where it was starts the timing (timing, still  *)
(* = 0: the code's since = now); each later such pass is one tick more,    *)
(* up to Bound, and a pass at still = Bound sets stalled (the code's       *)
(* now - since < bound_seconds failing).  Outside a stop nothing is timed. *)
(* Time is counted in passes: a pass is one tick of the platform clock the *)
(* worker reads (the code's bound is ROCKETNET_XDP_SEND_STALL_SECONDS of   *)
(* that clock).  The main thread's stop drain (SendIdle) releases the      *)
(* queue when it is stalled or nothing is pending.                         *)
(*                                                                         *)
(* Reversed witnesses, one constant: Defect = "norelease" is the drain     *)
(* before this model, deaf to a stall, and a ring that never drains keeps  *)
(* the stop from finishing (StopFinishes).  "noreset" never restarts the   *)
(* count when the consumer moves, and a ring that drains slowly is given   *)
(* up on (NeverAbandonDraining).  "noring" is the stop drain that looked   *)
(* at the handoff but not at the TX ring, and cuts off what the ring still *)
(* holds (NeverAbandonDraining).  A configuration witness: a Bound equal   *)
(* to SlowGap gives up on a healthy ring (NeverAbandonDraining), because a *)
(* stillness equal to the bound is a stall (the code compares with <),     *)
(* which is why the bound must sit far above the longest pause a live NIC  *)
(* takes.                                                                  *)
(* The indices are free running (RingSize < Modulus), as libxdp's are, so  *)
(* the consumer cannot move a whole Modulus between two passes.            *)
(***************************************************************************)
EXTENDS Naturals

CONSTANTS
    RingSize,       \* TX ring entries (ROCKETNET_XDP_SEND_QUEUE_SIZE)
    Modulus,        \* the index range (2^32 in the code)
    BatchMax,       \* frames the worker holds before the ring takes them (ROCKETNET_XDP_SEND_BATCH_SIZE)
    MaxFrames,      \* exploration bound: frames loaded over the run
    Bound,          \* ROCKETNET_XDP_SEND_STALL_SECONDS, in ticks
    SlowGap,        \* the most ticks of the worker's clock a healthy NIC lets pass with frames waiting
    Defect

ASSUME Defect \in {"none", "norelease", "noreset", "noring"}
ASSUME RingSize \in Nat \ {0} /\ Modulus \in Nat \ {0} /\ RingSize < Modulus
ASSUME BatchMax \in 1..RingSize /\ MaxFrames \in Nat \ {0}
ASSUME Bound \in Nat \ {0} /\ SlowGap \in Nat \ {0}

Phases == {"RUNNING", "DRAINING", "RELEASED"}

VARIABLES
    phase,          \* RUNNING; DRAINING after begin_shutdown; RELEASED once send_idle gave the queue up or found it idle
    prod,           \* the TX ring's producer index, modulo Modulus
    cons,           \* the TX ring's consumer index, modulo Modulus
    batch,          \* frames in the worker's batch
    loaded,         \* ghost: frames loaded so far
    watchCons,      \* rocketnet_xdp_send_stall_t consumer
    timing,         \* rocketnet_xdp_send_stall_t timing
    still,          \* now - rocketnet_xdp_send_stall_t since, in ticks, up to Bound; 0 when not timing
    stalled,        \* send_stalled
    pendingFlag,    \* send_pending as the last pass published it
    dead,           \* environment: the NIC takes no more descriptors
    gap,            \* environment: passes since the NIC last took a descriptor while frames waited
    cutLive         \* ghost: the queue was released while a live NIC still had frames to take

vars == <<phase, prod, cons, batch, loaded, watchCons, timing, still, stalled, pendingFlag, dead, gap, cutLive>>

OnRing == (prod + Modulus - cons) % Modulus

Pending == batch > 0 \/ OnRing > 0

Init ==
    /\ phase = "RUNNING"
    /\ prod = 0 /\ cons = 0
    /\ batch = 0 /\ loaded = 0
    /\ watchCons = 0 /\ timing = FALSE /\ still = 0
    /\ stalled = FALSE /\ pendingFlag = FALSE
    /\ dead = FALSE /\ gap = 0
    /\ cutLive = FALSE

\* The worker claims a staging buffer and copies a frame of it into its
\* batch; before RELEASED the main thread may still be handing it work.
Load ==
    /\ phase # "RELEASED"
    /\ batch < BatchMax
    /\ loaded < MaxFrames
    /\ batch' = batch + 1
    /\ loaded' = loaded + 1
    /\ pendingFlag' = TRUE
    /\ UNCHANGED <<phase, prod, cons, watchCons, timing, still, stalled, dead, gap, cutLive>>

\* Environment: the NIC takes the descriptor at the consumer.
Consume ==
    /\ ~dead
    /\ OnRing > 0
    /\ cons' = (cons + 1) % Modulus
    /\ gap' = 0
    /\ UNCHANGED <<phase, prod, batch, loaded, watchCons, timing, still, stalled, pendingFlag, dead, cutLive>>

\* Environment: the NIC stops taking descriptors, with no errno to say so.
Die ==
    /\ ~dead
    /\ dead' = TRUE
    /\ UNCHANGED <<phase, prod, cons, batch, loaded, watchCons, timing, still, stalled, pendingFlag, gap, cutLive>>

\* rocketnet_server_socket_begin_shutdown: the transport's stop began.
BeginShutdown ==
    /\ phase = "RUNNING"
    /\ phase' = "DRAINING"
    /\ UNCHANGED <<prod, cons, batch, loaded, watchCons, timing, still, stalled, pendingFlag, dead, gap, cutLive>>

\* One worker pass: the batch goes on the ring when it all fits, then the
\* stall watch (rocketnet_xdp_send_stall_pass) runs on the consumer it reads
\* and the pending it computes, and send_pending is published. A healthy NIC
\* takes a descriptor before SlowGap + 1 passes go by with frames waiting.
\* The watch, step for step (sdk/rocketnet_server_socket.h):
\* rocketnet_xdp_send_stall_pass resets on nothing pending or a moved
\* consumer (reset); outside a stop it does nothing (draining is
\* send_draining, set by begin_shutdown and never cleared); the first timed
\* pass sets since = now (timing, still = 0) and reports nothing; a later
\* pass is one tick more, and stalls once now - since < bound_seconds fails,
\* that is still = Bound.
Pass ==
    /\ dead \/ ~Pending \/ gap <= SlowGap
    /\ LET fits == batch > 0 /\ OnRing + batch <= RingSize
           p == IF fits THEN (prod + batch) % Modulus ELSE prod
           b == IF fits THEN 0 ELSE batch
           pending == b > 0 \/ (p + Modulus - cons) % Modulus > 0
           moved == cons # watchCons /\ Defect # "noreset"
           reset == ~pending \/ moved
           draining == phase # "RUNNING"
           s == IF reset \/ ~draining \/ ~timing THEN 0
                ELSE IF still < Bound THEN still + 1 ELSE still
       IN /\ prod' = p
          /\ batch' = b
          /\ pendingFlag' = pending
          /\ watchCons' = IF reset THEN cons ELSE watchCons
          /\ timing' = IF reset THEN FALSE ELSE timing \/ draining
          /\ still' = s
          /\ stalled' = IF reset THEN FALSE
                        ELSE stalled \/ s = Bound
          /\ gap' = IF pending /\ ~dead THEN gap + 1 ELSE 0
    /\ UNCHANGED <<phase, cons, loaded, dead, cutLive>>

\* The main thread's stop drain (rocketnet_server_socket_send_idle through
\* rocketnet_xdp_send_queue_released): the queue is given up when the worker
\* found it stalled, or is idle when nothing is pending.
Released ==
    CASE Defect = "norelease" -> ~pendingFlag
      [] Defect = "noring" -> stalled \/ batch = 0
      [] OTHER -> stalled \/ ~pendingFlag
SendIdle ==
    /\ phase = "DRAINING"
    /\ Released
    /\ phase' = "RELEASED"
    /\ cutLive' = (cutLive \/ (~dead /\ Pending))
    /\ UNCHANGED <<prod, cons, batch, loaded, watchCons, timing, still, stalled, pendingFlag, dead, gap>>

Next ==
    \/ Load
    \/ Consume
    \/ Die
    \/ BeginShutdown
    \/ Pass
    \/ SendIdle
    \/ phase = "RELEASED" /\ UNCHANGED vars

Spec == Init /\ [][Next]_vars

\* The worker keeps passing, the main thread keeps polling the stop, and a
\* live NIC keeps taking descriptors; a dead one takes none, and nothing
\* forces Die.
FairSpec == Spec /\ WF_vars(Pass) /\ WF_vars(SendIdle) /\ WF_vars(Consume)

TypeOK ==
    /\ phase \in Phases
    /\ prod \in 0..Modulus - 1 /\ cons \in 0..Modulus - 1
    /\ OnRing <= RingSize
    /\ batch \in 0..BatchMax /\ loaded \in 0..MaxFrames
    /\ watchCons \in 0..Modulus - 1
    /\ timing \in BOOLEAN /\ still \in 0..Bound /\ (~timing => still = 0)
    /\ stalled \in BOOLEAN /\ pendingFlag \in BOOLEAN
    /\ dead \in BOOLEAN /\ gap \in 0..SlowGap + 1
    /\ cutLive \in BOOLEAN

\* A queue whose NIC still takes descriptors is never given up on while it
\* holds frames: the stop waits for a ring that drains, however slowly
\* within SlowGap.
NeverAbandonDraining == ~cutLive

\* Once the stop has begun the queue is released, even when the NIC never
\* takes another descriptor.
StopFinishes == phase = "DRAINING" ~> phase = "RELEASED"
=============================================================================
