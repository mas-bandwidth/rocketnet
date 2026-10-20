------------------------- MODULE RNXdpReceiveRing -------------------------
(***************************************************************************)
(* rocketnet: the AF_XDP receive ring and the ownership of its frames.     *)
(*                                                                         *)
(* The XDP worker (rocketnet_server_socket_xdp_thread_function,            *)
(* sdk/rocketnet_server_socket_linux.cpp) peeks RX descriptors with        *)
(* xsk_ring_cons__peek, takes them while the staging buffer has room       *)
(* (rocketnet_xdp_receive_take, sdk/rocketnet_pending_frames.h), cancels   *)
(* the ones it did not take with xsk_ring_cons__cancel, releases the ones  *)
(* it took with xsk_ring_cons__release, and returns their frames to the    *)
(* fill ring through its pending return (rocketnet_pending_frames_t, an    *)
(* all-or-zero flush).  The staging buffer holds a copy of each packet's   *)
(* bytes, never a frame, and the application drains it under the receive  *)
(* mutex, which the worker holds for the whole pass.                       *)
(*                                                                         *)
(* The ring is the kernel's view: ring is the sequence of descriptors      *)
(* from the consumer index to the producer index.  ahead is the user's     *)
(* cached consumer minus the consumer: the descriptors peeked and neither  *)
(* released nor cancelled.  libxdp's peek starts at the cached consumer    *)
(* and advances it; cancel moves it back; release advances the consumer.   *)
(* The release hands the kernel the first n descriptors of the ring,       *)
(* whichever frames they hold, so a release while ahead > 0 at the start   *)
(* of a pass gives back descriptors that were never read.                  *)
(*                                                                         *)
(* Left out: the pending return's capacity (the peek is bounded by it and  *)
(* it is ROCKETNET_XDP_RECV_QUEUE_SIZE, more than the frames, so it never  *)
(* binds), the order the kernel takes fill frames in, packet contents and  *)
(* the frame codec's drops (a dropped frame is still taken and returned).  *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS
    Frames,       \* the UMEM receive frames
    RingSize,     \* the RX ring's entries
    PeekMax,      \* the most descriptors one peek asks for
    StageCap      \* the staging buffer's packets (ROCKETNET_XDP_RECV_QUEUE_SIZE)

ASSUME /\ Frames # {}
       /\ RingSize \in Nat \ {0}
       /\ PeekMax \in Nat \ {0}
       /\ StageCap \in Nat \ {0}

(***************************************************************************)
(* Rule switch, overridden by the reversed witness.                        *)
(*   CancelUnconsumed: the pass cancels the peeked descriptors it did not  *)
(*     take before the release (the design: TRUE).                         *)
(***************************************************************************)
CancelUnconsumed == TRUE

VARIABLES
    fill,      \* the frames in the fill ring (kernel, free)
    ring,      \* the RX ring's descriptors, consumer to producer (kernel view)
    ahead,     \* cached consumer - consumer (user)
    phase,     \* "idle" or "taking" (the worker holds the receive mutex)
    start,     \* the ring offset the current peek began at
    peeked,    \* the descriptors the current peek returned
    taken,     \* the descriptors the current pass took
    pending,   \* the frames the worker holds: taken, awaiting the fill ring
    staged,    \* the packets in the staging buffer (copies, not frames)
    took       \* ghost: the current pass has taken a descriptor

vars == <<fill, ring, ahead, phase, start, peeked, taken, pending, staged, took>>

Range(s) == {s[i] : i \in DOMAIN s}

Min(a, b) == IF a <= b THEN a ELSE b

TypeOK ==
    /\ fill \subseteq Frames
    /\ ring \in Seq(Frames) /\ Len(ring) <= RingSize
    /\ ahead \in 0..RingSize
    /\ phase \in {"idle", "taking"}
    /\ start \in 0..RingSize
    /\ peeked \in 0..PeekMax
    /\ taken \in 0..PeekMax
    /\ pending \subseteq Frames
    /\ staged \in 0..StageCap
    /\ took \in BOOLEAN

Init ==
    /\ fill = Frames
    /\ ring = <<>>
    /\ ahead = 0
    /\ phase = "idle"
    /\ start = 0
    /\ peeked = 0
    /\ taken = 0
    /\ pending = {}
    /\ staged = 0
    /\ took = FALSE

(* KernelReceive(f): a packet lands in fill frame f, posted to the RX ring *)
(* while the ring has a free entry as the kernel sees it.                  *)
KernelReceive(f) ==
    /\ f \in fill
    /\ Len(ring) < RingSize
    /\ fill' = fill \ {f}
    /\ ring' = Append(ring, f)
    /\ UNCHANGED <<ahead, phase, start, peeked, taken, pending, staged, took>>

(* Peek: xsk_ring_cons__peek from the cached consumer, at most PeekMax;    *)
(* the worker takes the receive mutex.  A peek of zero is no step.         *)
Peek ==
    LET n == Min(Len(ring) - ahead, PeekMax) IN
    /\ phase = "idle"
    /\ n > 0
    /\ start' = ahead
    /\ ahead' = ahead + n
    /\ peeked' = n
    /\ taken' = 0
    /\ phase' = "taking"
    /\ UNCHANGED <<fill, ring, pending, staged, took>>

(* Take: the staging buffer has room; read descriptor start + taken + 1    *)
(* (rx_desc at the peek's index + i) and hold its frame.                   *)
Take ==
    /\ phase = "taking"
    /\ taken < peeked
    /\ staged < StageCap
    /\ pending' = pending \cup {ring[start + taken + 1]}
    /\ taken' = taken + 1
    /\ staged' = staged + 1
    /\ took' = TRUE
    /\ UNCHANGED <<fill, ring, ahead, phase, start, peeked>>

(* Finish: every peeked descriptor taken, or the staging buffer full.      *)
(* Cancel the rest (CancelUnconsumed), release the mutex, release the      *)
(* taken count: the consumer advances over the first taken descriptors.    *)
Finish ==
    LET back == IF CancelUnconsumed THEN peeked - taken ELSE 0 IN
    /\ phase = "taking"
    /\ taken = peeked \/ staged = StageCap
    /\ ring' = SubSeq(ring, taken + 1, Len(ring))
    /\ ahead' = ahead - back - taken
    /\ phase' = "idle"
    /\ start' = 0
    /\ peeked' = 0
    /\ taken' = 0
    /\ took' = FALSE
    /\ UNCHANGED <<fill, pending, staged>>

(* Refill: the pending return's all-or-zero flush into the fill ring; a    *)
(* reservation of zero is this step not yet taken.                         *)
Refill ==
    /\ phase = "idle"
    /\ pending # {}
    /\ fill' = fill \cup pending
    /\ pending' = {}
    /\ UNCHANGED <<ring, ahead, phase, start, peeked, taken, staged, took>>

(* Drain: the application takes the staging buffer, under the mutex.       *)
Drain ==
    /\ phase = "idle"
    /\ staged > 0
    /\ staged' = 0
    /\ UNCHANGED <<fill, ring, ahead, phase, start, peeked, taken, pending, took>>

Next ==
    \/ \E f \in Frames : KernelReceive(f)
    \/ Peek
    \/ Take
    \/ Finish
    \/ Refill
    \/ Drain

(* The application drains whenever the worker is between passes: strong    *)
(* fairness, because the worker's passes disable it while they run.        *)
Spec == Init /\ [][Next]_vars /\ WF_vars(Next) /\ SF_vars(Drain)

(***************************************************************************)
(* Invariants.                                                             *)
(***************************************************************************)

(* The RX ring's descriptors the current pass has not taken: a taken      *)
(* descriptor stays in the kernel's ring until the release, and its frame  *)
(* is already the worker's.                                                *)
Untaken == {i \in 1..Len(ring) : i \notin start + 1..start + taken}
InRing == {ring[i] : i \in Untaken}

(* Every frame is in exactly one of the fill ring, the RX ring (untaken)   *)
(* or the worker's hands (the staging buffer holds copies), once each.     *)
FrameConservation ==
    /\ Cardinality(Untaken) = Cardinality(InRing)
    /\ fill \cap InRing = {}
    /\ fill \cap pending = {}
    /\ InRing \cap pending = {}
    /\ fill \cup InRing \cup pending = Frames

(* Between passes the cached consumer is the consumer: every peeked        *)
(* descriptor was released or cancelled.                                   *)
ConsumerNotBehind == phase = "idle" => ahead = 0

(***************************************************************************)
(* Liveness: receive never stalls while frames are free.  Whenever a frame *)
(* is in the fill ring or the RX ring, a pass eventually takes one.        *)
(***************************************************************************)
FramesFree == fill # {} \/ ring # <<>>

ReceiveNeverStalls == FramesFree ~> took
=============================================================================
