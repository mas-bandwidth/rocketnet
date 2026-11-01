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
(* (ROCKETNET_MAX_CLIENTS) and drops the new burst's copies otherwise.     *)
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
(* packet of it can ever leave.                                            *)
(*                                                                         *)
(* Stop is a state machine across updates: Stop moves RUNNING to STOPPING  *)
(* and appends; each stop step (a poll of rocketnet_server_stopped, or a   *)
(* transport update) is the drain's seals within Room and a Flush; once    *)
(* the ring is empty and the last seal is flushed the transport's I/O is   *)
(* told to stop (RequestIoStop), and STOPPED follows once its workers have *)
(* exited (IoStopped).  Complete is the environment: the kernel or worker  *)
(* finishing a flushed frame, which returns its staging.                   *)
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
(* never finishes (StopFinishes).  A ring of SteadyCap entries, the size  *)
(* before this model, is the configuration witness                         *)
(* MCServerStopSpreadBrokenRingSmall (RingHolds).                          *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    SlotCount,      \* client slots (max_clients)
    MaxSessions,    \* exploration bound: connections over the run
    Burst,          \* packets per burst (ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS)
    BankSize,       \* frames the transport stages before frames come back
    RingSize,       \* ring entries (ROCKETNET_SERVER_DISCONNECT_BURSTS)
    SteadyCap,      \* bursts a running server keeps waiting (ROCKETNET_MAX_CLIENTS)
    Defect

ASSUME Defect \in {"none", "singleshot", "steadycapoffbyone", "noskip", "unreachablestalls", "stopinflight"}
ASSUME SlotCount \in Nat \ {0} /\ MaxSessions \in Nat \ {0} /\ Burst \in Nat \ {0}
ASSUME BankSize \in Nat \ {0} /\ RingSize \in Nat \ {0} /\ SteadyCap \in Nat \ {0}

Slots == 1..SlotCount
Sessions == 1..MaxSessions
Idx == 0..RingSize - 1
NoEntry == [s |-> 0, sealed |-> 0]
Phases == {"RUNNING", "STOPPING", "IOSTOP", "STOPPED"}

VARIABLES
    phase,          \* the server transport's state; IOSTOP: the I/O told to stop
    slot,           \* the session each slot holds, 0 when free
    nextSess,       \* the next session tag
    ring,           \* disconnect_bursts: the session and its sealed count
    head,           \* disconnect_burst_head
    count,          \* disconnect_burst_count
    cursor,         \* disconnect_burst_cursor
    staged,         \* frames started and finished since the last flush
    inflight,       \* frames flushed that the transport still holds
    sent,           \* ghost: each session's packets handed to the staging
    owed,           \* ghost: sessions whose burst was kept to be sent
    unreachable,    \* environment: sessions with no link-layer destination
    abandoned,      \* ghost: bursts given up at an unreachable start
    refused,        \* ghost: a start was called with no room
    overflow,       \* ghost: an append found the ring full
    cut             \* ghost: the I/O was told to stop with a flushed send unfinished

vars == <<phase, slot, nextSess, ring, head, count, cursor, staged, inflight,
          sent, owed, unreachable, abandoned, refused, overflow, cut>>

Room == BankSize - staged - inflight

LiveIdx == {(head + k) % RingSize : k \in 0..count - 1}

Init ==
    /\ phase = "RUNNING"
    /\ slot = [i \in Slots |-> 0]
    /\ nextSess = 1
    /\ ring = [i \in Idx |-> NoEntry]
    /\ head = 0 /\ count = 0 /\ cursor = 0
    /\ staged = 0 /\ inflight = 0
    /\ sent = [s \in Sessions |-> 0]
    /\ owed = {} /\ unreachable = {} /\ abandoned = {}
    /\ refused = FALSE /\ overflow = FALSE /\ cut = FALSE

\* A client connects into a free slot.
Connect(i) ==
    /\ phase = "RUNNING"
    /\ slot[i] = 0
    /\ nextSess <= MaxSessions
    /\ slot' = [slot EXCEPT ![i] = nextSess]
    /\ nextSess' = nextSess + 1
    /\ UNCHANGED <<phase, ring, head, count, cursor, staged, inflight, sent,
                   owed, unreachable, abandoned, refused, overflow, cut>>

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
                   unreachable, abandoned, refused, cut>>

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
                   abandoned, refused, cut>>

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
\* count). An entry already complete is passed over. An unreachable start
\* abandons the burst. Otherwise a packet is sealed only while the transport
\* reports room; with no room the drain ends this step and the burst waits.
DrainAt == IF cursor >= count THEN 0 ELSE cursor
SealOne ==
    /\ phase \in {"RUNNING", "STOPPING"}
    /\ count > 0
    /\ LET c == DrainAt
           idx == (head + c) % RingSize
           e == ring[idx]
       IN \/ /\ e.sealed = Burst /\ Defect # "noskip"
             /\ cursor' = c + 1
             /\ UNCHANGED <<ring, head, count, staged, sent, abandoned, refused>>
          \/ /\ e.sealed < Burst \/ Defect = "noskip"
             /\ e.s \in unreachable /\ Defect # "unreachablestalls"
             /\ (Room > 0 \/ Defect = "singleshot")
             /\ PopAfter([ring EXCEPT ![idx].sealed = Burst], c + 1)
             /\ abandoned' = abandoned \cup {e.s}
             /\ UNCHANGED <<staged, sent, refused>>
          \/ /\ e.sealed < Burst \/ Defect = "noskip"
             /\ e.s \notin unreachable
             /\ \/ /\ Room > 0
                   /\ staged' = staged + 1
                   /\ sent' = [sent EXCEPT ![e.s] = @ + 1]
                   /\ UNCHANGED refused
                \/ /\ Room = 0 /\ Defect = "singleshot"
                   /\ refused' = TRUE
                   /\ UNCHANGED <<staged, sent>>
             /\ PopAfter([ring EXCEPT ![idx].sealed = @ + 1], c + 1)
             /\ UNCHANGED abandoned
    /\ UNCHANGED <<phase, slot, nextSess, inflight, owed, unreachable, overflow, cut>>

\* The step's flush hands the staged frames to the transport.
Flush ==
    /\ staged > 0
    /\ inflight' = inflight + staged
    /\ staged' = 0
    /\ UNCHANGED <<phase, slot, nextSess, ring, head, count, cursor, sent, owed,
                   unreachable, abandoned, refused, overflow, cut>>

\* Environment: the kernel or the transport's worker finishes a flushed
\* frame, and its staging comes back.
Complete ==
    /\ inflight > 0
    /\ inflight' = inflight - 1
    /\ UNCHANGED <<phase, slot, nextSess, ring, head, count, cursor, staged, sent,
                   owed, unreachable, abandoned, refused, overflow, cut>>

\* Environment: a waiting burst's destination has no link-layer address.
LoseRoute(s) ==
    /\ s \in owed /\ s \notin unreachable
    /\ unreachable' = unreachable \cup {s}
    /\ UNCHANGED <<phase, slot, nextSess, ring, head, count, cursor, staged,
                   inflight, sent, owed, abandoned, refused, overflow, cut>>

\* Environment: the destination is learned again (a reply MAC refreshed).
\* A burst already abandoned stays abandoned: its entry is complete and the
\* drain passes over it, so no packet past its burst is ever sealed.
FindRoute(s) ==
    /\ s \in unreachable
    /\ unreachable' = unreachable \ {s}
    /\ UNCHANGED <<phase, slot, nextSess, ring, head, count, cursor, staged,
                   inflight, sent, owed, abandoned, refused, overflow, cut>>

\* The stop step that finds the ring empty, its seals flushed and every
\* flushed send finished (the transport's send_idle) tells the I/O to stop
\* (rocketnet_server_transport_io_stop). The single-shot stop told it at
\* once; "stopinflight" tells it with a flushed send unfinished, which the
\* stopping I/O cuts off (the IOCP and RIO send workers retire a bank they
\* find stopping).
RequestIoStop ==
    /\ phase = "STOPPING"
    /\ staged = 0
    /\ count = 0 \/ Defect = "singleshot"
    /\ inflight = 0 \/ Defect \in {"singleshot", "stopinflight"}
    /\ phase' = "IOSTOP"
    /\ cut' = (cut \/ inflight > 0)
    /\ UNCHANGED <<slot, nextSess, ring, head, count, cursor, staged, inflight,
                   sent, owed, unreachable, abandoned, refused, overflow>>

\* The workers have exited: rocketnet_server_stopped reports true.
IoStopped ==
    /\ phase = "IOSTOP"
    /\ phase' = "STOPPED"
    /\ UNCHANGED <<slot, nextSess, ring, head, count, cursor, staged, inflight,
                   sent, owed, unreachable, abandoned, refused, overflow, cut>>

\* STOPPED is the end of the run, a stutter here (a later start builds a new
\* transport and a new ring).
Next ==
    \/ \E i \in Slots : Connect(i) \/ Kick(i)
    \/ Stop
    \/ SealOne
    \/ Flush
    \/ Complete
    \/ \E s \in Sessions : LoseRoute(s) \/ FindRoute(s)
    \/ RequestIoStop
    \/ IoStopped
    \/ phase = "STOPPED" /\ UNCHANGED vars

Spec == Init /\ [][Next]_vars

\* The caller keeps polling rocketnet_server_stopped (or updating the
\* transport), and the transport finishes what it was handed.
Fairness ==
    /\ WF_vars(SealOne)
    /\ WF_vars(Flush)
    /\ WF_vars(Complete)
    /\ WF_vars(RequestIoStop)
    /\ WF_vars(IoStopped)
FairSpec == Spec /\ Fairness

TypeOK ==
    /\ phase \in Phases
    /\ slot \in [Slots -> 0..MaxSessions]
    /\ nextSess \in 1..MaxSessions + 1
    /\ ring \in [Idx -> [s : 0..MaxSessions, sealed : 0..Burst + 1]]
    /\ head \in Idx /\ count \in 0..RingSize + SlotCount /\ cursor \in 0..RingSize + SlotCount
    /\ staged \in 0..BankSize /\ inflight \in 0..BankSize
    /\ sent \in [Sessions -> 0..Burst]
    /\ owed \subseteq Sessions /\ unreachable \subseteq Sessions /\ abandoned \subseteq Sessions
    /\ refused \in BOOLEAN /\ overflow \in BOOLEAN /\ cut \in BOOLEAN

\* The ring never holds more than its entries: an append always finds a free one.
RingHolds == ~overflow /\ count <= RingSize

\* No burst entry is sealed past its burst: a packet sequence is never reused.
SealedWithinBurst == \A i \in Idx : ring[i].sealed <= Burst

\* No staged start is called that the transport will refuse for room.
NoRefusedStart == ~refused

\* The staging never holds more than the transport has.
WithinBank == staged + inflight <= BankSize

\* The layout refines the abstract owed set: the live window holds exactly
\* the owed bursts not yet complete or abandoned, each once, each entry's
\* count the packets its session was sent, and every other entry is clear.
RingIsOwed ==
    /\ \A i \in LiveIdx : ring[i].s \in owed
    /\ \A i \in LiveIdx : ring[i].s \notin abandoned => ring[i].sealed = sent[ring[i].s]
    /\ \A i, j \in LiveIdx : i # j => ring[i].s # ring[j].s
    /\ \A s \in owed : (sent[s] < Burst /\ s \notin abandoned) => \E i \in LiveIdx : ring[i].s = s
    /\ \A i \in Idx \ LiveIdx : ring[i] = NoEntry

\* The head is never a finished burst: completion pops it at once.
HeadNeverDone == count > 0 => ring[head].sealed < Burst

\* Before the server reports STOPPED, every burst it kept (every client
\* connected at the stop, and every burst still waiting from a disconnect)
\* has been handed whole to the transport, but for a destination the
\* transport cannot reach.
FullBurstBeforeStopped ==
    phase \in {"IOSTOP", "STOPPED"} => \A s \in owed : sent[s] = Burst \/ s \in abandoned

\* The I/O is told to stop only once every send it was handed has finished:
\* no flushed disconnect packet is cut off by the stop.
NoSendCutAtStop == ~cut

\* A stop finishes.
StopFinishes == phase = "STOPPING" ~> phase = "STOPPED"
=============================================================================
