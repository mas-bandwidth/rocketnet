------------------------- MODULE RNXdpReceiveRing -------------------------
(***************************************************************************)
(* rocketnet: the AF_XDP receive ring and the ownership of its frames.     *)
(*                                                                         *)
(* The XDP worker (rocketnet_server_socket_xdp_thread_function,            *)
(* sdk/rocketnet_server_socket_linux.cpp) peeks RX descriptors with        *)
(* xsk_ring_cons__peek, takes them while the staging buffer has room,      *)
(* cancels the ones it did not take with xsk_ring_cons__cancel and         *)
(* releases the ones it took with xsk_ring_cons__release                   *)
(* (rocketnet_xdp_receive_ring_finish), and returns their frames to the    *)
(* fill ring through its pending return (rocketnet_pending_frames_t, an    *)
(* all-or-zero flush).  The staging buffer holds a copy of each packet's   *)
(* bytes, never a frame. This single-buffer frame-lifecycle abstraction   *)
(* serializes fill and drain; it does not model the exact bank handoff.    *)
(* In the SDK the mutex protects publication/claim/recycle, and the main  *)
(* thread drains its owned bank outside that mutex.                       *)
(*                                                                         *)
(* The fill ring is the kernel's too: fq is its entries from the consumer  *)
(* index the kernel has published to the producer index.  The kernel       *)
(* reads it through a cached batch (xskq_cons_peek_addr_unchecked,         *)
(* net/xdp/xsk_queue.h): kcache is the batch it peeked, kused the entries  *)
(* of the batch it has taken, and it publishes its consumer only when the  *)
(* batch is used up.  A taken entry keeps its slot until then, so the      *)
(* user's free slots are FillSize - Len(fq) (libxdp's                      *)
(* xsk_ring_prod__reserve), and the flush fits only in those.  The server  *)
(* sizes the ring at twice its receive frames (ROCKETNET_XDP_FILL_RING_SIZE*)
(* in rocketnet_constants.h); with exactly the frames, the worker cannot   *)
(* hand a frame back until the kernel ends its batch, and the kernel then  *)
(* finds the ring empty and drops the packet (hulk 2026-10-22, "rx fill    *)
(* ring empty" on the one queue all of one host's clients hash to).        *)
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
(* A driver or kernel defect can post a descriptor naming a frame outside  *)
(* the receive region (a send frame, SendFrame here): KernelMisdeliver,    *)
(* at most once per behaviour.  The worker reads it and refuses it         *)
(* (rocketnet_xdp_receive_descriptor), and hands a frame back to the fill  *)
(* ring only when rocketnet_xdp_fill_return (sdk/rocketnet_xdp_frame.h)    *)
(* places it in the receive region, so a send frame is never received      *)
(* into while it is also on the send free list.                            *)
(*                                                                         *)
(* Left out: the pending return's capacity (the peek is bounded by it and  *)
(* it is ROCKETNET_XDP_RECV_QUEUE_SIZE, no fewer than the frames, so it    *)
(* never binds), packet contents and the frame codec's drops (a dropped    *)
(* frame is still taken and returned).                                     *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS
    Frames,       \* the UMEM receive frames
    RingSize,     \* the RX ring's entries
    PeekMax,      \* the most descriptors one peek asks for
    StageCap,     \* the staging buffer's packets (ROCKETNET_XDP_RECV_QUEUE_SIZE)
    FillSize      \* the fill ring's entries (ROCKETNET_XDP_FILL_RING_SIZE)

ASSUME /\ Frames # {}
       /\ RingSize \in Nat \ {0}
       /\ PeekMax \in Nat \ {0}
       /\ StageCap \in Nat \ {0}
       /\ FillSize \in Nat /\ FillSize >= Cardinality(Frames)

(***************************************************************************)
(* Rule switch, overridden by the reversed witness.                        *)
(*   CancelUnconsumed: the pass cancels the peeked descriptors it did not  *)
(*     take before the release (the design: TRUE).                         *)
(***************************************************************************)
CancelUnconsumed == TRUE

(***************************************************************************)
(*   RefusedRefilled: a descriptor naming a frame outside the receive      *)
(*     region is handed back to the fill ring like any dropped frame (the  *)
(*     worker before rocketnet_xdp_fill_return: FALSE in the design).      *)
(***************************************************************************)
RefusedRefilled == FALSE

(* The frame a defective descriptor names: one the model does not own for  *)
(* receive (a send-region frame).  An untyped model value Frames holds is  *)
(* never equal to it.                                                      *)
SendFrame == "send frame"

AllFrames == Frames \cup {SendFrame}

VARIABLES
    fq,        \* the fill ring's entries, published consumer to producer
    kcache,    \* the entries at the front of fq the kernel's batch peeked
    kused,     \* the entries of that batch the kernel has taken
    ring,      \* the RX ring's descriptors, consumer to producer (kernel view)
    ahead,     \* cached consumer - consumer (user)
    phase,     \* "idle" or "taking" in the single-buffer abstraction
    start,     \* the ring offset the current peek began at
    peeked,    \* the descriptors the current peek returned
    taken,     \* the descriptors the current pass took
    pending,   \* the frames the worker holds: taken, awaiting the fill ring
    staged,    \* the packets in the staging buffer (copies, not frames)
    took,      \* ghost: the current pass has taken a descriptor
    defect     \* the kernel has posted a descriptor naming SendFrame

vars == <<fq, kcache, kused, ring, ahead, phase, start, peeked, taken, pending, staged, took, defect>>

Range(s) == {s[i] : i \in DOMAIN s}

Min(a, b) == IF a <= b THEN a ELSE b

(* The frames of a set in some order: the order the flush writes them.     *)
SetSeq(S) == CHOOSE s \in [1..Cardinality(S) -> S] : Range(s) = S

(* The fill ring's frames the kernel has not taken.                        *)
Fill == {fq[i] : i \in kused + 1..Len(fq)}

(* The flush fits: libxdp's reservation of every pending frame or none.    *)
FlushFits == Len(fq) + Cardinality(pending) <= FillSize

TypeOK ==
    /\ fq \in Seq(AllFrames) /\ Len(fq) <= FillSize
    /\ kcache \in 0..Len(fq)
    /\ kused \in 0..kcache
    /\ ring \in Seq(AllFrames) /\ Len(ring) <= RingSize
    /\ ahead \in 0..RingSize
    /\ phase \in {"idle", "taking"}
    /\ start \in 0..RingSize
    /\ peeked \in 0..PeekMax
    /\ taken \in 0..PeekMax
    /\ pending \subseteq AllFrames
    /\ staged \in 0..StageCap
    /\ took \in BOOLEAN
    /\ defect \in BOOLEAN

Init ==
    /\ fq = SetSeq(Frames)
    /\ kcache = 0
    /\ kused = 0
    /\ ring = <<>>
    /\ ahead = 0
    /\ phase = "idle"
    /\ start = 0
    /\ peeked = 0
    /\ taken = 0
    /\ pending = {}
    /\ staged = 0
    /\ took = FALSE
    /\ defect = FALSE

(* KernelReceive: a packet lands in the next frame of the kernel's batch,  *)
(* posted to the RX ring while the ring has a free entry as the kernel     *)
(* sees it.  The fill ring slot stays the kernel's until it publishes.     *)
KernelReceive ==
    /\ kused < kcache
    /\ Len(ring) < RingSize
    /\ kused' = kused + 1
    /\ ring' = Append(ring, fq[kused + 1])
    /\ UNCHANGED <<fq, kcache, ahead, phase, start, peeked, taken, pending, staged, took, defect>>

(* KernelMisdeliver: a driver or kernel defect posts a descriptor naming a *)
(* frame outside the receive region, once.                                 *)
KernelMisdeliver ==
    /\ ~defect
    /\ Len(ring) < RingSize
    /\ ring' = Append(ring, SendFrame)
    /\ defect' = TRUE
    /\ UNCHANGED <<fq, kcache, kused, ahead, phase, start, peeked, taken, pending, staged, took>>

(* KernelRefresh: a packet finds the batch used up.  The kernel publishes  *)
(* its consumer (the taken entries' slots go back to the user) and peeks   *)
(* what the producer shows now.  If that is nothing, the packet is         *)
(* dropped: "rx fill ring empty".                                          *)
KernelRefresh ==
    /\ kused = kcache
    /\ kused > 0 \/ Len(fq) > kcache
    /\ fq' = SubSeq(fq, kused + 1, Len(fq))
    /\ kused' = 0
    /\ kcache' = Len(fq) - kused
    /\ UNCHANGED <<ring, ahead, phase, start, peeked, taken, pending, staged, took, defect>>

(* Peek: xsk_ring_cons__peek from the cached consumer, at most PeekMax;    *)
(* begin the modeled fill pass. A peek of zero is no step.                 *)
Peek ==
    LET n == Min(Len(ring) - ahead, PeekMax) IN
    /\ phase = "idle"
    /\ n > 0
    /\ start' = ahead
    /\ ahead' = ahead + n
    /\ peeked' = n
    /\ taken' = 0
    /\ phase' = "taking"
    /\ UNCHANGED <<fq, kcache, kused, ring, pending, staged, took, defect>>

(* Take: the staging buffer has room; read descriptor start + taken + 1    *)
(* (rx_desc at the peek's index + i) and hold its frame.  A descriptor     *)
(* outside the receive region is refused: no packet is staged and its      *)
(* frame is not held for the fill ring (rocketnet_xdp_fill_return false),  *)
(* unless RefusedRefilled.                                                 *)
Take ==
    LET d == ring[start + taken + 1] IN
    /\ phase = "taking"
    /\ taken < peeked
    /\ staged < StageCap
    /\ pending' = IF d \in Frames \/ RefusedRefilled THEN pending \cup {d} ELSE pending
    /\ taken' = taken + 1
    /\ staged' = IF d \in Frames THEN staged + 1 ELSE staged
    /\ took' = TRUE
    /\ UNCHANGED <<fq, kcache, kused, ring, ahead, phase, start, peeked, defect>>

(* Finish: every peeked descriptor taken, or the staging buffer full.      *)
(* Cancel the rest (CancelUnconsumed), end the fill pass, release the      *)
(* taken count: the consumer advances over the first taken descriptors.    *)
(* Then the pass flushes its pending return into the fill ring, all of it  *)
(* if it fits, else none (a zero reservation keeps every frame).           *)
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
    /\ IF FlushFits
          THEN /\ fq' = fq \o SetSeq(pending)
               /\ pending' = {}
          ELSE UNCHANGED <<fq, pending>>
    /\ UNCHANGED <<kcache, kused, staged, defect>>

(* Refill: the flush again at the top of the worker's next pass; a         *)
(* reservation of zero is this step not yet taken.                         *)
Refill ==
    /\ phase = "idle"
    /\ pending # {}
    /\ FlushFits
    /\ fq' = fq \o SetSeq(pending)
    /\ pending' = {}
    /\ UNCHANGED <<kcache, kused, ring, ahead, phase, start, peeked, taken, staged, took, defect>>

(* Drain: consume the staged bytes in the single-buffer abstraction.      *)
Drain ==
    /\ phase = "idle"
    /\ staged > 0
    /\ staged' = 0
    /\ UNCHANGED <<fq, kcache, kused, ring, ahead, phase, start, peeked, taken, pending, took, defect>>

Next ==
    \/ KernelReceive
    \/ KernelRefresh
    \/ KernelMisdeliver
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

(* The RX ring's descriptors the current pass has not taken: a taken       *)
(* descriptor stays in the kernel's ring until the release, and its frame  *)
(* is already the worker's.                                                *)
(* A descriptor naming SendFrame is the defect's, not a receive frame's.   *)
Untaken == {i \in 1..Len(ring) : i \notin start + 1..start + taken /\ ring[i] \in Frames}
InRing == {ring[i] : i \in Untaken}

(* The fill ring only ever holds receive-region frames: a frame outside    *)
(* the region on it would be received into while it is also the send       *)
(* path's.                                                                 *)
FillHoldsOnlyReceiveFrames == \A i \in 1..Len(fq) : fq[i] \in Frames

(* Every frame is in exactly one of the fill ring (untaken), the RX ring   *)
(* (untaken) or the worker's hands (the staging buffer holds copies),      *)
(* once each, and the fill ring holds nothing else.                        *)
FrameConservation ==
    /\ FillHoldsOnlyReceiveFrames
    /\ Cardinality(Untaken) = Cardinality(InRing)
    /\ Len(fq) - kused = Cardinality(Fill)
    /\ Fill \cap InRing = {}
    /\ Fill \cap pending = {}
    /\ InRing \cap pending = {}
    /\ Fill \cup InRing \cup pending = Frames

(* Between passes the cached consumer is the consumer: every peeked        *)
(* descriptor was released or cancelled.                                   *)
ConsumerNotBehind == phase = "idle" => ahead = 0

(* Between passes the worker holds no frame: every flush fits, because the *)
(* ring has a slot for each frame beyond the kernel's unpublished batch.   *)
PendingReturnedEachPass == phase = "idle" => pending = {}

(* The kernel finds the fill ring empty (no entry it has not taken, so a   *)
(* refresh peeks nothing) only while no frame is free: none is waiting in  *)
(* the worker's hands between passes to go back.                           *)
FillEmpty == kused = Len(fq)

FillNeverEmptyWhileFramesFree == FillEmpty => ~(phase = "idle" /\ pending # {})

(***************************************************************************)
(* Liveness: receive never stalls while frames are free.  Whenever a frame *)
(* is in the fill ring or the RX ring, a pass eventually takes one.        *)
(***************************************************************************)
FramesFree == Fill # {} \/ ring # <<>>

ReceiveNeverStalls == FramesFree ~> took
=============================================================================
