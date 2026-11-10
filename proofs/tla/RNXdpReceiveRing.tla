------------------------- MODULE RNXdpReceiveRing -------------------------
(***************************************************************************)
(* rocketnet: the AF_XDP receive ring and the ownership of its frames.     *)
(*                                                                         *)
(* The XDP worker (rocketnet_server_socket_xdp_thread_function,            *)
(* sdk/rocketnet_server_socket_linux.cpp) peeks RX descriptors with        *)
(* xsk_ring_cons__peek, takes them while the current receive bank has      *)
(* room, releases the ones it took with xsk_ring_cons__release             *)
(* (rocketnet_xdp_receive_ring_finish), and returns their frames to the    *)
(* fill ring through its pending return (rocketnet_pending_frames_t, an    *)
(* all-or-zero flush).  The peek is bounded by the bank's remaining room,  *)
(* so every peeked descriptor is taken and the cancel branch is dead.      *)
(*                                                                         *)
(* The receive side is a three-bank handoff.  The worker appends to its    *)
(* current producer bank under the receive mutex and never publishes;      *)
(* main holds one bank as its stable view and swaps it, under the same     *)
(* mutex, for the worker's filled bank once it has consumed every packet   *)
(* of the bank it holds.  The first swap takes a spare, and a retained     *)
(* read cursor carries a partly consumed bank into the next receive call.  *)
(* Main's swap is a try-acquire and never waits; a busy mutex defers the    *)
(* queue to main's next call.  A receive call also has a packet budget:    *)
(* when the budget ends inside a bank, main keeps that bank and its read   *)
(* cursor for the next call, so every packet staged before a claim is in   *)
(* that claim's view.  Drain is that claim; Consume is main reading one    *)
(* packet from the bank it holds; NewCall starts the next receive call     *)
(* with a fresh budget, carrying the cursor.                               *)
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
    StageCap,     \* packets one receive bank holds (ROCKETNET_XDP_RECV_QUEUE_SIZE)
    FillSize,     \* the fill ring's entries (ROCKETNET_XDP_FILL_RING_SIZE)
    Budget,       \* packets one receive call may consume
    MaxPackets    \* packets that arrive in one behaviour

ASSUME /\ Frames # {}
       /\ RingSize \in Nat \ {0}
       /\ PeekMax \in Nat \ {0}
       /\ StageCap \in Nat \ {0}
       /\ FillSize \in Nat /\ FillSize >= Cardinality(Frames)
       /\ Budget \in Nat \ {0}
       /\ MaxPackets \in Nat \ {0}

(***************************************************************************)
(* Rule switch, overridden by the reversed witness.                        *)
(*   RefusedRefilled: a descriptor naming a frame outside the receive      *)
(*     region is handed back to the fill ring like any dropped frame (the  *)
(*     worker before rocketnet_xdp_fill_return: FALSE in the design).      *)
(***************************************************************************)
RefusedRefilled == FALSE

(***************************************************************************)
(*   DrainStale: a claim leaves the most recently staged packet behind in  *)
(*     the worker's next bank, as the worker-driven publish deferred a     *)
(*     packet staged after it to a later claim, so a packet staged before *)
(*     the claim was not in that claim's view. FALSE in the design.        *)
(***************************************************************************)
DrainStale == FALSE

(* The frame a defective descriptor names: one the model does not own for  *)
(* receive (a send-region frame).  An untyped model value Frames holds is  *)
(* never equal to it.                                                      *)
SendFrame == "send frame"

AllFrames == Frames \cup {SendFrame}

Banks == 0 .. 2

NONE == 3

VARIABLES
    fq,          \* the fill ring's entries, published consumer to producer
    kcache,      \* the entries at the front of fq the kernel's batch peeked
    kused,       \* the entries of that batch the kernel has taken
    ring,        \* the RX ring's descriptors, consumer to producer (kernel view)
    ahead,       \* cached consumer - consumer (user)
    phase,       \* "idle" or "taking" in the fill pass
    start,       \* the ring offset the current peek began at
    peeked,      \* the descriptors the current peek returned
    taken,       \* the descriptors the current pass took
    pending,     \* the frames the worker holds: taken, awaiting the fill ring
    workerBank,  \* the receive bank the worker stages into (the producer)
    mainBank,    \* the receive bank main holds, or NONE
    freeBanks,   \* the spare receive banks, oldest first
    bankContent, \* the packet ids staged in each receive bank
    readCursor,  \* packets of mainBank main has consumed
    budget,      \* packets this receive call may still consume
    nextPacket,  \* the next packet staged
    claimMark,   \* ghost: nextPacket at main's last claim
    consumed,    \* ghost: the packet ids main consumed, in order
    took,        \* ghost: the current pass has taken a descriptor
    defect       \* the kernel has posted a descriptor naming SendFrame

vars == <<fq, kcache, kused, ring, ahead, phase, start, peeked, taken, pending,
           workerBank, mainBank, freeBanks, bankContent, readCursor, budget,
           nextPacket, claimMark, consumed, took, defect>>

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
    /\ workerBank \in Banks
    /\ mainBank \in Banks \cup {NONE}
    /\ freeBanks \in Seq(Banks)
    /\ bankContent \in [Banks -> Seq(1..MaxPackets)]
    /\ \A b \in Banks : Len(bankContent[b]) <= StageCap
    /\ readCursor \in 0..StageCap
    /\ budget \in 0..Budget
    /\ nextPacket \in 0..MaxPackets
    /\ claimMark \in 0..MaxPackets
    /\ consumed \in Seq(1..MaxPackets)
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
    /\ workerBank = 0
    /\ mainBank = NONE
    /\ freeBanks = <<1, 2>>
    /\ bankContent = [b \in Banks |-> <<>>]
    /\ readCursor = 0
    /\ budget = Budget
    /\ nextPacket = 0
    /\ claimMark = 0
    /\ consumed = <<>>
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
    /\ UNCHANGED <<fq, kcache, ahead, phase, start, peeked, taken, pending,
                    workerBank, mainBank, freeBanks, bankContent, readCursor,
                    budget, nextPacket, claimMark, consumed, took, defect>>

(* KernelMisdeliver: a driver or kernel defect posts a descriptor naming a *)
(* frame outside the receive region, once.                                 *)
KernelMisdeliver ==
    /\ ~defect
    /\ Len(ring) < RingSize
    /\ ring' = Append(ring, SendFrame)
    /\ defect' = TRUE
    /\ UNCHANGED <<fq, kcache, kused, ahead, phase, start, peeked, taken, pending,
                    workerBank, mainBank, freeBanks, bankContent, readCursor,
                    budget, nextPacket, claimMark, consumed, took>>

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
    /\ UNCHANGED <<ring, ahead, phase, start, peeked, taken, pending,
                    workerBank, mainBank, freeBanks, bankContent, readCursor,
                    budget, nextPacket, claimMark, consumed, took, defect>>

(* Peek: xsk_ring_cons__peek from the cached consumer, at most PeekMax and *)
(* at most the current bank's remaining room; begin the modeled fill pass. *)
(* The bank room bound is why the pass takes every descriptor it peeks and *)
(* the finish's cancel branch is dead.  A peek of zero is no step.         *)
Peek ==
    LET room == StageCap - Len(bankContent[workerBank])
        n == Min(Len(ring) - ahead, Min(PeekMax, room)) IN
    /\ phase = "idle"
    /\ n > 0
    /\ start' = ahead
    /\ ahead' = ahead + n
    /\ peeked' = n
    /\ taken' = 0
    /\ phase' = "taking"
    /\ UNCHANGED <<fq, kcache, kused, ring, pending, workerBank, mainBank,
                    freeBanks, bankContent, readCursor, budget, nextPacket,
                    claimMark, consumed, took, defect>>

(* Take: read descriptor start + taken + 1 (rx_desc at the peek's index +  *)
(* i) and hold its frame.  A receive-region descriptor is also staged as   *)
(* the next packet in the worker's current bank; a main claim may swap     *)
(* the bank between takes, so the staging reads the bank as it is now.     *)
(* A descriptor outside the receive region is refused: no packet is        *)
(* staged and its frame is not held for the fill ring                      *)
(* (rocketnet_xdp_fill_return false), unless RefusedRefilled.              *)
Take ==
    LET d == ring[start + taken + 1] IN
    /\ phase = "taking"
    /\ taken < peeked
    /\ (d \notin Frames \/ (Len(bankContent[workerBank]) < StageCap /\ nextPacket < MaxPackets))
    /\ pending' = IF d \in Frames \/ RefusedRefilled THEN pending \cup {d} ELSE pending
    /\ taken' = taken + 1
    /\ bankContent' = IF d \in Frames
                         THEN [bankContent EXCEPT ![workerBank] = Append(@, nextPacket + 1)]
                         ELSE bankContent
    /\ nextPacket' = IF d \in Frames THEN nextPacket + 1 ELSE nextPacket
    /\ took' = TRUE
    /\ UNCHANGED <<fq, kcache, kused, ring, ahead, phase, start, peeked,
                    workerBank, mainBank, freeBanks, readCursor, budget,
                    claimMark, consumed, defect>>

(* Finish: every peeked descriptor was taken (the peek was bounded by the  *)
(* bank's room).  End the fill pass and release the taken count: the       *)
(* consumer advances over the first taken descriptors.  Then the pass      *)
(* flushes its pending return into the fill ring, all of it if it fits,    *)
(* else none (a zero reservation keeps every frame).                       *)
Finish ==
    /\ phase = "taking"
    /\ taken = peeked
    /\ ring' = SubSeq(ring, taken + 1, Len(ring))
    /\ ahead' = ahead - taken
    /\ phase' = "idle"
    /\ start' = 0
    /\ peeked' = 0
    /\ taken' = 0
    /\ took' = FALSE
    /\ IF FlushFits
          THEN /\ fq' = fq \o SetSeq(pending)
               /\ pending' = {}
          ELSE UNCHANGED <<fq, pending>>
    /\ UNCHANGED <<kcache, kused, workerBank, mainBank, freeBanks, bankContent,
                    readCursor, budget, nextPacket, claimMark, consumed, defect>>

(* Refill: the flush again at the top of the worker's next pass; a         *)
(* reservation of zero is this step not yet taken.                         *)
Refill ==
    /\ phase = "idle"
    /\ pending # {}
    /\ FlushFits
    /\ fq' = fq \o SetSeq(pending)
    /\ pending' = {}
    /\ UNCHANGED <<kcache, kused, ring, ahead, phase, start, peeked, taken,
                    workerBank, mainBank, freeBanks, bankContent, readCursor,
                    budget, nextPacket, claimMark, consumed, took, defect>>

(* Drain: main claims the worker's filled bank, exchanging the exhausted  *)
(* bank main holds (or a spare on the first claim) under the receive      *)
(* mutex.  A claim happens at the start of a receive call, only once      *)
(* main has consumed every packet of the bank it holds.  Every packet     *)
(* staged before the claim is in the claimed bank's view; DrainStale      *)
(* instead leaves the most recently staged packet in the worker's new     *)
(* bank, as the pre-fix worker publish did.                               *)
Drain ==
    LET c == bankContent[workerBank]
        stale == DrainStale /\ c # <<>>
        view == IF stale THEN SubSeq(c, 1, Len(c) - 1) ELSE c
        left == IF stale THEN <<c[Len(c)]>> ELSE <<>> IN
    /\ budget = Budget
    /\ IF mainBank = NONE THEN TRUE ELSE readCursor = Len(bankContent[mainBank])
    /\ (mainBank # NONE \/ freeBanks # <<>>)
    /\ workerBank' = IF mainBank = NONE THEN Head(freeBanks) ELSE mainBank
    /\ freeBanks' = IF mainBank = NONE THEN Tail(freeBanks) ELSE freeBanks
    /\ mainBank' = workerBank
    /\ bankContent' = IF mainBank = NONE
                         THEN [bankContent EXCEPT ![workerBank] = view]
                         ELSE [bankContent EXCEPT ![workerBank] = view, ![mainBank] = left]
    /\ readCursor' = 0
    /\ budget' = Budget
    /\ claimMark' = nextPacket
    /\ UNCHANGED <<fq, kcache, kused, ring, ahead, phase, start, peeked, taken,
                    pending, nextPacket, consumed, took, defect>>

(* Consume: main reads one packet from the bank it holds, advancing its    *)
(* read cursor and spending one packet of the call's budget.  A budget    *)
(* that ends here leaves the cursor for the next call: the retained tail  *)
(* is not claimed again until it is consumed.                              *)
Consume ==
    LET held == IF mainBank = NONE THEN <<>> ELSE bankContent[mainBank] IN
    /\ mainBank # NONE
    /\ readCursor < Len(held)
    /\ budget > 0
    /\ consumed' = Append(consumed, held[readCursor + 1])
    /\ readCursor' = readCursor + 1
    /\ budget' = budget - 1
    /\ UNCHANGED <<fq, kcache, kused, ring, ahead, phase, start, peeked, taken,
                    pending, workerBank, mainBank, freeBanks, bankContent,
                    nextPacket, claimMark, took, defect>>

(* NewCall: main's next receive call starts with a fresh budget, keeping   *)
(* the bank and read cursor its previous call left behind.                 *)
NewCall ==
    /\ budget < Budget
    /\ budget' = Budget
    /\ UNCHANGED <<fq, kcache, kused, ring, ahead, phase, start, peeked, taken,
                    pending, workerBank, mainBank, freeBanks, bankContent,
                    readCursor, nextPacket, claimMark, consumed, took, defect>>

Next ==
    \/ KernelReceive
    \/ KernelRefresh
    \/ KernelMisdeliver
    \/ Peek
    \/ Take
    \/ Finish
    \/ Refill
    \/ Drain
    \/ Consume
    \/ NewCall

(* The application consumes and claims whenever the worker is between      *)
(* passes, and the worker takes whenever the ring has descriptors.         *)
Spec == Init /\ [][Next]_vars
        /\ WF_vars(KernelReceive) /\ WF_vars(KernelRefresh)
        /\ WF_vars(Peek) /\ WF_vars(Take) /\ WF_vars(Finish) /\ WF_vars(Refill)
        /\ WF_vars(Consume) /\ WF_vars(NewCall) /\ SF_vars(Drain)

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
(* (untaken) or the worker's hands (the receive banks hold copies), once   *)
(* each, and the fill ring holds nothing else.                             *)
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

(* Every receive bank has exactly one owner: the worker's producer, main's *)
(* held bank, or the spare queue.                                          *)
BankOneOwner ==
    \A b \in Banks :
        (IF workerBank = b THEN 1 ELSE 0)
      + (IF mainBank = b THEN 1 ELSE 0)
      + Len(SelectSeq(freeBanks, LAMBDA x : x = b)) = 1

(* Every packet staged before a claim is in that claim's view: each packet *)
(* staged before main's last claim was consumed earlier or is in the bank  *)
(* main now holds, none left behind in the worker's bank (the reversed     *)
(* DrainStale does leave the most recently staged one behind).             *)
StagedBeforeClaimInView ==
    LET view == IF mainBank = NONE THEN {} ELSE Range(bankContent[mainBank]) IN
    \A n \in 1..claimMark : n \in Range(consumed) \cup view

(* The kernel finds the fill ring empty (no entry it has not taken, so a   *)
(* refresh peeks nothing) only while no frame is free: none is waiting in  *)
(* the worker's hands between passes to go back.                           *)
FillEmpty == kused = Len(fq)

FillNeverEmptyWhileFramesFree == FillEmpty => ~(phase = "idle" /\ pending # {})

(***************************************************************************)
(* Liveness: receive never stalls while frames are free and another packet *)
(* may still arrive.  Whenever a frame is in the fill ring or the RX ring, *)
(* a pass eventually takes one.                                            *)
(***************************************************************************)
FramesFree == Fill # {} \/ ring # <<>>

ReceiveNeverStalls == (FramesFree /\ nextPacket < MaxPackets) ~> took
=============================================================================
