--------------------------- MODULE RNClientGroCarry ---------------------------
(* The gso client's GRO receive at the code's layout
   (sdk/rocketnet_client_gso_linux.cpp, rocketnet_client_gso_receive_batch;
   README-RNClientGroCarry.md).

   One call fills at most `room` slots, 0 .. room - 1, in order. The coalesce
   buffer holds one message: its segments in `coalesce`, the next one to
   deliver at index `seg` (carry_segment) of `nseg` (carry_segments). A call
   first delivers the held segments while it has room; the socket is read
   only once none is held, and past the first slot only while a message of
   MaxSeg segments would fit (room - total >= MaxSeg). A message read is held
   from then on, so what does not fit in this call's slots is the next
   call's. Datagrams are the integers 1, 2, ... in arrival order; a message is
   a run of them.

   Each call has one read budget, `room` reads (reads_remaining): every read
   of the socket spends one, a message kept (Read) or a read that delivers
   nothing (Skip: a weather errno, a zero-length, truncated or refused-source
   datagram), as the platform's receive has it (docs/TRANSPORT-INTERNALS.md).

   Defect, a set of defect names, empty in the passing case, selects the
   reversed witnesses:
     "droptail"   a message's segments past the call's room are dropped at
                  the end of the call (the code before this model): Conservation
     "readoverheld" the socket is read while segments are still held, and the
                  new message overwrites them in the coalesce buffer: Conservation
     "pastroom"   the delivery loop runs while total <= room, one slot past
                  the call's room: InRoom
     "skipbudget" a skip does not spend the call's read budget (the code
                  before v1.1.8 kept separate weather and skip counts, up to
                  2 * max_packets + 8 reads in one call): ReadBudget *)
EXTENDS Integers, Sequences
CONSTANTS Cap,      \* the largest room a call is given (max_packets)
          MaxSeg,   \* segments in one message at most (ROCKETNET_CLIENT_GSO_RECEIVE_MAX_SEGMENTS)
          MaxIds,   \* datagrams that arrive in the whole behavior
          Defect
ASSUME Cap \in Nat \ {0} /\ MaxSeg \in Nat \ {0} /\ MaxIds \in Nat
ASSUME Defect \subseteq {"droptail", "readoverheld", "pastroom", "skipbudget"}

VARIABLES socket,    \* the messages in the socket, oldest first, each a sequence of datagrams
          next,      \* the next datagram's number
          coalesce,  \* the held message's segments
          seg,       \* the next held segment to deliver (carry_segment, 0-based)
          nseg,      \* the held message's segment count (carry_segments)
          phase,     \* "idle" between calls, "call" inside one
          room,      \* this call's max_packets
          total,     \* slots this call filled
          reads,     \* reads of the socket this call made (max_packets - reads_remaining)
          slots,     \* [0 .. Cap -> datagram or 0]: the call's slots; slot Cap is past every room
          out        \* the datagrams every finished call returned, in order
vars == <<socket, next, coalesce, seg, nseg, phase, room, total, reads, slots, out>>

Slots == 0 .. Cap
Range(s) == {s[i] : i \in DOMAIN s}
Flatten(q) == LET F[i \in 0 .. Len(q)] == IF i = 0 THEN <<>> ELSE F[i - 1] \o q[i] IN F[Len(q)]
Written == [i \in 1 .. total |-> slots[i - 1]]
Held == SubSeq(coalesce, seg + 1, nseg)

Init ==
    /\ socket = <<>> /\ next = 1
    /\ coalesce = <<>> /\ seg = 0 /\ nseg = 0
    /\ phase = "idle" /\ room = 1 /\ total = 0 /\ reads = 0
    /\ slots = [i \in Slots |-> 0]
    /\ out = <<>>

(* the network: a message of k datagrams lands in the socket *)
Arrive(k) ==
    /\ next + k - 1 <= MaxIds
    /\ socket' = Append(socket, [i \in 1 .. k |-> next + i - 1])
    /\ next' = next + k
    /\ UNCHANGED <<coalesce, seg, nseg, phase, room, total, reads, slots, out>>

