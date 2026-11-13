--------------------------- MODULE RNServerLinkStop ---------------------------
(***************************************************************************)
(* The server's stop with the development link simulator holding packets *)
(* (sdk/rocketnet_server_transport.cpp: rocketnet_server_link_drain at    *)
(* shutdown, called from rocketnet_server_transport_stop_step, and the    *)
(* stop step's link_empty gate on rocketnet_server_transport_io_stop).    *)
(*                                                                         *)
(* At the stop the simulator holds copies in slot order, each bound for   *)
(* the send queue its destination hashes to (send_queue_of).  Each stop   *)
(* step's drain takes them in that order at the shutdown clock (every     *)
(* copy due): a copy whose own queue has room is started and staged; a    *)
(* copy whose queue has no room is held back, and ends the step's drain,  *)
(* while a send staged or in flight could still free room; once the       *)
(* transport is idle (send_idle: nothing staged or in flight but on a     *)
(* queue that has lost its room for good) no copy for that queue can ever *)
(* leave, so it is discarded and counted as SEND_RING_FULL.  The I/O is   *)
(* told to stop only once the simulator is empty, nothing is staged and   *)
(* the transport is idle (RequestIoStop), and STOPPED follows.            *)
(*                                                                         *)
(* The disconnect bursts are ServerStopSpread's; this module is the        *)
(* simulator's copies alone, which the I/O stop also waits on.  One drain *)
(* iteration is one DrainOne step: the drain ending early is DrainOne not *)
(* enabled until another action changes the room or the idle state.      *)
(*                                                                         *)
(* Reversed witnesses, one constant: Defect = "fewest" is the drain before *)
(* this model: it goes on only while the fewest free frames of any queue  *)
(* are above 0 and never discards, so with a queue full through the stop  *)
(* the simulator keeps its copies and the stop never finishes             *)
(* (StopFinishes).  "nodiscard" judges each copy by its own queue but      *)
(* never discards, so a copy for a jammed queue at the front holds the    *)
(* stop forever (StopFinishes).  "earlydiscard" discards a copy whose     *)
(* queue has no room without waiting for the transport to be idle, while  *)
(* a send in flight could still free room (NoDropWhileRoomCanReturn).     *)
(***************************************************************************)
EXTENDS Naturals, Sequences

CONSTANTS
    QueueCount,     \* send queues (send_queues)
    BankSize,       \* frames each send queue stages before frames come back
    Copies,         \* most copies the simulator holds at the stop
    Defect

ASSUME Defect \in {"none", "fewest", "nodiscard", "earlydiscard"}
ASSUME QueueCount \in Nat \ {0} /\ BankSize \in Nat \ {0} /\ Copies \in Nat

Queues == 0..QueueCount - 1
Phases == {"STOPPING", "IOSTOP", "STOPPED"}

VARIABLES
    phase,          \* the server transport's state; IOSTOP: the I/O told to stop
    held,           \* the simulator's copies in slot order, each its queue
    staged,         \* per queue: frames started since the last flush
    inflight,       \* per queue: frames flushed that the transport still holds
    jammed,         \* environment: the queues that have lost their send room for good
    total,          \* ghost: the copies held at the stop
    sent,           \* ghost: copies handed to the staging
    dropped,        \* ghost: per queue, copies discarded and counted SEND_RING_FULL
    early           \* ghost: a copy was discarded while room could still return

vars == <<phase, held, staged, inflight, jammed, total, sent, dropped, early>>

\* send_room_queue (send_room with one queue). A jammed queue reports none.
Room(q) == IF q \in jammed THEN 0 ELSE BankSize - staged[q] - inflight[q]

\* send_room: the fewest free frames of any queue.
MinRoom == IF \E q \in Queues : Room(q) = 0 THEN 0 ELSE 1

\* send_idle: nothing staged or in flight but on a queue the transport has
\* given up on.
Idle == \A q \in Queues : (staged[q] = 0 /\ inflight[q] = 0) \/ q \in jammed

NoneStaged == \A q \in Queues : staged[q] = 0

RECURSIVE SumFrom(_, _)
SumFrom(f, q) == IF q >= QueueCount THEN 0 ELSE f[q] + SumFrom(f, q + 1)

Init ==
    /\ phase = "STOPPING"
    /\ held \in UNION {[1..n -> Queues] : n \in 0..Copies}
    /\ staged = [q \in Queues |-> 0]
    /\ inflight \in [Queues -> 0..BankSize]
    /\ jammed = {}
    /\ total = Len(held)
    /\ sent = 0
    /\ dropped = [q \in Queues |-> 0]
    /\ early = FALSE

\* One iteration of the stop's drain of the simulator: the copy at the front
\* (receive_any at the shutdown clock). "fewest" is the drain before this
\* model: it starts a copy only while every queue has room.
DrainOne ==
    /\ phase = "STOPPING"
    /\ held # << >>
    /\ LET q == Head(held)
       IN \/ /\ Room(q) > 0
             /\ Defect # "fewest" \/ MinRoom > 0
             /\ held' = Tail(held)
             /\ staged' = [staged EXCEPT ![q] = @ + 1]
             /\ sent' = sent + 1
             /\ UNCHANGED <<dropped, early>>
          \/ /\ Room(q) = 0
             /\ Defect \notin {"fewest", "nodiscard"}
             /\ Idle \/ Defect = "earlydiscard"
             /\ held' = Tail(held)
             /\ dropped' = [dropped EXCEPT ![q] = @ + 1]
             /\ early' = (early \/ q \notin jammed)
             /\ UNCHANGED <<staged, sent>>
    /\ UNCHANGED <<phase, inflight, jammed, total>>

\* The step's flush hands every queue's staged frames to the transport.
Flush ==
    /\ ~NoneStaged
    /\ inflight' = [q \in Queues |-> inflight[q] + staged[q]]
    /\ staged' = [q \in Queues |-> 0]
    /\ UNCHANGED <<phase, held, jammed, total, sent, dropped, early>>

\* Environment: a flushed frame of a queue finishes. A jammed queue
\* finishes nothing.
Complete(q) ==
    /\ inflight[q] > 0
    /\ q \notin jammed
    /\ inflight' = [inflight EXCEPT ![q] = @ - 1]
    /\ UNCHANGED <<phase, held, staged, jammed, total, sent, dropped, early>>

\* Environment: a queue loses its send room for good (an XDP queue in hard
\* send error), as in ServerStopSpread.
Jam(q) ==
    /\ phase = "STOPPING"
    /\ q \notin jammed
    /\ jammed' = jammed \cup {q}
    /\ UNCHANGED <<phase, held, staged, inflight, total, sent, dropped, early>>

\* The stop step that finds the simulator empty (link_empty), nothing staged
\* and the transport idle tells the I/O to stop.
RequestIoStop ==
    /\ phase = "STOPPING"
    /\ held = << >>
    /\ NoneStaged
    /\ Idle
    /\ phase' = "IOSTOP"
    /\ UNCHANGED <<held, staged, inflight, jammed, total, sent, dropped, early>>

IoStopped ==
    /\ phase = "IOSTOP"
    /\ phase' = "STOPPED"
    /\ UNCHANGED <<held, staged, inflight, jammed, total, sent, dropped, early>>

Next ==
    \/ DrainOne
    \/ Flush
    \/ \E q \in Queues : Complete(q) \/ Jam(q)
    \/ RequestIoStop
    \/ IoStopped
    \/ phase = "STOPPED" /\ UNCHANGED vars

Spec == Init /\ [][Next]_vars

\* The caller keeps polling rocketnet_server_stopped, and the transport
\* finishes what it was handed (a jammed queue finishes nothing, and the jam
\* itself is never forced).
Fairness ==
    /\ WF_vars(DrainOne)
    /\ WF_vars(Flush)
    /\ \A q \in Queues : WF_vars(Complete(q))
    /\ WF_vars(RequestIoStop)
    /\ WF_vars(IoStopped)
FairSpec == Spec /\ Fairness

TypeOK ==
    /\ phase \in Phases
    /\ held \in UNION {[1..n -> Queues] : n \in 0..Copies}
    /\ staged \in [Queues -> 0..BankSize] /\ inflight \in [Queues -> 0..BankSize]
    /\ jammed \subseteq Queues
    /\ total \in 0..Copies /\ sent \in 0..Copies
    /\ dropped \in [Queues -> 0..Copies]
    /\ early \in BOOLEAN

\* Every copy held at the stop is still held, sent or discarded, once.
CopiesAccounted == Len(held) + sent + SumFrom(dropped, 0) = total

\* No queue's staging ever holds more than its bank.
WithinBank == \A q \in Queues : staged[q] + inflight[q] <= BankSize

\* Only a copy bound for a jammed queue is ever discarded.
OnlyJammedQueuesDrop == \A q \in Queues : dropped[q] > 0 => q \in jammed

\* No copy is discarded while its queue has room or a send could free some.
NoDropWhileRoomCanReturn == ~early

\* The I/O is told to stop only with the simulator empty.
LinkEmptyAtIoStop == phase # "STOPPING" => held = << >>

\* A stop finishes, whatever queue stays full.
StopFinishes == phase = "STOPPING" ~> phase = "STOPPED"
=============================================================================
