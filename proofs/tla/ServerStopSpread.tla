--------------------------- MODULE ServerStopSpread ---------------------------
(***************************************************************************)
(* The server's disconnect bursts across updates and across the stop, with *)
(* the ring's own index arithmetic and the transport's send staging        *)
(* (sdk/rocketnet_server_transport.cpp: disconnect_bursts, the defer,      *)
(* rocketnet_server_transport_drain_disconnect_bursts, the stop step and   *)
(* rocketnet_server_transport_send_capacity_available; sdk/rocketnet_api.cpp *)
(* rocketnet_server_stop).                                                 *)
(*                                                                         *)
(* An ended connection's burst of Burst disconnect packets waits in a      *)
(* ring of RingSize entries: an array, a head, a count and a cursor, every *)
(* index taken modulo RingSize as the code takes it.  A running server     *)
(* defers a kicked client's burst while fewer than SteadyCap wait          *)
(* (disconnect_burst_steady_cap, max_clients) and drops the new burst's    *)
(* copies otherwise.  The code's ring is rocketnet_server_disconnect_bursts *)
(* (max_clients) = 2 * max_clients entries.                                *)
(* Stop appends every connected client's burst, whatever SteadyCap, so the *)
(* ring is RingSize = 2 * SteadyCap entries: SteadyCap waiting and one per *)
(* slot.                                                                   *)
(*                                                                         *)
(* The drain seals one packet per waiting burst per pass from the cursor,  *)
(* and calls a staged start only while the transport reports room: Room is *)
(* send_capacity_available, the staging less what is staged and what the  *)
(* transport still holds from earlier flushes (BankSize frames).  A        *)
(* completed burst at the head leaves the ring, and the cursor moves back  *)
(* with it.  An entry already complete away from the head (one abandoned   *)
(* for an unreachable destination) is passed over.  A start the transport  *)
(* refuses for want of a link-layer destination abandons that burst: no    *)
(* packet of it can ever leave.  A sealed write that fails after its start *)
(* (rocketnet_write_sealed_packet returning no bytes; the drain aborts the *)
(* frame, abandons the burst and keeps draining) is taken as the same      *)
(* abandon: LoseRoute(s) followed by SealOne's abandon branch, which needs *)
(* the same room and marks the burst complete.  One difference is          *)
(* abstracted away: on a staged transport the start already claimed a slot *)
(* of the queue's bank, and rocketnet_server_abort_send_frame only zeroes  *)
(* its byte count, so that slot stays claimed until the step's flush hands *)
(* the bank off; the model's abandon claims none.  The claim only takes    *)
(* room (a later burst on that queue in the same pass may wait for the     *)
(* next flush instead of sealing now); it carries nothing and every flush  *)
(* frees it.  LoseRoute changes nothing but the environment set            *)
(* unreachable, which names only that now complete burst.                  *)
(*                                                                         *)
(* The transport stages into QueueCount send queues (send_queues), each    *)
(* with its own bank of BankSize frames; a session's packets go to the     *)
(* queue QueueOf(s) (send_queue_of, a function of the address).  One queue *)
(* is the model before queues.  With several, a burst whose own queue has  *)
(* no room while another queue has some is passed over (PassFull), so the  *)
(* bursts behind it for queues with room are still sealed.                *)
(*                                                                         *)
(* Stop is a state machine across updates: Stop moves RUNNING to STOPPING  *)
(* and appends; each stop step (a poll of rocketnet_server_stopped, or a   *)
(* transport update) is the drain's seals within Room and a Flush; once    *)
(* the ring is empty and the last seal is flushed the transport's I/O is   *)
(* told to stop (RequestIoStop), and STOPPED follows once its workers have *)
(* exited (IoStopped).  Complete is the environment: the kernel or worker  *)
(* finishing a flushed frame, which returns its staging.                   *)
(*                                                                         *)
(* A queue can lose its send room for good (Jam: an XDP queue in hard      *)
(* send error with a full producer): it reports no room, never finishes    *)
(* what it holds, and the transport's send_idle gives up on it.  No        *)
(* waiting packet for that queue can then leave, so a stop step that finds *)
(* bursts waiting, the transport idle and a waiting burst's own queue with *)
(* no room abandons each such burst, each one counted once as              *)
(* SEND_RING_FULL (AbandonNoRoom).  The room is read again after the idle  *)
(* check, which may itself free room (the portable transport's send_idle   *)
(* publishes its producer, and the worker may finish it before the check  *)
(* reads idle).  With one queue this is every waiting burst, once the      *)
(* transport has no room.                                                  *)
(*                                                                         *)
(* Simulator = TRUE is a development build's active link simulator         *)
(* (rocketnet_server_commit_staged): a sealed packet goes to the simulator *)
(* (held grows) and io_abort_packet zeroes its slot, which stays claimed   *)
(* in the queue's bank (staged grows) until the flush hands the bank off   *)
(* and the worker recycles it, as on every staged driver.                  *)
(* The link drain (rocketnet_server_link_drain, called by the stop step    *)
(* after its flush) moves held copies into the staging only while          *)
(* rocketnet_server_transport_io_send_room is above 0, and that is the     *)
(* FEWEST room over the send queues (FewestRoom).  The stop step tells the *)
(* I/O to stop only when the ring is empty, the simulator holds nothing    *)
(* (link_empty) and send_idle is true, so RequestIoStop needs held = 0.    *)
(* OPEN CODE DEFECT (MM080, owned by the send-queue-capacity follow-up):   *)
(* no step of the code discards what the simulator holds, so once any      *)
(* queue jams (FewestRoom is 0 for good) while copies are held, the drain  *)
(* never runs again, link_empty stays false and the stop never finishes;   *)
(* destroy's bounded wait gives up and keeps the server, unfreed.  The     *)
(* model states the code as it is:                                         *)
(* MCServerStopSpreadSimulatorJam.cfg fails StopFinishesWithoutRoom for    *)
(* that reason and is recorded as the open defect, not as a reversed       *)
(* witness.  When the code gains a terminal discard of held copies, the    *)
(* model gains it as an action and that case passes.                       *)
(*                                                                         *)
(* Reversed witnesses, one constant: Defect = "singleshot" is the stop     *)
(* before this model: every burst staged in the stop call whatever the     *)
(* room, a refused start dropping that packet, and the I/O told to stop    *)
(* at once (FullBurstBeforeStopped).  "steadycapoffbyone" lets a running   *)
(* server defer one burst past SteadyCap, so the stop's appends overrun    *)
(* the ring (RingHolds).  "stopinflight" tells the I/O to stop while a     *)
(* flushed send is unfinished (NoSendCutAtStop).  "noskip" seals an entry  *)
(* the drain should pass over (SealedWithinBurst).  "unreachablestalls"    *)
(* ends the drain at an unreachable start as at a full one, so the stop    *)
(* never finishes (StopFinishes).  "noabandon" is the stop step before     *)
(* AbandonNoRoom: a jammed transport's waiting bursts wait forever         *)
(* (StopFinishesWithoutRoom).  "noreread" abandons without reading the     *)
(* room again after the idle check (NoDropWhileRoomCanReturn).             *)
(* "pooledroom" is the code before queues were judged one by one: the      *)
(* drain ends at a burst whose own queue is full, and the stop step        *)
(* abandons only once the room summed over every queue is 0, so with one   *)
(* queue jammed and another with room the stop never finishes              *)
(* (StopFinishesWithoutRoom).  TLC's trace fails on the pooled abandon     *)
(* alone (one burst, on the jammed queue); no property here needs the      *)
(* drain's pass over a full queue, so PassFull has no witness of its own   *)
(* (a roadmap entry in notes-for-R).  "stopwithheld" tells the I/O to stop *)
(* while the simulator still holds copies, the step without its link_empty *)
(* test (NoCopyHeldAtStop).  "nolinkdrain" is a transport that never       *)
(* drains the simulator, in the update or the stop step, so with no queue  *)
(* jammed the held copies never leave (StopFinishesWhileUnjammed).  A ring of SteadyCap entries, the size     *)
(* before this model, is the configuration witness                         *)
(* MCServerStopSpreadBrokenRingSmall (RingHolds).                          *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    SlotCount,      \* client slots (max_clients)
    MaxSessions,    \* exploration bound: connections over the run
    Burst,          \* packets per burst (ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS)
    BankSize,       \* frames each send queue stages before frames come back
    QueueCount,     \* send queues (send_queues)
    RingSize,       \* ring entries (rocketnet_server_disconnect_bursts( max_clients ), 2 * max_clients)
    SteadyCap,      \* bursts a running server keeps waiting (disconnect_burst_steady_cap, max_clients)
    Simulator,      \* a development build's active link simulator takes every sealed packet
    Defect

ASSUME Defect \in {"none", "singleshot", "steadycapoffbyone", "noskip", "unreachablestalls", "stopinflight",
                   "noabandon", "noreread", "pooledroom", "stopwithheld", "nolinkdrain"}
ASSUME Simulator \in BOOLEAN
ASSUME SlotCount \in Nat \ {0} /\ MaxSessions \in Nat \ {0} /\ Burst \in Nat \ {0}
ASSUME BankSize \in Nat \ {0} /\ RingSize \in Nat \ {0} /\ SteadyCap \in Nat \ {0}
ASSUME QueueCount \in Nat \ {0}

Slots == 1..SlotCount
Sessions == 1..MaxSessions
Queues == 0..QueueCount - 1
Idx == 0..RingSize - 1
NoEntry == [s |-> 0, sealed |-> 0]
Phases == {"RUNNING", "STOPPING", "IOSTOP", "STOPPED"}

\* send_queue_of: the queue a session's packets are staged on.
QueueOf(s) == s % QueueCount

VARIABLES
    phase,          \* the server transport's state; IOSTOP: the I/O told to stop
    slot,           \* the session each slot holds, 0 when free
    nextSess,       \* the next session tag
    ring,           \* disconnect_bursts: the session and its sealed count
    head,           \* disconnect_burst_head
    count,          \* disconnect_burst_count
    cursor,         \* disconnect_burst_cursor
    staged,         \* per queue: frames started and finished since the last flush
    inflight,       \* per queue: frames flushed that the transport still holds
    sent,           \* ghost: each session's packets handed to the staging
    owed,           \* ghost: sessions whose burst was kept to be sent
    unreachable,    \* environment: sessions with no link-layer destination
    abandoned,      \* ghost: bursts given up at an unreachable start
    refused,        \* ghost: a start was called with no room
    overflow,       \* ghost: an append found the ring full
    cut,            \* ghost: the I/O was told to stop with a flushed send unfinished
    jammed,         \* environment: the queues that have lost their send room for good
    dropped,        \* ghost: bursts abandoned at a stop step for want of send room
    ringFull,       \* ghost: the SEND_RING_FULL counts those abandons added
    early,          \* ghost: a burst was abandoned while room could still return
    held            \* the sealed copies the link simulator holds (0 without one)

vars == <<phase, slot, nextSess, ring, head, count, cursor, staged, inflight,
          sent, owed, unreachable, abandoned, refused, overflow, cut,
          jammed, dropped, ringFull, early, held>>

\* send_room_queue (send_room with one queue). A jammed queue reports none,
\* whatever it holds.
Room(q) == IF q \in jammed THEN 0 ELSE BankSize - staged[q] - inflight[q]

\* io_send_room_total: the room summed over the queues, f's values summed
\* from queue q up (a walk by index, not CHOOSE over a set: TLC evaluates it
\* in most states).
RECURSIVE SumFrom(_, _)
SumFrom(f, q) == IF q >= QueueCount THEN 0 ELSE f[q] + SumFrom(f, q + 1)
RoomTotal == IF QueueCount = 1 THEN Room(0) ELSE SumFrom([q \in Queues |-> Room(q)], 0)

\* send_idle: every flushed frame finished, or the transport has given up on
\* what a queue holds (an XDP queue in hard send error).
Idle == \A q \in Queues : inflight[q] = 0 \/ q \in jammed

NoneStaged == \A q \in Queues : staged[q] = 0

\* The variables only the jam and the abandon for want of room change.
jamVars == <<jammed, dropped, ringFull, early>>

\* rocketnet_server_transport_io_send_room: the fewest room over the send
\* queues (with one queue, its room). The link drain reads it.
RECURSIVE MinFrom(_, _)
MinFrom(f, q) == IF q = QueueCount - 1 THEN f[q]
                 ELSE LET rest == MinFrom(f, q + 1) IN IF f[q] < rest THEN f[q] ELSE rest
FewestRoom == MinFrom([q \in Queues |-> Room(q)], 0)

LiveIdx == {(head + k) % RingSize : k \in 0..count - 1}

Init ==
    /\ phase = "RUNNING"
    /\ slot = [i \in Slots |-> 0]
    /\ nextSess = 1
    /\ ring = [i \in Idx |-> NoEntry]
    /\ head = 0 /\ count = 0 /\ cursor = 0
    /\ staged = [q \in Queues |-> 0] /\ inflight = [q \in Queues |-> 0]
    /\ sent = [s \in Sessions |-> 0]
    /\ owed = {} /\ unreachable = {} /\ abandoned = {}
    /\ refused = FALSE /\ overflow = FALSE /\ cut = FALSE
    /\ jammed = {} /\ dropped = {} /\ ringFull = 0 /\ early = FALSE
    /\ held = 0

\* A client connects into a free slot.
Connect(i) ==
    /\ phase = "RUNNING"
    /\ slot[i] = 0
    /\ nextSess <= MaxSessions
    /\ slot' = [slot EXCEPT ![i] = nextSess]
    /\ nextSess' = nextSess + 1
    /\ UNCHANGED <<phase, ring, head, count, cursor, staged, inflight, sent,
                   owed, unreachable, abandoned, refused, overflow, cut, held>>
    /\ UNCHANGED jamVars

\* rocketnet_server_transport_disconnect_client on a running server: the slot
\* ends now; its burst is appended at (head + count) % RingSize while fewer
\* than SteadyCap wait, else its copies are dropped and it is not owed. The
\* immediate seal of a burst the allowance and the room hold whole is this
\* append followed by its seals in the same update.
SteadyHasRoom ==
    IF Defect = "steadycapoffbyone" THEN count <= SteadyCap ELSE count < SteadyCap
Kick(i) ==
    /\ phase = "RUNNING"
    /\ slot[i] # 0
    /\ slot' = [slot EXCEPT ![i] = 0]
    /\ IF SteadyHasRoom
          THEN /\ ring' = [ring EXCEPT ![(head + count) % RingSize] = [s |-> slot[i], sealed |-> 0]]
               /\ count' = count + 1
               /\ owed' = owed \cup {slot[i]}
               /\ overflow' = (overflow \/ count + 1 > RingSize)
          ELSE UNCHANGED <<ring, count, owed, overflow>>
    /\ UNCHANGED <<phase, nextSess, head, cursor, staged, inflight, sent,
                   unreachable, abandoned, refused, cut, held>>
    /\ UNCHANGED jamVars

\* rocketnet_server_stop: every connected slot, in slot order, ends and its
\* burst is appended (rocketnet_server_transport_disconnect_client_shutdown);
\* then the transport is STOPPING. An append past RingSize lands on a live
\* entry, which the ghost overflow records.
RECURSIVE AppendSlots(_, _, _)
AppendSlots(r, c, i) ==
    IF i > SlotCount THEN [ring |-> r, count |-> c]
    ELSE IF slot[i] = 0 THEN AppendSlots(r, c, i + 1)
    ELSE AppendSlots([r EXCEPT ![(head + c) % RingSize] = [s |-> slot[i], sealed |-> 0]], c + 1, i + 1)
Connected == {slot[i] : i \in {j \in Slots : slot[j] # 0}}
Stop ==
    /\ phase = "RUNNING"
    /\ LET appended == AppendSlots(ring, count, 1)
       IN /\ ring' = appended.ring
          /\ count' = appended.count
          /\ overflow' = (overflow \/ appended.count > RingSize)
    /\ owed' = owed \cup Connected
    /\ slot' = [i \in Slots |-> 0]
    /\ phase' = "STOPPING"
    /\ UNCHANGED <<nextSess, head, cursor, staged, inflight, sent, unreachable,
                   abandoned, refused, cut, held>>
    /\ UNCHANGED jamVars

\* The head leaves while its burst is complete; the cursor is an offset from
\* the head, so it moves back with it. The entry is cleared as its key is
\* wiped.
RECURSIVE Pop(_, _, _, _)
Pop(r, h, n, cur) ==
    IF n > 0 /\ r[h].sealed = Burst
    THEN Pop([r EXCEPT ![h] = NoEntry], (h + 1) % RingSize, n - 1, IF cur > 0 THEN cur - 1 ELSE 0)
    ELSE [ring |-> r, head |-> h, count |-> n, cursor |-> cur]

PopAfter(r, cur) ==
    LET p == Pop(r, head, count, cur)
    IN /\ ring' = p.ring /\ head' = p.head /\ count' = p.count /\ cursor' = p.cursor

\* One pass position of the drain: the burst at the cursor (wrapped to 0 at
\* count). An entry already complete is passed over. A burst whose own queue
\* has no room while another queue has some is passed over (PassFull);
\* "pooledroom" has no such step, so the drain ends there. An unreachable
\* start abandons the burst (and so does a sealed write that fails: see the
\* module comment). Otherwise a packet is sealed only while its queue
\* reports room; with no room anywhere the drain ends this step and the
\* burst waits. With the link simulator the sealed packet goes to it
\* (rocketnet_server_commit_staged): held grows, and the slot io_abort_packet
\* zeroed stays claimed on queue q until the flush, so staged grows too.
DrainAt == IF cursor >= count THEN 0 ELSE cursor
SealOne ==
    /\ phase \in {"RUNNING", "STOPPING"}
    /\ count > 0
    /\ LET c == DrainAt
           idx == (head + c) % RingSize
           e == ring[idx]
           q == QueueOf(e.s)
       IN \/ /\ e.sealed = Burst /\ Defect # "noskip"
             /\ cursor' = c + 1
             /\ UNCHANGED <<ring, head, count, staged, sent, abandoned, refused, held>>
          \/ /\ e.sealed < Burst
             /\ Defect # "pooledroom"
             /\ Room(q) = 0 /\ RoomTotal > 0
             /\ cursor' = c + 1
             /\ UNCHANGED <<ring, head, count, staged, sent, abandoned, refused, held>>
          \/ /\ e.sealed < Burst \/ Defect = "noskip"
             /\ e.s \in unreachable /\ Defect # "unreachablestalls"
             /\ (Room(q) > 0 \/ Defect = "singleshot")
             /\ PopAfter([ring EXCEPT ![idx].sealed = Burst], c + 1)
             /\ abandoned' = abandoned \cup {e.s}
             /\ UNCHANGED <<staged, sent, refused, held>>
          \/ /\ e.sealed < Burst \/ Defect = "noskip"
             /\ e.s \notin unreachable
             /\ \/ /\ Room(q) > 0
                   /\ staged' = [staged EXCEPT ![q] = @ + 1]
                   /\ held' = IF Simulator THEN held + 1 ELSE held
                   /\ sent' = [sent EXCEPT ![e.s] = @ + 1]
                   /\ UNCHANGED refused
                \/ /\ Room(q) = 0 /\ Defect = "singleshot"
                   /\ refused' = TRUE
                   /\ UNCHANGED <<staged, sent, held>>
             /\ PopAfter([ring EXCEPT ![idx].sealed = @ + 1], c + 1)
             /\ UNCHANGED abandoned
    /\ UNCHANGED <<phase, slot, nextSess, inflight, owed, unreachable, overflow, cut>>
    /\ UNCHANGED jamVars

\* The step's flush hands every queue's staged frames to the transport. The
\* stop step and the update follow it with the link drain
\* (rocketnet_server_link_drain): while the fewest room over the queues is
\* above 0, a held copy is started on its destination's queue (any queue
\* here: the guard does not depend on the destination, and with the fewest
\* room above 0 every queue has room). With any queue at no room the drain
\* moves nothing, whatever room the copy's own queue has. "nolinkdrain"
\* leaves the drain out.
Flush ==
    /\ \/ /\ ~NoneStaged
          /\ inflight' = [q \in Queues |-> inflight[q] + staged[q]]
          /\ staged' = [q \in Queues |-> 0]
          /\ UNCHANGED held
       \/ /\ phase \in {"RUNNING", "STOPPING"}
          /\ Defect # "nolinkdrain"
          /\ held > 0
          /\ FewestRoom > 0
          /\ \E q \in Queues : staged' = [staged EXCEPT ![q] = @ + 1]
          /\ held' = held - 1
          /\ UNCHANGED inflight
    /\ UNCHANGED <<phase, slot, nextSess, ring, head, count, cursor, sent, owed,
                   unreachable, abandoned, refused, overflow, cut>>
    /\ UNCHANGED jamVars

\* Environment: the kernel or the transport's worker finishes a flushed
\* frame of a queue, and its staging comes back. A jammed queue finishes
\* nothing.
Complete(q) ==
    /\ inflight[q] > 0
    /\ q \notin jammed
    /\ inflight' = [inflight EXCEPT ![q] = @ - 1]
    /\ UNCHANGED <<phase, slot, nextSess, ring, head, count, cursor, staged, sent,
                   owed, unreachable, abandoned, refused, overflow, cut, held>>
    /\ UNCHANGED jamVars

\* Environment: a queue loses its send room for good (an XDP queue whose TX
\* wakeup failed hard, its producer full): from here it reports no room,
\* finishes nothing it holds, and send_idle gives up on it. The hard error
\* can clear on a later wakeup; the stop takes send_idle's giving up as
\* final, as the code does, so the model keeps the jam.
Jam(q) ==
    /\ phase \in {"RUNNING", "STOPPING"}
    /\ q \notin jammed
    /\ jammed' = jammed \cup {q}
    /\ UNCHANGED <<phase, slot, nextSess, ring, head, count, cursor, staged, inflight,
                   sent, owed, unreachable, abandoned, refused, overflow, cut,
                   dropped, ringFull, early, held>>

\* Environment: a waiting burst's destination has no link-layer address.
LoseRoute(s) ==
    /\ s \in owed /\ s \notin unreachable
    /\ unreachable' = unreachable \cup {s}
    /\ UNCHANGED <<phase, slot, nextSess, ring, head, count, cursor, staged,
                   inflight, sent, owed, abandoned, refused, overflow, cut, held>>
    /\ UNCHANGED jamVars

\* Environment: the destination is learned again (a reply MAC refreshed).
\* A burst already abandoned stays abandoned: its entry is complete and the
\* drain passes over it, so no packet past its burst is ever sealed.
FindRoute(s) ==
    /\ s \in unreachable
    /\ unreachable' = unreachable \ {s}
    /\ UNCHANGED <<phase, slot, nextSess, ring, head, count, cursor, staged,
                   inflight, sent, owed, abandoned, refused, overflow, cut, held>>
    /\ UNCHANGED jamVars

\* The stop step after its flush that finds a waiting burst whose own queue
\* has no room (the first read; with one queue, the transport's room), the
\* transport's send_idle true, and that queue still without room (the second
\* read): no packet for that queue can ever leave, so each waiting burst
\* bound for a queue without room is abandoned, its key wiped, marked
\* complete and popped once it reaches the head, and counted once as
\* SEND_RING_FULL. With one queue that is every waiting burst, the ring
\* emptied and the cursor back to 0, as before queues. The reads are not one
\* atomic look: a queue may finish some of its flushed frames before
\* send_idle reads idle (the portable transport's send_idle publishes its
\* producer, and its worker may send the bank before the check), which frees
\* room the second read sees. "noabandon" is the stop step before this
\* action; "noreread" leaves out the second read; "pooledroom" judges the
\* room summed over every queue, abandoning only once that is 0.
AbandonNoRoom ==
    /\ phase = "STOPPING"
    /\ Defect # "noabandon"
    /\ NoneStaged
    /\ count > 0
    /\ IF Defect = "pooledroom"
          THEN RoomTotal = 0
          ELSE \E i \in LiveIdx : ring[i].sealed < Burst /\ Room(QueueOf(ring[i].s)) = 0
    \* send_idle true: every queue not jammed has finished what it held, so
    \* what is left in flight is a jammed queue's (one value, no choice)
    /\ LET left == [q \in Queues |-> IF q \in jammed THEN inflight[q] ELSE 0]
           roomAfter(q) == IF q \in jammed THEN 0 ELSE BankSize - left[q]
           judged(q) == IF Defect = "noreread" THEN Room(q) ELSE roomAfter(q)
           totalAfter == IF Defect = "noreread" THEN RoomTotal
                         ELSE SumFrom([x \in Queues |-> roomAfter(x)], 0)
           waiting == {i \in LiveIdx : ring[i].sealed < Burst /\
                          IF Defect = "pooledroom" THEN totalAfter = 0 ELSE judged(QueueOf(ring[i].s)) = 0}
       IN /\ waiting # {}
          /\ inflight' = left
          /\ PopAfter([i \in Idx |-> IF i \in waiting THEN [ring[i] EXCEPT !.sealed = Burst] ELSE ring[i]], cursor)
          /\ dropped' = dropped \cup {ring[i].s : i \in waiting}
          /\ ringFull' = ringFull + Cardinality(waiting)
          /\ early' = (early \/ \E i \in waiting : roomAfter(QueueOf(ring[i].s)) > 0)
    /\ UNCHANGED <<phase, slot, nextSess, staged, sent, owed, unreachable, abandoned,
                   refused, overflow, cut, jammed, held>>

\* The stop step that finds the ring empty, its seals flushed and every
\* flushed send finished (the transport's send_idle) tells the I/O to stop
\* (rocketnet_server_transport_io_stop). The single-shot stop told it at
\* once; "stopinflight" tells it with a flushed send unfinished, which the
\* stopping I/O cuts off (the IOCP and RIO send workers retire a bank they
\* find stopping). What a jammed queue holds can never leave, so the stop
\* cuts none of it off. The step also needs the link simulator empty
\* (link_empty): send_idle gives up on a jammed queue, link_empty gives up
\* on nothing, so a held copy keeps the I/O running ("stopwithheld" leaves
\* that test out).
RequestIoStop ==
    /\ phase = "STOPPING"
    /\ NoneStaged
    /\ count = 0 \/ Defect = "singleshot"
    /\ held = 0 \/ Defect \in {"singleshot", "stopwithheld"}
    /\ Idle \/ Defect \in {"singleshot", "stopinflight"}
    /\ phase' = "IOSTOP"
    /\ cut' = (cut \/ \E q \in Queues : inflight[q] > 0 /\ q \notin jammed)
    /\ UNCHANGED <<slot, nextSess, ring, head, count, cursor, staged, inflight,
                   sent, owed, unreachable, abandoned, refused, overflow, held>>
    /\ UNCHANGED jamVars

\* The workers have exited: rocketnet_server_stopped reports true.
IoStopped ==
    /\ phase = "IOSTOP"
    /\ phase' = "STOPPED"
    /\ UNCHANGED <<slot, nextSess, ring, head, count, cursor, staged, inflight,
                   sent, owed, unreachable, abandoned, refused, overflow, cut, held>>
    /\ UNCHANGED jamVars

\* STOPPED is the end of the run, a stutter here (a later start builds a new
\* transport and a new ring).
Next ==
    \/ \E i \in Slots : Connect(i) \/ Kick(i)
    \/ Stop
    \/ SealOne
    \/ Flush
    \/ \E q \in Queues : Complete(q) \/ Jam(q)
    \/ \E s \in Sessions : LoseRoute(s) \/ FindRoute(s)
    \/ AbandonNoRoom
    \/ RequestIoStop
    \/ IoStopped
    \/ phase = "STOPPED" /\ UNCHANGED vars

Spec == Init /\ [][Next]_vars

\* The caller keeps polling rocketnet_server_stopped (or updating the
\* transport), and the transport finishes what it was handed (a jammed queue
\* finishes nothing, and the jam itself is never forced).
Fairness ==
    /\ WF_vars(SealOne)
    /\ WF_vars(Flush)
    /\ \A q \in Queues : WF_vars(Complete(q))
    /\ WF_vars(AbandonNoRoom)
    /\ WF_vars(RequestIoStop)
    /\ WF_vars(IoStopped)
FairSpec == Spec /\ Fairness

TypeOK ==
    /\ phase \in Phases
    /\ slot \in [Slots -> 0..MaxSessions]
    /\ nextSess \in 1..MaxSessions + 1
    /\ ring \in [Idx -> [s : 0..MaxSessions, sealed : 0..Burst + 1]]
    /\ head \in Idx /\ count \in 0..RingSize + SlotCount /\ cursor \in 0..RingSize + SlotCount
    /\ staged \in [Queues -> 0..BankSize] /\ inflight \in [Queues -> 0..BankSize]
    /\ sent \in [Sessions -> 0..Burst]
    /\ owed \subseteq Sessions /\ unreachable \subseteq Sessions /\ abandoned \subseteq Sessions
    /\ refused \in BOOLEAN /\ overflow \in BOOLEAN /\ cut \in BOOLEAN
    /\ jammed \subseteq Queues /\ dropped \subseteq Sessions /\ ringFull \in 0..MaxSessions
    /\ early \in BOOLEAN
    /\ held \in 0..MaxSessions * Burst
    /\ ~Simulator => held = 0

\* The ring never holds more than its entries: an append always finds a free one.
RingHolds == ~overflow /\ count <= RingSize

\* No burst entry is sealed past its burst: a packet sequence is never reused.
SealedWithinBurst == \A i \in Idx : ring[i].sealed <= Burst

\* No staged start is called that the transport will refuse for room.
NoRefusedStart == ~refused

\* No queue's staging ever holds more than its bank.
WithinBank == \A q \in Queues : staged[q] + inflight[q] <= BankSize

\* The layout refines the abstract owed set: the live window holds exactly
\* the owed bursts not yet complete or abandoned, each once, each entry's
\* count the packets its session was sent, and every other entry is clear.
RingIsOwed ==
    /\ \A i \in LiveIdx : ring[i].s \in owed
    /\ \A i \in LiveIdx : (ring[i].s \notin abandoned /\ ring[i].s \notin dropped) => ring[i].sealed = sent[ring[i].s]
    /\ \A i, j \in LiveIdx : i # j => ring[i].s # ring[j].s
    /\ \A s \in owed : (sent[s] < Burst /\ s \notin abandoned /\ s \notin dropped) => \E i \in LiveIdx : ring[i].s = s
    /\ \A i \in Idx \ LiveIdx : ring[i] = NoEntry

\* The head is never a finished burst: completion pops it at once.
HeadNeverDone == count > 0 => ring[head].sealed < Burst

\* Before the server reports STOPPED, every burst it kept (every client
\* connected at the stop, and every burst still waiting from a disconnect)
\* has been handed whole to the transport, but for a destination the
\* transport cannot reach or a queue that has no send room left.
FullBurstBeforeStopped ==
    phase \in {"IOSTOP", "STOPPED"} => \A s \in owed : sent[s] = Burst \/ s \in abandoned \/ s \in dropped

\* The I/O is told to stop only once every send it was handed has finished:
\* no flushed disconnect packet is cut off by the stop.
NoSendCutAtStop == ~cut

\* No burst is abandoned for want of room while its queue has room, or while
\* a send staged or in flight on it could still finish and free some.
NoDropWhileRoomCanReturn == ~early

\* Each burst abandoned for want of room was not yet complete, was not
\* already abandoned for its destination, and is counted as SEND_RING_FULL
\* exactly once; once the I/O is told to stop, every burst kept is settled
\* exactly one way: sent whole, abandoned for its destination, or abandoned
\* for want of room.
EachBurstSettledOnce ==
    /\ ringFull = Cardinality(dropped)
    /\ dropped \cap abandoned = {}
    /\ \A s \in dropped : sent[s] < Burst
    /\ phase \in {"IOSTOP", "STOPPED"} =>
          \A s \in owed :
              LET whole == sent[s] = Burst
                  gone == s \in abandoned
                  lost == s \in dropped
              IN /\ whole \/ gone \/ lost
                 /\ ~(whole /\ gone) /\ ~(whole /\ lost) /\ ~(gone /\ lost)

\* Only a burst bound for a jammed queue is ever abandoned for want of room:
\* a burst for a queue that keeps its room is sent whole (or abandoned for
\* its destination), whatever another queue does.
OnlyJammedQueuesDrop == \A s \in dropped : QueueOf(s) \in jammed

\* The I/O is told to stop only once the link simulator holds no copy: the
\* stop step's link_empty test.
NoCopyHeldAtStop == phase \in {"IOSTOP", "STOPPED"} => held = 0

\* A stop finishes.
StopFinishes == phase = "STOPPING" ~> phase = "STOPPED"

\* A stop finishes when a queue's room never returns. With Simulator = TRUE
\* the code does not have this (the open defect in the module comment).
StopFinishesWithoutRoom == (phase = "STOPPING" /\ jammed # {}) ~> phase = "STOPPED"

\* A stop with no queue jammed finishes, or a queue jams first: what the
\* stop keeps with the link simulator, whose held copies drain only while
\* every queue has room.
StopFinishesWhileUnjammed ==
    (phase = "STOPPING" /\ jammed = {}) ~> (phase = "STOPPED" \/ jammed # {})
=============================================================================
