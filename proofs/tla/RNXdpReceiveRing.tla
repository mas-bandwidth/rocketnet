------------------------- MODULE RNXdpReceiveRing -------------------------
(***************************************************************************)
(* rocketnet: the AF_XDP receive ring and the ownership of its frames.     *)
(*                                                                         *)
(* The XDP worker (rocketnet_server_socket_xdp_thread_function,            *)
(* sdk/rocketnet_server_socket_linux.cpp) peeks RX descriptors with        *)
(* xsk_ring_cons__peek, asking for at most receive_limit: the staging      *)
(* buffer's room and the pending return's room, whichever is less.  It     *)
(* reads every descriptor the peek returned (there is no room test in the  *)
(* loop, only an assert compiled out of a release build), releases them   *)
(* with xsk_ring_cons__release (rocketnet_xdp_receive_ring_finish, whose   *)
(* cancel of the descriptors not read is called with zero), and returns    *)
(* their frames to the                                                     *)
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
(* of a pass gives back descriptors that were never read.  cprod is the    *)
(* user's cached producer minus the consumer: libxdp's peek                *)
(* (xsk_cons_nb_avail) reads the producer again only when its cached view  *)
(* is empty, so a peek can hand out fewer descriptors than the kernel has  *)
(* posted; the rest wait for a later pass.                                 *)
(*                                                                         *)
(* The frames go onto the fill ring in the order the code writes them: the *)
(* first fill in frame order, a flush in the pending return's order.  The  *)
(* model does not fix that order (Orders): no rule here depends on it.     *)
(*                                                                         *)
(* A driver or kernel defect can post a descriptor naming a frame outside  *)
(* the receive region (a send frame, SendFrame here): KernelMisdeliver,    *)
(* at most once per behaviour.  The worker reads it and refuses it         *)
(* (rocketnet_xdp_receive_descriptor), and hands a frame back to the fill  *)
(* ring only when rocketnet_xdp_fill_return (sdk/rocketnet_xdp_frame.h)    *)
(* places it in the receive region, so a send frame is never received      *)
(* into while it is also on the send free list.                            *)
(*                                                                         *)
(* The peek's two room terms.  The staging term binds: the staging buffer *)
(* holds copies, so it fills across passes while the frames go back to    *)
(* the fill ring each pass, and without the term the loop writes past the  *)
(* buffer (StagingInBounds; the reversed witness PeekPastStaging).  The   *)
(* pending term never binds in the code: the pending return holds          *)
(* ROCKETNET_XDP_RECV_QUEUE_SIZE frames, which equals                      *)
(* ROCKETNET_XDP_FILL_FRAMES, and it only ever holds receive frames, so    *)
(* its room is never less than the receive frames the RX ring can name.    *)
(* The model carries it at the code's ratio (PendCap = the frames) and     *)
(* checks PendingInBounds; a witness dropping it would pass, so there is   *)
(* none.                                                                   *)
(*                                                                         *)
(* The cancel: rocketnet_xdp_receive_ring_finish cancels the peeked        *)
(* descriptors the pass did not read before releasing the ones it read.   *)
(* With the peek capped by the room, the loop reads every one, so the      *)
(* count it cancels is always zero.  Finish keeps the code's arithmetic    *)
(* (back = peeked - taken) and Finish runs only once taken = peeked, as    *)
(* the loop ends; the cancel is a defence for a pass the code cannot make, *)
(* driven only by tests/test_unit_xdp_receive_ring.cpp.                    *)
(*                                                                         *)
(* Left out: packet contents and the frame codec's drops (a dropped frame  *)
(* is still read and returned; the model stages a packet for every         *)
(* receive frame read, at least as many as the code stages).               *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS
    Frames,       \* the UMEM receive frames
    RingSize,     \* the RX ring's entries
    StageCap,     \* the staging buffer's packets (ROCKETNET_XDP_RECV_QUEUE_SIZE)
    PendCap,      \* the pending return's frames (ROCKETNET_XDP_RECV_QUEUE_SIZE)
    FillSize      \* the fill ring's entries (ROCKETNET_XDP_FILL_RING_SIZE)

ASSUME /\ Frames # {}
       /\ RingSize \in Nat \ {0}
       /\ StageCap \in Nat \ {0}
       /\ PendCap \in Nat \ {0}
       /\ FillSize \in Nat /\ FillSize >= Cardinality(Frames)

(***************************************************************************)
(* Rule switch, overridden by the reversed witness.                        *)
(*   PeekStagingRoom: the peek asks for no more than the staging buffer's  *)
(*     room (receive_capacity in receive_limit; the design: TRUE).         *)
(***************************************************************************)
PeekStagingRoom == TRUE

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
    cprod,     \* cached producer - consumer (user)
    phase,     \* "idle" or "taking" in the single-buffer abstraction
    start,     \* the ring offset the current peek began at
    peeked,    \* the descriptors the current peek returned
    taken,     \* the descriptors the current pass took
    pending,   \* the frames the worker holds: taken, awaiting the fill ring
    staged,    \* the packets in the staging buffer (copies, not frames)
    took,      \* ghost: the current pass has taken a descriptor
    defect     \* the kernel has posted a descriptor naming SendFrame

vars == <<fq, kcache, kused, ring, ahead, cprod, phase, start, peeked, taken, pending, staged, took, defect>>

Range(s) == {s[i] : i \in DOMAIN s}

Min(a, b) == IF a <= b THEN a ELSE b

(* The frames of a set in each order: the flush writes them in one.        *)
Orders(S) == {s \in [1..Cardinality(S) -> S] : Range(s) = S}

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
    /\ cprod \in ahead..Len(ring)
    /\ phase \in {"idle", "taking"}
    /\ start \in 0..RingSize
    /\ peeked \in 0..RingSize
    /\ taken \in 0..RingSize
    /\ pending \subseteq AllFrames
    /\ staged \in Nat
    /\ took \in BOOLEAN
    /\ defect \in BOOLEAN

Init ==
    /\ fq \in Orders(Frames)
    /\ kcache = 0
    /\ kused = 0
    /\ ring = <<>>
    /\ ahead = 0
    /\ cprod = 0
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
    /\ UNCHANGED <<fq, kcache, ahead, cprod, phase, start, peeked, taken, pending, staged, took, defect>>

(* KernelMisdeliver: a driver or kernel defect posts a descriptor naming a *)
(* frame outside the receive region, once.                                 *)
KernelMisdeliver ==
    /\ ~defect
    /\ Len(ring) < RingSize
    /\ ring' = Append(ring, SendFrame)
    /\ defect' = TRUE
    /\ UNCHANGED <<fq, kcache, kused, ahead, cprod, phase, start, peeked, taken, pending, staged, took>>

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
    /\ UNCHANGED <<ring, ahead, cprod, phase, start, peeked, taken, pending, staged, took, defect>>

(* Peek: xsk_ring_cons__peek from the cached consumer, asking for          *)
(* receive_limit, the lesser of the staging buffer's room                  *)
(* (receive_capacity) and the pending return's room                        *)
(* (pending_return.available()); it returns what its cached view holds up  *)
(* to that, reading the producer again (the whole ring) only when that     *)
(* view is empty.  Begin the modeled fill pass.  A peek of zero is no      *)
(* step: it changes nothing, since a refresh that finds nothing new leaves *)
(* the cached producer where it was.  The code peeks only when the limit   *)
(* is above zero.                                                          *)
StageRoom == IF PeekStagingRoom THEN StageCap - staged ELSE RingSize
PendRoom == PendCap - Cardinality(pending)
ReceiveLimit == Min(StageRoom, PendRoom)
Refreshed == IF cprod = ahead THEN Len(ring) ELSE cprod
Peek ==
    LET n == Min(Refreshed - ahead, ReceiveLimit) IN
    /\ phase = "idle"
    /\ ReceiveLimit > 0
    /\ n > 0
    /\ cprod' = Refreshed
    /\ start' = ahead
    /\ ahead' = ahead + n
    /\ peeked' = n
    /\ taken' = 0
    /\ phase' = "taking"
    /\ UNCHANGED <<fq, kcache, kused, ring, pending, staged, took, defect>>

(* Take: read descriptor start + taken + 1 (rx_desc at the peek's index   *)
(* + i) and hold its frame; no room test, as in the loop, which reads      *)
(* every peeked descriptor (its rocketnet_assert on the staging room is    *)
(* compiled out of a release build).  A descriptor                         *)
(* outside the receive region is refused: no packet is staged and its      *)
(* frame is not held for the fill ring (rocketnet_xdp_fill_return false),  *)
(* unless RefusedRefilled.                                                 *)
Take ==
    LET d == ring[start + taken + 1] IN
    /\ phase = "taking"
    /\ taken < peeked
    /\ pending' = IF d \in Frames \/ RefusedRefilled THEN pending \cup {d} ELSE pending
    /\ taken' = taken + 1
    /\ staged' = IF d \in Frames THEN staged + 1 ELSE staged
    /\ took' = TRUE
    /\ UNCHANGED <<fq, kcache, kused, ring, ahead, cprod, phase, start, peeked, defect>>

(* Finish: every peeked descriptor taken, as the loop ends.               *)
(* rocketnet_xdp_receive_ring_finish cancels peeked - taken (zero) and     *)
(* releases the taken count: the consumer advances over the first taken    *)
(* descriptors.  End the fill pass.                                        *)
(* Then the pass flushes its pending return into the fill ring, all of it  *)
(* if it fits, else none (a zero reservation keeps every frame).           *)
Finish ==
    LET back == peeked - taken IN
    /\ phase = "taking"
    /\ taken = peeked
    /\ ring' = SubSeq(ring, taken + 1, Len(ring))
    /\ ahead' = ahead - back - taken
    /\ cprod' = cprod - taken
    /\ phase' = "idle"
    /\ start' = 0
    /\ peeked' = 0
    /\ taken' = 0
    /\ took' = FALSE
    /\ IF FlushFits
          THEN /\ \E order \in Orders(pending) : fq' = fq \o order
               /\ pending' = {}
          ELSE UNCHANGED <<fq, pending>>
    /\ UNCHANGED <<kcache, kused, staged, defect>>

(* Refill: the flush again at the top of the worker's next pass; a         *)
(* reservation of zero is this step not yet taken.                         *)
Refill ==
    /\ phase = "idle"
    /\ pending # {}
    /\ FlushFits
    /\ \E order \in Orders(pending) : fq' = fq \o order
    /\ pending' = {}
    /\ UNCHANGED <<kcache, kused, ring, ahead, cprod, phase, start, peeked, taken, staged, took, defect>>

(* Drain: consume the staged bytes in the single-buffer abstraction.      *)
Drain ==
    /\ phase = "idle"
    /\ staged > 0
    /\ staged' = 0
    /\ UNCHANGED <<fq, kcache, kused, ring, ahead, cprod, phase, start, peeked, taken, pending, took, defect>>

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

(* The staging buffer is never written past its end: the peek asked for   *)
(* no more than its room, and the loop stages at most one packet per       *)
(* descriptor read.                                                        *)
StagingInBounds == staged <= StageCap

(* The pending return never holds more than its capacity (an append past   *)
(* it is refused, and the worker stops).                                   *)
PendingInBounds == Cardinality(pending) <= PendCap

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
