---------------------------- MODULE RNXdpReceiveRing ----------------------------
(***************************************************************************)
(* The AF_XDP RX ring's producer, peek, consume, cancel, release and the   *)
(* fill-ring return, for one queue.                                         *)
(*                                                                          *)
(* A frame token stands for one UMEM frame. It sits in exactly one of the  *)
(* fill ring, the visible RX ring, the staging buffer or the application.  *)
(* The kernel produces by moving a frame from the fill ring onto the tail  *)
(* of the visible RX ring. A receive pass peeks the whole visible ring     *)
(* (the pending-return room is at least the ring), copies frames into the  *)
(* staging buffer until that buffer is full, and releases only those.      *)
(* Cancel puts the unconsumed tail back on the visible ring. Release       *)
(* without cancel drops that tail out of every place the next peek, the    *)
(* fill ring, the staging buffer and the application can see.              *)
(*                                                                          *)
(* The implementation returns the UMEM address to the fill ring at consume *)
(* time and keeps a copy of the packet in the staging buffer. This module  *)
(* follows the packet token until the application returns it to the fill   *)
(* ring. The lost token is the descriptor the receive loop does not read   *)
(* and does not hand back.                                                  *)
(*                                                                          *)
(* rx is the visible suffix, the descriptors a peek still reads, which is  *)
(* the index range from cached_cons to the producer. skipped is the gap    *)
(* from the published consumer index up to cached_cons. After a pass that  *)
(* cancels, that gap is empty and cached_cons equals the consumer index.   *)
(* staging holds at most StagingCap frames. The application drains it and  *)
(* later returns a frame to the fill ring.                                  *)
(*                                                                          *)
(* CancelUnconsumed is TRUE in the receive loop. The reversed witness sets *)
(* it FALSE: release without cancel.                                        *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS
    RingSize,      \* descriptor capacity of the RX ring
    StagingCap,    \* receive staging buffer capacity
    FrameCount     \* UMEM frames in this instance

ASSUME /\ RingSize \in Nat \ {0}
       /\ StagingCap \in Nat \ {0}
       /\ FrameCount \in Nat \ {0}
       /\ FrameCount <= RingSize
       /\ StagingCap <= RingSize

CancelUnconsumed == TRUE

Frames == 1..FrameCount

VARIABLES
    fill,       \* set of frames in the fill ring
    rx,         \* sequence of frames a peek still reads, oldest first
    skipped,    \* sequence of frames in the consumer..cached_cons gap
    staging,    \* sequence of frames in the staging buffer, oldest first
    app         \* set of frames the application holds

vars == <<fill, rx, skipped, staging, app>>

SeqSet(s) == { s[i] : i \in DOMAIN s }

TypeOK ==
    /\ fill \subseteq Frames
    /\ rx \in Seq(Frames)
    /\ skipped \in Seq(Frames)
    /\ staging \in Seq(Frames)
    /\ Len(staging) <= StagingCap
    /\ app \subseteq Frames
    /\ Len(rx) + Len(skipped) <= RingSize

FramePartition ==
    LET rxS == SeqSet(rx)
        stS == SeqSet(staging)
        skS == SeqSet(skipped)
    IN /\ skS = {}
       /\ fill \cup rxS \cup stS \cup app = Frames
       /\ Cardinality(fill) + Cardinality(rxS) + Cardinality(stS) + Cardinality(app) = FrameCount
       /\ fill \cap rxS = {}
       /\ fill \cap stS = {}
       /\ fill \cap app = {}
       /\ rxS \cap stS = {}
       /\ rxS \cap app = {}
       /\ stS \cap app = {}

Init ==
    /\ fill = Frames
    /\ rx = <<>>
    /\ skipped = <<>>
    /\ staging = <<>>
    /\ app = {}

ProduceFrame(f) ==
    /\ f \in fill
    /\ Len(rx) + Len(skipped) < RingSize
    /\ fill' = fill \ {f}
    /\ rx' = rx \o <<f>>
    /\ UNCHANGED <<skipped, staging, app>>

Produce == \E f \in Frames : ProduceFrame(f)

Receive ==
    /\ Len(rx) > 0
    /\ LET room == StagingCap - Len(staging)
           n == Len(rx)
           consumed == IF n < room THEN n ELSE room
       IN /\ \/ consumed > 0
             \/ ~CancelUnconsumed
          /\ LET taken == IF consumed = 0 THEN <<>> ELSE SubSeq(rx, 1, consumed)
                 rest == IF consumed = n THEN <<>> ELSE SubSeq(rx, consumed + 1, n)
             IN /\ staging' = staging \o taken
                /\ rx' = IF CancelUnconsumed THEN rest ELSE <<>>
                /\ skipped' = IF CancelUnconsumed THEN skipped ELSE skipped \o rest
                /\ UNCHANGED <<fill, app>>

Drain ==
    /\ Len(staging) > 0
    /\ app' = app \cup {Head(staging)}
    /\ staging' = Tail(staging)
    /\ UNCHANGED <<fill, rx, skipped>>

Return ==
    /\ \E f \in app :
        /\ app' = app \ {f}
        /\ fill' = fill \cup {f}
        /\ UNCHANGED <<rx, skipped, staging>>

Next == Produce \/ Receive \/ Drain \/ Return

Fairness ==
    /\ \A f \in Frames : WF_vars(ProduceFrame(f))
    /\ WF_vars(Receive)
    /\ WF_vars(Drain)
    /\ WF_vars(Return)

Spec == Init /\ [][Next]_vars /\ Fairness

(***************************************************************************)
(* A frame in the fill ring is eventually delivered to the application,  *)
(* and a frame in the visible RX ring is eventually staged or delivered.   *)
(* Receive does not stall while a frame is free. Per-frame weak fairness   *)
(* on ProduceFrame stops the kernel from producing only the other frames.  *)
(***************************************************************************)
ReceiveNeverStalls ==
    \A f \in Frames :
        /\ []( (f \in fill) => (<>(f \in app)) )
        /\ []( (f \in SeqSet(rx)) => (<>(f \in SeqSet(staging) \/ f \in app)) )

=============================================================================
