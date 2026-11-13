--------------------- MODULE RNServerPayloadRingLayout ---------------------
(***************************************************************************)
(* The portable server transport's payload ring, as laid out in            *)
(* sdk/rocketnet_server_transport.cpp, with the code's own arithmetic: the *)
(* counter num_packets, the info array of RingSize records (to,            *)
(* client_index, wire_bytes), the data array of RingSize frames at a       *)
(* fixed stride, and each fill line's chunk word (fill_lines[i].ring,      *)
(* [end:32][next:32]).                                                     *)
(*                                                                         *)
(* Game threads take slots between updates                                 *)
(* (rocketnet_server_transport_start_packet, through                       *)
(* rocketnet_server_payload_ring_take), on the line of the calling thread  *)
(* (LineOf: lines are shared, so two threads may take on one line).  A     *)
(* take is a loop of single atomic steps, each one step of Acquire:        *)
(*   look     the line's chunk holds a slot (next < end): one              *)
(*            compare-and-swap takes next (a lost swap reloads, so the    *)
(*            take is one step); else the word is kept for the install;   *)
(*   guard    the counter is read: while the room left, send_capacity -   *)
(*            num_packets, is at least Chunk slots for each of GuardLines  *)
(*            lines (ROCKETNET_SERVER_FILL_CHUNK, FILL_LINES), a chunk is *)
(*            claimed, else one slot;                                      *)
(*   claim    one fetch-add of Chunk: a base at or past send_capacity is   *)
(*            refused; else the end is clipped at send_capacity, a chunk   *)
(*            of one slot is taken whole, and a longer one goes to install;*)
(*   install  a compare-and-swap of the line from the kept word to the     *)
(*            chunk's tail, taking the base; a line another thread on it  *)
(*            refilled first keeps that chunk: this chunk's slots are     *)
(*            marked empty (wire_bytes 0) and the take looks again;        *)
(*   one      one fetch-add of one slot, refused at send_capacity.         *)
(* A refused claim or one still moves the counter past send_capacity.  A   *)
(* slot taken has its info written (wire_bytes 0) and its frame at         *)
(* data + slot * stride.  A finish writes wire_bytes and an abort clears   *)
(* it; both find the slot from the frame pointer, (frame - data) / stride  *)
(* (rocketnet_server_transport_finish_packet_unchecked,                    *)
(* rocketnet_server_transport_abort_packet_unchecked).  The flush          *)
(* (rocketnet_server_transport_flush) first marks every line's unused      *)
(* chunk slots empty and clears the lines                                  *)
(* (rocketnet_server_payload_ring_return_chunks), then reads the counter,  *)
(* clamps it to Cap, sends every slot below it whose wire_bytes is         *)
(* positive, and sets the counter to 0.  The info records are not cleared  *)
(* by the flush: a slot below the counter that no take of this update      *)
(* wrote still holds a wire_bytes of an earlier update, which only the two *)
(* empty marks keep out of the send.                                       *)
(*                                                                         *)
(* The flush runs with no take in progress and no packet between start and *)
(* finish: the caller's contract that the game threads' sends of an update *)
(* end before its flush (a lease left open is RNDriverSendStaging's).  A   *)
(* frame is an offset into data.  A take starts only while the counter is  *)
(* below MaxCount, so the counter, which has no bound of its own between   *)
(* flushes, stays finite; MaxCount above Cap reaches refused takes.  Every *)
(* packet carries wire_bytes 1: identity is the slot.  The DEV hook        *)
(* steal_ring_chunk_for_test is a second thread on the line, which the     *)
(* model has as one.                                                       *)
(*                                                                         *)
(* Reversed witnesses (each flag FALSE is a real bug class):               *)
(*   Clamp         the flush walks the raw counter                         *)
(*   FullTest      the refusal test is slot > Cap, one late                *)
(*   RecoverStride the slot is recovered by a stride other than the        *)
(*                 frames' (WrongStride, a frame size below the stride)    *)
(*   ReturnClear   the flush does not mark the lines' unused chunk slots   *)
(*                 empty                                                   *)
(*   LostClear     a lost install does not mark its chunk's slots empty    *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS
    Threads,        \* game threads
    LineOf,         \* LineOf[t]: the fill line thread t takes on
    Cap,            \* send_capacity
    RingSize,       \* payload_ring.size, allocated from configured max_clients
    Stride,         \* ROCKETNET_FRAME_STRIDE
    WrongStride,    \* the stride the broken recovery divides by
    Chunk,          \* ROCKETNET_SERVER_FILL_CHUNK
    GuardLines,     \* ROCKETNET_FILL_PHASE_FILL_LINES, the guard's multiplier
    MaxCount,       \* a take starts only while the counter is below this
    Clamp,          \* the flush clamps the counter to Cap
    FullTest,       \* the refusal test is slot >= Cap
    RecoverStride,  \* the slot is recovered as frame / Stride
    ReturnClear,    \* the flush marks the lines' unused chunk slots empty
    LostClear       \* a lost install marks its chunk's slots empty

ASSUME Cap \in Nat \ {0} /\ RingSize \in Nat /\ Cap <= RingSize
ASSUME Stride \in Nat \ {0} /\ WrongStride \in Nat \ {0} /\ MaxCount \in Nat
ASSUME Chunk \in Nat \ {0, 1} /\ GuardLines \in Nat \ {0}
ASSUME LineOf \in [Threads -> Nat]
ASSUME \A f \in {Clamp, FullTest, RecoverStride, ReturnClear, LostClear} : f \in BOOLEAN

Slots == 0..(RingSize - 1)
Lines == { LineOf[t] : t \in Threads }
EMPTY == [next |-> 0, end |-> 0]
PCs == {"idle", "look", "guard", "claim", "install", "one", "held"}
IDLE == [pc |-> "idle", frame |-> 0, slot |-> 0, word |-> EMPTY, base |-> 0, end |-> 0]

VARIABLES
    count,          \* num_packets
    line,           \* line[l]: fill_lines[l].ring, the chunk [next, end)
    info,           \* info[i]: [wb, who], wire_bytes and the client (here the thread)
    thread,         \* thread[t]: its step of the take, its frame offset, (ghost) the slot
                    \* it holds, the word its look kept and the chunk its claim got
    finished,       \* ghost: the slots finished since the last flush
    outOfBounds,    \* ghost: a slot or info record past the ring, or past Cap, used
    badFlush        \* ghost: a flush sent other than exactly the finished slots

vars == <<count, line, info, thread, finished, outOfBounds, badFlush>>

Init ==
    /\ count = 0
    /\ line = [l \in Lines |-> EMPTY]
    /\ info = [i \in Slots |-> [wb |-> 0, who |-> CHOOSE t \in Threads : TRUE]]
    /\ thread = [t \in Threads |-> IDLE]
    /\ finished = {}
    /\ outOfBounds = FALSE
    /\ badFlush = FALSE

Held(t) == thread[t].pc = "held"

Refused(slot) == IF FullTest THEN slot >= Cap ELSE slot > Cap

Min(a, b) == IF a < b THEN a ELSE b

\* the slot a finish or an abort writes: (frame - data) / stride
Recovered(t) == thread[t].frame \div (IF RecoverStride THEN Stride ELSE WrongStride)

\* write wire_bytes w into info[i], recording an index past the ring
WriteWb(i, w) ==
    /\ info' = IF i \in Slots THEN [info EXCEPT ![i].wb = w] ELSE info
    /\ outOfBounds' = (outOfBounds \/ i \notin Slots \/ i >= Cap)

\* info with wire_bytes 0 in every slot of [lo, hi)
Emptied(inf, lo, hi) ==
    [i \in Slots |-> IF lo <= i /\ i < hi THEN [inf[i] EXCEPT !.wb = 0] ELSE inf[i]]

\* the take returns slot s: info[s] written (wire_bytes 0) and the frame at
\* data + s * stride, the thread now holding it
Take(t, s, inf) ==
    /\ info' = IF s \in Slots THEN [inf EXCEPT ![s] = [wb |-> 0, who |-> t]] ELSE inf
    /\ thread' = [thread EXCEPT ![t] = [IDLE EXCEPT !.pc = "held", !.frame = s * Stride, !.slot = s]]
    /\ outOfBounds' = (outOfBounds \/ s \notin Slots \/ s >= Cap)

\* one step of rocketnet_server_transport_start_packet's portable branch, which
\* is rocketnet_server_payload_ring_take on the calling thread's line, then the
\* info written and the frame at data + slot * stride
Acquire(t) ==
    LET me == thread[t]
        l  == LineOf[t]
    IN
    /\ me.pc # "held"
    /\ CASE me.pc = "idle" ->
              \* a start enters the take
              /\ count < MaxCount
              /\ thread' = [thread EXCEPT ![t].pc = "look"]
              /\ UNCHANGED <<count, line, info, outOfBounds>>
         [] me.pc = "look" ->
              IF line[l].next < line[l].end
              THEN \* the compare-and-swap on the line takes its next slot
                   /\ line' = [line EXCEPT ![l].next = @ + 1]
                   /\ Take(t, line[l].next, info)
                   /\ UNCHANGED count
              ELSE \* the line holds no slot: its word is kept for the install
                   /\ thread' = [thread EXCEPT ![t].pc = "guard", ![t].word = line[l]]
                   /\ UNCHANGED <<count, line, info, outOfBounds>>
         [] me.pc = "guard" ->
              \* used > capacity || capacity - used < size * FILL_LINES: one slot
              /\ thread' = [thread EXCEPT ![t].pc =
                               IF count > Cap \/ Cap - count < Chunk * GuardLines
                               THEN "one" ELSE "claim"]
              /\ UNCHANGED <<count, line, info, outOfBounds>>
         [] me.pc = "claim" ->
              \* the fetch-add of a chunk; its end clipped at send_capacity
              LET base == count
                  end  == Min(count + Chunk, Cap)
              IN /\ count' = count + Chunk
                 /\ IF Refused(base)
                    THEN /\ thread' = [thread EXCEPT ![t] = IDLE]
                         /\ UNCHANGED <<line, info, outOfBounds>>
                    ELSE IF end = base + 1
                    THEN /\ Take(t, base, info)
                         /\ UNCHANGED line
                    ELSE /\ thread' = [thread EXCEPT ![t].pc = "install", ![t].base = base,
                                                     ![t].end = end]
                         /\ UNCHANGED <<line, info, outOfBounds>>
         [] me.pc = "install" ->
              IF line[l] = me.word
              THEN \* the swap installs the chunk's tail and takes its base
                   /\ line' = [line EXCEPT ![l] = [next |-> me.base + 1, end |-> me.end]]
                   /\ Take(t, me.base, info)
                   /\ UNCHANGED count
              ELSE \* the line was refilled: this chunk is marked empty, look again
                   /\ info' = IF LostClear THEN Emptied(info, me.base, me.end) ELSE info
                   /\ thread' = [thread EXCEPT ![t] = [IDLE EXCEPT !.pc = "look"]]
                   /\ outOfBounds' = (outOfBounds \/ (LostClear /\ me.end > Cap))
                   /\ UNCHANGED <<count, line>>
         [] me.pc = "one" ->
              \* the fetch-add of one slot
              /\ count' = count + 1
              /\ IF Refused(count)
                 THEN /\ thread' = [thread EXCEPT ![t] = IDLE]
                      /\ UNCHANGED <<line, info, outOfBounds>>
                 ELSE /\ Take(t, count, info)
                      /\ UNCHANGED line
    /\ UNCHANGED <<finished, badFlush>>

\* rocketnet_server_transport_finish_packet_unchecked's portable branch (the slot from
\* the frame pointer): wire_bytes set
Finish(t) ==
    /\ Held(t)
    /\ WriteWb(Recovered(t), 1)
    /\ thread' = [thread EXCEPT ![t] = IDLE]
    /\ finished' = finished \cup {thread[t].slot}
    /\ UNCHANGED <<count, line, badFlush>>

\* rocketnet_server_transport_abort_packet_unchecked's portable branch: wire_bytes cleared
Abort(t) ==
    /\ Held(t)
    /\ WriteWb(Recovered(t), 0)
    /\ thread' = [thread EXCEPT ![t] = IDLE]
    /\ UNCHANGED <<count, line, finished, badFlush>>

\* rocketnet_server_payload_ring_return_chunks: every line's unused slots marked empty
InChunk(i) == \E l \in Lines : line[l].next <= i /\ i < line[l].end
Returned ==
    IF ReturnClear
    THEN [i \in Slots |-> IF InChunk(i) THEN [info[i] EXCEPT !.wb = 0] ELSE info[i]]
    ELSE info

\* rocketnet_server_transport_flush's portable branch: the lines returned, the
\* counter clamped to send_capacity, every slot below it with wire_bytes > 0
\* sent, the counter reset
Walked == 0..((IF Clamp /\ count > Cap THEN Cap ELSE count) - 1)
Sent == { i \in Walked : i \in Slots /\ Returned[i].wb > 0 }
Flush ==
    /\ \A t \in Threads : thread[t].pc = "idle"
    /\ info' = Returned
    /\ line' = [l \in Lines |-> EMPTY]
    /\ badFlush' = (badFlush \/ Sent # finished)
    /\ outOfBounds' = (outOfBounds \/ \E i \in Walked : i \notin Slots \/ i >= Cap)
    /\ count' = 0
    /\ finished' = {}
    /\ UNCHANGED thread

Next ==
    \/ \E t \in Threads : Acquire(t) \/ Finish(t) \/ Abort(t)
    \/ Flush

Spec == Init /\ [][Next]_vars

----------------------------------------------------------------------------

TypeOK ==
    /\ count \in Nat
    /\ line \in [Lines -> [next : Nat, end : Nat]]
    /\ info \in [Slots -> [wb : {0, 1}, who : Threads]]
    /\ thread \in [Threads -> [pc : PCs, frame : Nat, slot : Nat,
                               word : [next : Nat, end : Nat], base : Nat, end : Nat]]
    /\ finished \subseteq Nat
    /\ outOfBounds \in BOOLEAN /\ badFlush \in BOOLEAN

\* no slot at or past send_capacity, and no info record past the ring, is
\* handed out, written or read
InBounds == ~outOfBounds

\* each flush sends exactly the slots finished since the last one
ExactSend == ~badFlush

\* a held slot is below Cap, its info names its thread, its frame is its root
\* and no two threads hold one slot
HeldExclusive ==
    /\ \A t \in Threads : Held(t) =>
          /\ thread[t].slot < Cap
          /\ thread[t].frame = thread[t].slot * Stride
          /\ info[thread[t].slot].who = t
    /\ \A t, u \in Threads : t # u /\ Held(t) /\ Held(u) => thread[t].slot # thread[u].slot

\* a line's chunk lies below send_capacity and below the counter, and holds no
\* slot a thread holds: every unused chunk slot is one no take has returned
ChunksBelowCap ==
    \A l \in Lines :
        line[l].next < line[l].end =>
            /\ line[l].end <= Cap /\ line[l].end <= count
            /\ \A t \in Threads : Held(t) => ~(line[l].next <= thread[t].slot /\ thread[t].slot < line[l].end)

=============================================================================
