------------------------ MODULE RNClientIoCompactLayout ------------------------
(***************************************************************************)
(* The client I/O thread's batch receive at its layout                     *)
(* (sdk/rocketnet_client_io.h, rocketnet_client_io_step, the receive_batch *)
(* branch, with rocketnet_client_io_fill_record).  RNClientIoDecrypt       *)
(* checks the records as a sequence; this model checks how a batch is laid *)
(* into the fill buffer's frame slots, which it cannot see.                *)
(*                                                                         *)
(* The fill buffer has C frame slots and fill_slots of them hold records.  *)
(* One receive_batch call writes up to min( C - fill_slots, B ) datagrams  *)
(* into consecutive slots from fill_slots.  The loop then walks the batch: *)
(* a datagram whose byte count is a transport fault is skipped; any other  *)
(* is moved down from slot fill_slots + i to slot fill_slots + accepted    *)
(* (when they differ) and becomes the next record, which names slot        *)
(* fill_slots + accepted.  fill_slots then advances by accepted.  A        *)
(* publish hands the buffer on and the I/O thread fills another, whose     *)
(* slots hold whatever they held.                                          *)
(*                                                                         *)
(* The checked claims:                                                     *)
(*   RecordsExact  every record's slot is inside the buffer and holds the  *)
(*                 datagram the record was made for: no record names a     *)
(*                 slot a later move overwrote, or a slot that was not     *)
(*                 moved into.                                             *)
(*   RecordsDense  record k names slot k - 1, so fill_slots counts them.   *)
(*                                                                         *)
(* Rule switches, each changed in one reversed witness:                    *)
(*   Move       the memmove from slot i to slot accepted; FALSE drops it.  *)
(*   RecordAt   "accepted" is the code's record slot; "source" names slot  *)
(*              fill_slots + i, where the datagram arrived.                *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS C,        \* ROCKETNET_NUM_CLIENT_PACKETS: frame slots per buffer
          B,        \* ROCKETNET_CLIENT_IO_BATCH: datagrams per receive_batch
          MaxIds,   \* datagrams the harness delivers in all
          Move, RecordAt

ASSUME C >= 1 /\ B >= 1 /\ MaxIds >= 1
ASSUME Move \in BOOLEAN /\ RecordAt \in {"accepted", "source"}

SlotIds == 0..C - 1
Ids == 1..MaxIds

VARIABLES slot,      \* buffer->data: the datagram id each frame slot holds (0 none)
          fill,      \* io->fill_slots
          recs,      \* the records: <<id, slot>> per buffer->packet_data[k]
          pc,        \* "idle" or "loop"
          i,         \* the loop index
          accepted,  \* accepted
          cnt,       \* count: datagrams the call returned
          batch,     \* ghost: batch[k] the id received at offset k
          faulty,    \* ghost: offsets whose byte count is a transport fault
          nextId     \* ghost: the next datagram id

vars == <<slot, fill, recs, pc, i, accepted, cnt, batch, faulty, nextId>>

Init ==
    /\ slot = [s \in SlotIds |-> 0]
    /\ fill = 0
    /\ recs = <<>>
    /\ pc = "idle"
    /\ i = 0
    /\ accepted = 0
    /\ cnt = 0
    /\ batch = <<>>
    /\ faulty = {}
    /\ nextId = 1

\* receive_batch: got datagrams land in slots fill .. fill + got - 1, some of
\* them with a faulty byte count.
Receive ==
    /\ pc = "idle"
    /\ fill < C
    /\ LET room == C - fill
           max == IF room < B THEN room ELSE B
       IN \E got \in 1..max, bad \in SUBSET (0..max - 1) :
            /\ nextId + got - 1 <= MaxIds
            /\ bad \subseteq 0..got - 1
            /\ slot' = [s \in SlotIds |->
                          IF s >= fill /\ s < fill + got THEN nextId + (s - fill) ELSE slot[s]]
            /\ batch' = [k \in 0..got - 1 |-> nextId + k]
            /\ faulty' = bad
            /\ cnt' = got
            /\ nextId' = nextId + got
    /\ i' = 0
    /\ accepted' = 0
    /\ pc' = "loop"
    /\ UNCHANGED <<fill, recs>>

\* one pass of the loop: a fault is skipped, any other datagram is moved down
\* and recorded
Step ==
    /\ pc = "loop"
    /\ i < cnt
    /\ IF i \in faulty
         THEN UNCHANGED <<slot, recs, accepted>>
         ELSE /\ slot' = IF Move /\ accepted # i
                           THEN [slot EXCEPT ![fill + accepted] = slot[fill + i]]
                           ELSE slot
              /\ recs' = Append(recs, <<batch[i],
                                        IF RecordAt = "accepted" THEN fill + accepted
                                        ELSE fill + i>>)
              /\ accepted' = accepted + 1
    /\ i' = i + 1
    /\ UNCHANGED <<fill, pc, cnt, batch, faulty, nextId>>

\* the loop's end: fill_slots += accepted (nothing when 0, the break)
EndBatch ==
    /\ pc = "loop"
    /\ i = cnt
    /\ fill' = fill + accepted
    /\ pc' = "idle"
    /\ UNCHANGED <<slot, recs, i, accepted, cnt, batch, faulty, nextId>>

\* the buffer is published and a fresh one taken: no records, its slots
\* holding whatever they held
Publish ==
    /\ pc = "idle"
    /\ fill > 0
    /\ fill' = 0
    /\ recs' = <<>>
    /\ UNCHANGED <<slot, pc, i, accepted, cnt, batch, faulty, nextId>>

Next == Receive \/ Step \/ EndBatch \/ Publish

Spec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------

TypeOK ==
    /\ slot \in [SlotIds -> 0..MaxIds]
    /\ fill \in 0..C
    /\ pc \in {"idle", "loop"}
    /\ accepted \in 0..B
    /\ i \in 0..B

RecordsExact ==
    \A k \in 1..Len(recs) :
        /\ recs[k][2] \in SlotIds
        /\ slot[recs[k][2]] = recs[k][1]

RecordsDense == \A k \in 1..Len(recs) : recs[k][2] = k - 1

=============================================================================
