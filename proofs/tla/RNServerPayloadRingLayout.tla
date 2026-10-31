--------------------- MODULE RNServerPayloadRingLayout ---------------------
(***************************************************************************)
(* The portable server transport's payload ring, as laid out in            *)
(* sdk/rocketnet_server_transport.cpp, with the code's own arithmetic: the *)
(* counter num_packets, the info array of RingSize records (to,            *)
(* client_index, wire_bytes) and the data array of RingSize frames at a    *)
(* fixed stride.                                                           *)
(*                                                                         *)
(* Game threads acquire slots between updates                              *)
(* (rocketnet_server_transport_start_packet and                            *)
(* rocketnet_server_reserve_fill_frame): one fetch-add on num_packets      *)
(* returns the slot, and a slot at or past send_capacity (Cap, at most     *)
(* RingSize) is refused, though the counter has already moved past it.  A  *)
(* slot handed out has its info written (wire_bytes 0) and its frame at    *)
(* data + slot * stride.  A finish writes wire_bytes and an abort clears   *)
(* it; both find the slot from the frame pointer, (frame - data) / stride  *)
(* (rocketnet_server_transport_finish_packet_unchecked,                    *)
(* rocketnet_server_transport_abort_packet_unchecked), or from the slot    *)
(* the fill frame kept (rocketnet_server_commit_fill_frame,                *)
(* rocketnet_server_abort_fill_frame).  The flush                          *)
(* (rocketnet_server_transport_send_packets_impl) reads the counter,       *)
(* clamps it to Cap, sends every slot below it whose wire_bytes is         *)
(* positive, and sets the counter to 0.                                    *)
(*                                                                         *)
(* The flush runs with no packet between start and finish: the caller's    *)
(* contract that the game threads' sends of an update end before its flush *)
(* (a lease left open is RNDriverSendStaging's).  A frame is an offset     *)
(* into data.  Acquires are bounded by MaxCount so the counter, which has  *)
(* no bound of its own between flushes, stays finite; MaxCount above Cap   *)
(* reaches refused acquires.  Every packet carries wire_bytes 1: identity  *)
(* is the slot.                                                            *)
(*                                                                         *)
(* Reversed witnesses (each flag FALSE is a real bug class):               *)
(*   Clamp         the flush walks the raw counter                         *)
(*   FullTest      the refusal test is slot > Cap, one late                *)
(*   RecoverStride the slot is recovered by a stride other than the        *)
(*                 frames' (WrongStride, a frame size below the stride)    *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS
    Threads,        \* game threads
    Cap,            \* send_capacity
    RingSize,       \* ROCKETNET_SERVER_PAYLOAD_RING_SIZE
    Stride,         \* ROCKETNET_FRAME_STRIDE
    WrongStride,    \* the stride the broken recovery divides by
    MaxCount,       \* acquires between flushes in the bounded instance
    Clamp,          \* the flush clamps the counter to Cap
    FullTest,       \* the refusal test is slot >= Cap
    RecoverStride   \* the slot is recovered as frame / Stride

ASSUME Cap \in Nat \ {0} /\ RingSize \in Nat /\ Cap <= RingSize
ASSUME Stride \in Nat \ {0} /\ WrongStride \in Nat \ {0} /\ MaxCount \in Nat
ASSUME \A f \in {Clamp, FullTest, RecoverStride} : f \in BOOLEAN

Slots == 0..(RingSize - 1)
IDLE == [held |-> FALSE, frame |-> 0, slot |-> 0]

VARIABLES
    count,          \* num_packets
    info,           \* info[i]: [wb, who], wire_bytes and the client (here the thread)
    thread,         \* thread[t]: held, its frame offset and (ghost) the slot it was handed
    finished,       \* ghost: the slots finished since the last flush
    outOfBounds,    \* ghost: a slot or info record past the ring, or past Cap, used
    badFlush        \* ghost: a flush sent other than exactly the finished slots

vars == <<count, info, thread, finished, outOfBounds, badFlush>>

Init ==
    /\ count = 0
    /\ info = [i \in Slots |-> [wb |-> 0, who |-> CHOOSE t \in Threads : TRUE]]
    /\ thread = [t \in Threads |-> IDLE]
    /\ finished = {}
    /\ outOfBounds = FALSE
    /\ badFlush = FALSE

Refused(slot) == IF FullTest THEN slot >= Cap ELSE slot > Cap

\* the slot a finish or an abort writes: (frame - data) / stride
Recovered(t) == thread[t].frame \div (IF RecoverStride THEN Stride ELSE WrongStride)

\* write wire_bytes w into info[i], recording an index past the ring
WriteWb(i, w) ==
    /\ info' = IF i \in Slots THEN [info EXCEPT ![i].wb = w] ELSE info
    /\ outOfBounds' = (outOfBounds \/ i \notin Slots \/ i >= Cap)

\* rocketnet_server_transport_start_packet's portable branch and
\* rocketnet_server_reserve_fill_frame: the fetch-add, the refusal at
\* send_capacity, the info written and the frame at data + slot * stride
Acquire(t) ==
    /\ ~thread[t].held
    /\ count < MaxCount
    /\ count' = count + 1
    /\ IF Refused(count)
       THEN UNCHANGED <<info, thread, outOfBounds>>
       ELSE /\ info' = IF count \in Slots
                       THEN [info EXCEPT ![count] = [wb |-> 0, who |-> t]]
                       ELSE info
            /\ thread' = [thread EXCEPT ![t] = [held |-> TRUE, frame |-> count * Stride, slot |-> count]]
            /\ outOfBounds' = (outOfBounds \/ count \notin Slots \/ count >= Cap)
    /\ UNCHANGED <<finished, badFlush>>

\* rocketnet_server_transport_finish_packet_unchecked's portable branch (the slot from
\* the frame pointer) and rocketnet_server_commit_fill_frame: wire_bytes set
Finish(t) ==
    /\ thread[t].held
    /\ WriteWb(Recovered(t), 1)
    /\ thread' = [thread EXCEPT ![t] = IDLE]
    /\ finished' = finished \cup {thread[t].slot}
    /\ UNCHANGED <<count, badFlush>>

\* rocketnet_server_transport_abort_packet_unchecked's portable branch and
\* rocketnet_server_abort_fill_frame: wire_bytes cleared
Abort(t) ==
    /\ thread[t].held
    /\ WriteWb(Recovered(t), 0)
    /\ thread' = [thread EXCEPT ![t] = IDLE]
    /\ UNCHANGED <<count, finished, badFlush>>

\* rocketnet_server_transport_send_packets_impl's portable flush: the counter
\* clamped to send_capacity, every slot below it with wire_bytes > 0 sent,
\* the counter reset
Walked == 0..((IF Clamp /\ count > Cap THEN Cap ELSE count) - 1)
Sent == { i \in Walked : i \in Slots /\ info[i].wb > 0 }
Flush ==
    /\ \A t \in Threads : ~thread[t].held
    /\ badFlush' = (badFlush \/ Sent # finished)
    /\ outOfBounds' = (outOfBounds \/ \E i \in Walked : i \notin Slots \/ i >= Cap)
    /\ count' = 0
    /\ finished' = {}
    /\ UNCHANGED <<info, thread>>

Next ==
    \/ \E t \in Threads : Acquire(t) \/ Finish(t) \/ Abort(t)
    \/ Flush

Spec == Init /\ [][Next]_vars

----------------------------------------------------------------------------

TypeOK ==
    /\ count \in 0..MaxCount
    /\ info \in [Slots -> [wb : {0, 1}, who : Threads]]
    /\ thread \in [Threads -> [held : BOOLEAN, frame : 0..(MaxCount * Stride), slot : 0..MaxCount]]
    /\ finished \subseteq 0..MaxCount
    /\ outOfBounds \in BOOLEAN /\ badFlush \in BOOLEAN

\* no slot at or past send_capacity, and no info record past the ring, is
\* handed out, written or read
InBounds == ~outOfBounds

\* each flush sends exactly the slots finished since the last one
ExactSend == ~badFlush

\* a held slot is below Cap, its info names its thread, its frame is its root
\* and no two threads hold one slot
HeldExclusive ==
    /\ \A t \in Threads : thread[t].held =>
          /\ thread[t].slot < Cap
          /\ thread[t].frame = thread[t].slot * Stride
          /\ info[thread[t].slot].who = t
    /\ \A t, u \in Threads : t # u /\ thread[t].held /\ thread[u].held => thread[t].slot # thread[u].slot

=============================================================================