(* rocketnet_client_gso_receive_batch is called with max_packets = r *)
Begin(r) ==
    /\ phase = "idle"
    /\ phase' = "call" /\ room' = r /\ total' = 0 /\ reads' = 0
    /\ slots' = [i \in Slots |-> 0]
    /\ UNCHANGED <<socket, next, coalesce, seg, nseg, out>>

Bound == IF "pastroom" \in Defect THEN room + 1 ELSE room

(* rocketnet_client_gso_receive_deliver: one held segment into slot total *)
Deliver ==
    /\ phase = "call"
    /\ seg < nseg /\ total < Bound
    /\ slots' = [slots EXCEPT ![total] = coalesce[seg + 1]]
    /\ seg' = seg + 1 /\ total' = total + 1
    /\ UNCHANGED <<socket, next, coalesce, nseg, phase, room, reads, out>>

(* the reads the receive loop may make: nothing held, room left, a whole
   message fits past the first slot, and the read budget not spent *)
MayRead ==
    /\ phase = "call"
    /\ seg = nseg \/ "readoverheld" \in Defect
    /\ total < room
    /\ total = 0 \/ room - total >= MaxSeg

(* the recvmmsg of one message into the coalesce buffer, which then holds it *)
Read ==
    /\ MayRead
    /\ reads < room
    /\ socket # <<>>
    /\ coalesce' = Head(socket) /\ seg' = 0 /\ nseg' = Len(Head(socket))
    /\ socket' = Tail(socket)
    /\ reads' = reads + 1
    /\ UNCHANGED <<next, phase, room, total, slots, out>>

(* a read that delivers nothing: a weather errno, or a datagram skipped
   (zero-length, truncated, a refused source). It spends one read and no
   datagram moves; the network's chaff is not a datagram of the sequence. *)
Skip ==
    /\ MayRead
    /\ reads < room \/ "skipbudget" \in Defect
    /\ reads' = reads + 1
    /\ UNCHANGED <<socket, next, coalesce, seg, nseg, phase, room, total, slots, out>>

(* the call returns total: no delivery and no read of a message is left to
   it (an empty socket's EAGAIN, a full room or a spent budget) *)
End ==
    /\ phase = "call"
    /\ ~ENABLED Deliver /\ ~ENABLED Read
    /\ phase' = "idle"
    /\ out' = out \o Written
    /\ IF "droptail" \in Defect THEN seg' = nseg ELSE UNCHANGED seg
    /\ UNCHANGED <<socket, next, coalesce, nseg, room, total, reads, slots>>

Done ==
    /\ phase = "idle" /\ next > MaxIds /\ socket = <<>> /\ seg = nseg
    /\ UNCHANGED vars

Next ==
    \/ \E k \in 1 .. MaxSeg : Arrive(k)
    \/ \E r \in 1 .. Cap : Begin(r)
    \/ Deliver \/ Read \/ Skip \/ End
    \/ Done

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ next \in 1 .. MaxIds + 1
    /\ seg \in 0 .. MaxSeg /\ nseg \in 0 .. MaxSeg /\ seg <= nseg /\ Len(coalesce) = nseg
    /\ phase \in {"idle", "call"} /\ room \in 1 .. Cap /\ total \in 0 .. Cap + 1
    /\ reads \in Nat
    /\ slots \in [Slots -> 0 .. MaxIds]

(* Every datagram that arrived is returned, written in this call, held, or
   still in the socket, each once and in arrival order. *)
Conservation ==
    out \o (IF phase = "call" THEN Written ELSE <<>>) \o Held \o Flatten(socket) = [i \in 1 .. next - 1 |-> i]

(* A call writes only the slots below its room. *)
InRoom == phase = "call" => total <= room

(* A call reads the socket at most max_packets times: one read budget. *)
ReadBudget == phase = "call" => reads <= room
=============================================================================
