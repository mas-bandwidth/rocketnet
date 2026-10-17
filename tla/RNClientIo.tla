------------------------------ MODULE RNClientIo ------------------------------
(***************************************************************************)
(* The client transport with its socket I/O on one I/O thread, as         *)
(* docs/CLIENT-IO.md designs it.  The code at the base of the design is   *)
(* sdk/rocketnet_client_transport.cpp: the payload ring the game threads  *)
(* fill (rocketnet_client_transport_start_packet), the flush that sends   *)
(* it per packet on the main thread                                       *)
(* (rocketnet_client_transport_send_packets, also called after every      *)
(* packet by rocketnet_client_transport_send_packet), the receive thread  *)
(* and its 1 ms poll, and the receive buffers update swaps under a mutex. *)
(*                                                                         *)
(* The design.  The payload ring becomes two rings, A and B.  Game        *)
(* threads write slots of the ring that is Filling.  A hand-off (from     *)
(* send_packets, from send_packet, which calls it, or at the end of       *)
(* update) gives that ring to the I/O thread with one release store and   *)
(* one wake when the other ring is open, and the main thread moves to the *)
(* other ring.  A hand-off never waits: when the other ring is still      *)
(* HandedOff or Sending, the committed slots stay in the Filling ring,    *)
(* the deferral is counted, and they leave with the next hand-off that    *)
(* finds the other ring open.  The I/O thread sends every slot of the     *)
(* handed-off ring, then marks it Sent; only then does the main thread    *)
(* reopen it.  Receive runs on the same thread into two buffers of        *)
(* BufferCap datagrams, exchanged by a publish and return handshake: the  *)
(* I/O thread fills its buffer and publishes it Ready at a batch end      *)
(* when a Free buffer is there to fill next.  update is two steps: at its *)
(* start it exposes the Ready buffer if there is one (Expose), and at its *)
(* end, every update, it returns the buffer it holds as Free (Return).    *)
(* An I/O thread that has an unpublished batch and no Free buffer records *)
(* that a publish is owed before it blocks; Return wakes it when the owed *)
(* flag is set.  The I/O thread waits on socket readable or the wake, and *)
(* on the wake only while its buffer is full, so datagrams left in the    *)
(* socket do not end the wait at once.  stop sets the wake, the I/O       *)
(* thread finishes the rings handed off and exits, the main thread joins  *)
(* it and only then frees the transport; destroy makes the last hand-off  *)
(* first, so no committed slot is dropped.                                *)
(*                                                                         *)
(* Five switches restore the defects the reversed witnesses must catch:   *)
(* ReopenBeforeSent lets the main thread reopen a ring the I/O thread is  *)
(* still sending; ReceiveIntoExposed lets the I/O thread write a          *)
(* datagram into the buffer update has exposed; SwapNeedsReady restores   *)
(* the one-step swap, where update returns its exposed buffer only in     *)
(* the same step that exposes a Ready one; HandOffWaits restores the      *)
(* hand-off that waits for the other ring to be Sent; FullWaitReadable    *)
(* restores the wait on readable with a full buffer, which returns at     *)
(* once while datagrams sit in the socket.                                *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS NumSlots,            \* slots per send ring
          BufferCap,           \* datagrams one receive buffer holds
          MaxDatagrams,        \* datagrams that arrive in one behaviour
          ReopenBeforeSent,    \* defect: reopen a ring while it is Sending
          ReceiveIntoExposed,  \* defect: receive writes the exposed buffer
          SwapNeedsReady,      \* defect: update returns only when it exposes
          HandOffWaits,        \* defect: a hand-off waits for the other ring
          FullWaitReadable     \* defect: a full buffer waits on readable too

ASSUME NumSlots \in Nat \ {0} /\ BufferCap \in Nat \ {0}
ASSUME MaxDatagrams \in Nat
ASSUME /\ ReopenBeforeSent \in BOOLEAN /\ ReceiveIntoExposed \in BOOLEAN
       /\ SwapNeedsReady \in BOOLEAN /\ HandOffWaits \in BOOLEAN
       /\ FullWaitReadable \in BOOLEAN

Rings   == {"A", "B"}
Slots   == 1..NumSlots
Buffers == {0, 1}
Other(r) == IF r = "A" THEN "B" ELSE "A"

RingStates   == {"Filling", "HandedOff", "Sending", "Sent"}
Owners       == {"free", "game", "main", "io"}
BufferStates == {"Fill", "Ready", "Exposed", "Free"}
Phases       == {"Running", "Stopping", "Joined", "Freed"}

VARIABLES ring,      \* each send ring's state
          owner,     \* each slot's owner: free, a game thread, main (committed), io
          sent,      \* slots of a ring the I/O thread has sent
          fill,      \* the ring the game threads fill
          ioRing,    \* the ring the I/O thread is sending, or "none"
          deferred,  \* CLIENT_SEND_DEFERRED has moved (a hand-off deferred)
          mainWait,  \* the main thread is blocked on the I/O thread
          wake,      \* the level-triggered wake (eventfd, kevent or event)
          waiting,   \* the I/O thread is blocked in its wait
          waitReadable, \* the blocked wait also returns on socket readable
          pending,   \* datagrams readable on the socket
          arrived,   \* datagrams that have arrived so far
          buf,       \* each receive buffer's state
          count,     \* datagrams in each receive buffer
          snap,      \* the exposed buffer's count when update exposed it
          owed,      \* the I/O thread holds a batch it cannot publish yet
          delivered, \* datagrams update has been exposed so far
          phase,     \* the transport's life
          ioAlive    \* the I/O thread has not exited

sendVars == <<ring, owner, sent, fill, ioRing, deferred, mainWait>>
recvVars == <<pending, arrived, buf, count, snap, owed, delivered>>

vars == <<ring, owner, sent, fill, ioRing, deferred, mainWait, wake, waiting,
          waitReadable, pending, arrived, buf, count, snap, owed, delivered,
          phase, ioAlive>>

TypeOK ==
    /\ ring \in [Rings -> RingStates]
    /\ owner \in [Rings -> [Slots -> Owners]]
    /\ sent \in [Rings -> SUBSET Slots]
    /\ fill \in Rings
    /\ ioRing \in Rings \cup {"none"}
    /\ deferred \in BOOLEAN /\ mainWait \in BOOLEAN
    /\ wake \in BOOLEAN /\ waiting \in BOOLEAN /\ waitReadable \in BOOLEAN
    /\ pending \in 0..MaxDatagrams /\ arrived \in 0..MaxDatagrams
    /\ buf \in [Buffers -> BufferStates]
    /\ count \in [Buffers -> 0..BufferCap]
    /\ snap \in 0..BufferCap
    /\ owed \in BOOLEAN
    /\ delivered \in 0..MaxDatagrams
    /\ phase \in Phases
    /\ ioAlive \in BOOLEAN

Init ==
    /\ ring = [r \in Rings |-> "Filling"]
    /\ owner = [r \in Rings |-> [s \in Slots |-> "free"]]
    /\ sent = [r \in Rings |-> {}]
    /\ fill = "A"
    /\ ioRing = "none"
    /\ deferred = FALSE /\ mainWait = FALSE
    /\ wake = FALSE /\ waiting = FALSE /\ waitReadable = TRUE
    /\ pending = 0 /\ arrived = 0
    /\ buf = [b \in Buffers |-> IF b = 0 THEN "Fill" ELSE "Free"]
    /\ count = [b \in Buffers |-> 0]
    /\ snap = 0
    /\ owed = FALSE
    /\ delivered = 0
    /\ phase = "Running"
    /\ ioAlive = TRUE

FillBuffer    == CHOOSE b \in Buffers : buf[b] = "Fill"
HasExposed    == \E b \in Buffers : buf[b] = "Exposed"
ExposedBuffer == CHOOSE b \in Buffers : buf[b] = "Exposed"
HasReady      == \E b \in Buffers : buf[b] = "Ready"
HasFree       == \E b \in Buffers : buf[b] = "Free"

\* the main thread is running its own code, not blocked on the I/O thread
MainFree == phase = "Running" /\ ~mainWait

\* a hand-off is due: fills joined (the phase contract) and slots committed
HandOffDue ==
    /\ ring[fill] = "Filling"
    /\ \A s \in Slots : owner[fill][s] # "game"
    /\ \E s \in Slots : owner[fill][s] = "main"

(* ---- game threads and the main thread ---- *)

\* start_packet: a game thread takes a free slot of the Filling ring
Write(s) ==
    /\ phase = "Running"
    /\ ring[fill] = "Filling"
    /\ owner[fill][s] = "free"
    /\ owner' = [owner EXCEPT ![fill][s] = "game"]
    /\ UNCHANGED <<ring, sent, fill, ioRing, deferred, mainWait, wake,
                   waiting, waitReadable, phase, ioAlive>>
    /\ UNCHANGED recvVars

\* finish_packet: the slot is sealed and committed
Commit(s) ==
    /\ owner[fill][s] = "game"
    /\ owner' = [owner EXCEPT ![fill][s] = "main"]
    /\ UNCHANGED <<ring, sent, fill, ioRing, deferred, mainWait, wake,
                   waiting, waitReadable, phase, ioAlive>>
    /\ UNCHANGED recvVars

\* the hand-off (send_packets, send_packet through it, or the end of
\* update) when the other ring is open: the committed ring goes to the I/O
\* thread with one release store and one wake
HandOff ==
    /\ MainFree
    /\ HandOffDue
    /\ ring[Other(fill)] = "Filling"
    /\ ring' = [ring EXCEPT ![fill] = "HandedOff"]
    /\ owner' = [owner EXCEPT ![fill] =
                    [s \in Slots |-> IF owner[fill][s] = "main" THEN "io"
                                     ELSE owner[fill][s]]]
    /\ fill' = Other(fill)
    /\ wake' = TRUE
    /\ UNCHANGED <<sent, ioRing, deferred, mainWait, waiting, waitReadable,
                   phase, ioAlive>>
    /\ UNCHANGED recvVars

\* the hand-off when the other ring is still HandedOff or Sending: the
\* committed slots stay in the Filling ring and CLIENT_SEND_DEFERRED moves;
\* they leave with the next hand-off.  Under HandOffWaits the main thread
\* blocks on the I/O thread instead.
Defer ==
    /\ MainFree
    /\ HandOffDue
    /\ ring[Other(fill)] \in {"HandedOff", "Sending"}
    /\ IF HandOffWaits THEN /\ mainWait' = TRUE
                            /\ UNCHANGED deferred
                       ELSE /\ deferred' = TRUE
                            /\ UNCHANGED mainWait
    /\ UNCHANGED <<ring, owner, sent, fill, ioRing, wake, waiting,
                   waitReadable, phase, ioAlive>>
    /\ UNCHANGED recvVars

\* the defect's blocked main thread resumes once the other ring is Sent
WaitDone ==
    /\ mainWait
    /\ ring[Other(fill)] = "Sent"
    /\ mainWait' = FALSE
    /\ UNCHANGED <<ring, owner, sent, fill, ioRing, deferred, wake, waiting,
                   waitReadable, phase, ioAlive>>
    /\ UNCHANGED recvVars

\* a hand-off reopens a ring the I/O thread has finished (acquire load)
Reopen(r) ==
    /\ ~mainWait
    /\ r # fill
    /\ IF ReopenBeforeSent THEN ring[r] \in {"Sending", "Sent"}
                           ELSE ring[r] = "Sent"
    /\ ring' = [ring EXCEPT ![r] = "Filling"]
    /\ owner' = [owner EXCEPT ![r] = [s \in Slots |-> "free"]]
    /\ sent' = [sent EXCEPT ![r] = {}]
    /\ UNCHANGED <<fill, ioRing, deferred, mainWait, wake, waiting,
                   waitReadable, phase, ioAlive>>
    /\ UNCHANGED recvVars

\* update, first step: expose the Ready buffer (acquire) when there is
\* one.  The design holds no buffer at this point (the last update
\* returned it).  Under SwapNeedsReady this is the one-step swap of the
\* defect: the held buffer is returned here, and only here.
Expose ==
    /\ MainFree
    /\ HasReady
    /\ SwapNeedsReady \/ ~HasExposed
    /\ LET rb == CHOOSE b \in Buffers : buf[b] = "Ready" IN
         /\ buf' = [b \in Buffers |->
                      IF b = rb THEN "Exposed"
                      ELSE IF SwapNeedsReady /\ buf[b] = "Exposed" THEN "Free"
                      ELSE buf[b]]
         /\ snap' = count[rb]
         /\ delivered' = delivered + count[rb]
    /\ UNCHANGED sendVars
    /\ UNCHANGED <<wake, waiting, waitReadable, pending, arrived, count,
                   owed, phase, ioAlive>>

\* update, last step, every update: the buffer it exposed is returned as
\* Free (seq_cst store), then the owed flag is read (seq_cst load) and the
\* I/O thread is woken when a publish is owed.
Return ==
    /\ ~SwapNeedsReady
    /\ MainFree
    /\ HasExposed
    /\ buf' = [buf EXCEPT ![ExposedBuffer] = "Free"]
    /\ wake' = (wake \/ owed)
    /\ UNCHANGED sendVars
    /\ UNCHANGED <<waiting, waitReadable, pending, arrived, count, snap, owed,
                   delivered, phase, ioAlive>>

\* destroy, first half: once the Filling ring holds no committed slot
\* (destroy makes the last hand-off itself, waiting for the other ring to
\* be Sent if it must; that wait is this guard), ask the I/O thread to
\* stop and wake it
Stop ==
    /\ MainFree
    /\ \A s \in Slots : owner[fill][s] = "free"
    /\ phase' = "Stopping"
    /\ wake' = TRUE
    /\ UNCHANGED sendVars
    /\ UNCHANGED <<waiting, waitReadable, ioAlive>>
    /\ UNCHANGED recvVars

Join ==
    /\ phase = "Stopping"
    /\ ~ioAlive
    /\ phase' = "Joined"
    /\ UNCHANGED sendVars
    /\ UNCHANGED <<wake, waiting, waitReadable, ioAlive>>
    /\ UNCHANGED recvVars

Free ==
    /\ phase = "Joined"
    /\ phase' = "Freed"
    /\ UNCHANGED sendVars
    /\ UNCHANGED <<wake, waiting, waitReadable, ioAlive>>
    /\ UNCHANGED recvVars

(* ---- the network ---- *)

Arrive ==
    /\ arrived < MaxDatagrams
    /\ arrived' = arrived + 1
    /\ pending' = pending + 1
    /\ UNCHANGED sendVars
    /\ UNCHANGED <<wake, waiting, waitReadable, buf, count, snap, owed,
                   delivered, phase, ioAlive>>

(* ---- the I/O thread ---- *)

IoReady == ioAlive /\ ~waiting

\* send one slot of the handed-off ring
IoSend(r, s) ==
    /\ IoReady
    /\ \/ ioRing = r
       \/ ioRing = "none" /\ ring[r] = "HandedOff"
    /\ owner[r][s] = "io"
    /\ s \notin sent[r]
    /\ ioRing' = r
    /\ ring' = [ring EXCEPT ![r] = IF ring[r] = "HandedOff" THEN "Sending"
                                    ELSE ring[r]]
    /\ sent' = [sent EXCEPT ![r] = sent[r] \cup {s}]
    /\ UNCHANGED <<owner, fill, deferred, mainWait, wake, waiting,
                   waitReadable, phase, ioAlive>>
    /\ UNCHANGED recvVars

\* every slot sent: the ring is released to the main thread (release store)
IoFinishRing(r) ==
    /\ IoReady
    /\ ioRing = r
    /\ \A s \in Slots : owner[r][s] = "io" => s \in sent[r]
    /\ ring' = [ring EXCEPT ![r] = "Sent"]
    /\ ioRing' = "none"
    /\ UNCHANGED <<owner, sent, fill, deferred, mainWait, wake, waiting,
                   waitReadable, phase, ioAlive>>
    /\ UNCHANGED recvVars

\* read one datagram into the I/O thread's buffer while it has room
IoReceive ==
    /\ IoReady
    /\ pending > 0
    /\ LET target == IF ReceiveIntoExposed /\ HasExposed THEN ExposedBuffer
                     ELSE FillBuffer IN
         /\ count[target] < BufferCap
         /\ count' = [count EXCEPT ![target] = count[target] + 1]
    /\ pending' = pending - 1
    /\ UNCHANGED sendVars
    /\ UNCHANGED <<wake, waiting, waitReadable, arrived, buf, snap, owed,
                   delivered, phase, ioAlive>>

\* batch end: publish the filled buffer when a returned one is there to fill
IoPublish ==
    /\ IoReady
    /\ count[FillBuffer] > 0
    /\ HasFree
    /\ LET fb == FillBuffer
           nb == CHOOSE b \in Buffers : buf[b] = "Free" IN
         /\ buf' = [buf EXCEPT ![fb] = "Ready", ![nb] = "Fill"]
         /\ count' = [count EXCEPT ![nb] = 0]
    /\ owed' = FALSE
    /\ UNCHANGED sendVars
    /\ UNCHANGED <<wake, waiting, waitReadable, pending, arrived, snap,
                   delivered, phase, ioAlive>>

\* batch end with no Free buffer: record that a publish is owed (seq_cst
\* store) before the wait; the wait re-checks for a Free buffer after it
IoOwe ==
    /\ IoReady
    /\ count[FillBuffer] > 0
    /\ ~HasFree
    /\ ~owed
    /\ owed' = TRUE
    /\ UNCHANGED sendVars
    /\ UNCHANGED <<wake, waiting, waitReadable, pending, arrived, buf, count,
                   snap, delivered, phase, ioAlive>>

\* nothing to do: block.  The thread never blocks while it could publish,
\* blocks with an unpublished batch only after it has recorded the publish
\* as owed, and blocks with datagrams in the socket only when its buffer is
\* full.  The wait returns on readable only while the buffer has room
\* (wait's readable argument); a full buffer waits on the wake alone.
IoWait ==
    /\ IoReady
    /\ phase = "Running"
    /\ ioRing = "none"
    /\ \A r \in Rings : ring[r] # "HandedOff"
    /\ ~wake
    /\ ~(count[FillBuffer] > 0 /\ HasFree)
    /\ count[FillBuffer] > 0 => owed
    /\ pending = 0 \/ count[FillBuffer] = BufferCap
    /\ waiting' = TRUE
    /\ waitReadable' = (FullWaitReadable \/ count[FillBuffer] < BufferCap)
    /\ UNCHANGED sendVars
    /\ UNCHANGED <<wake, phase, ioAlive>>
    /\ UNCHANGED recvVars

IoWake ==
    /\ ioAlive /\ waiting
    /\ wake \/ (waitReadable /\ pending > 0)
    /\ waiting' = FALSE
    /\ wake' = FALSE
    /\ UNCHANGED sendVars
    /\ UNCHANGED <<waitReadable, phase, ioAlive>>
    /\ UNCHANGED recvVars

\* a woken I/O thread consumes a wake it does not need to block on
IoClearWake ==
    /\ IoReady
    /\ wake
    /\ wake' = FALSE
    /\ UNCHANGED sendVars
    /\ UNCHANGED <<waiting, waitReadable, phase, ioAlive>>
    /\ UNCHANGED recvVars

\* stop seen: the ring in flight is finished first, then the thread exits
IoExit ==
    /\ IoReady
    /\ phase = "Stopping"
    /\ ioRing = "none"
    /\ \A r \in Rings : ring[r] # "HandedOff"
    /\ ioAlive' = FALSE
    /\ UNCHANGED sendVars
    /\ UNCHANGED <<wake, waiting, waitReadable, phase>>
    /\ UNCHANGED recvVars

Done ==
    /\ phase = "Freed"
    /\ UNCHANGED vars

MainNext ==
    \/ \E s \in Slots : Write(s) \/ Commit(s)
    \/ HandOff \/ Defer \/ WaitDone
    \/ \E r \in Rings : Reopen(r)
    \/ Expose \/ Return \/ Stop \/ Join \/ Free

IoNext ==
    \/ \E r \in Rings, s \in Slots : IoSend(r, s)
    \/ \E r \in Rings : IoFinishRing(r)
    \/ IoReceive \/ IoPublish \/ IoOwe \/ IoWait \/ IoWake \/ IoClearWake
    \/ IoExit

Next == MainNext \/ IoNext \/ Arrive \/ Done

Spec == Init /\ [][Next]_vars

\* Weak fairness of the I/O thread's actions, the join, a game thread's
\* finish of a packet it started, the reopen, the hand-off and update's
\* two receive steps: the game calls update every frame while the
\* transport runs, and update ends with a hand-off, so a deferred hand-off
\* is retried every frame.  A game thread mid-write disables the hand-off
\* only until it commits, and a ring has NumSlots slots, so weak fairness
\* is enough.
SpecFair ==
    /\ Spec
    /\ WF_vars(\E r \in Rings, s \in Slots : IoSend(r, s))
    /\ WF_vars(\E r \in Rings : IoFinishRing(r))
    /\ WF_vars(IoReceive)
    /\ WF_vars(IoPublish)
    /\ WF_vars(IoOwe)
    /\ WF_vars(IoWake)
    /\ WF_vars(IoExit)
    /\ WF_vars(Join)
    /\ WF_vars(\E s \in Slots : Commit(s))
    /\ WF_vars(\E r \in Rings : Reopen(r))
    /\ WF_vars(HandOff)
    /\ WF_vars(Expose)
    /\ WF_vars(Return)

(* ---- what the design must keep ---- *)

\* a slot of a ring is never written while the I/O thread owns that ring
NoReuseBeforeSent ==
    \A r \in Rings, s \in Slots :
        owner[r][s] = "game" => /\ ioRing # r
                                /\ ring[r] = "Filling"

\* the exposed receive buffer never changes between Expose and Return
ExposedStable ==
    \A b \in Buffers : buf[b] = "Exposed" => count[b] = snap

\* the transport is freed only after the I/O thread has exited and been joined
FreedOnlyAfterJoin ==
    phase = "Freed" => /\ ~ioAlive
                       /\ ioRing = "none"

\* a hand-off never blocks the main thread on the I/O thread
HandOffNeverWaits == ~mainWait

\* a blocked I/O thread with a full buffer waits on the wake alone: a wait
\* that also returns on readable returns at once while datagrams sit in the
\* socket, and the thread spins until update returns a buffer
FullWaitsOnWake ==
    (waiting /\ count[FillBuffer] = BufferCap) => ~waitReadable

\* while the transport runs, received datagrams in the I/O thread's buffer
\* always have a next step toward update: a publish, an expose, a return,
\* or a wake of the blocked I/O thread
NoReceiveStall ==
    (phase = "Running" /\ count[FillBuffer] > 0) =>
        ENABLED (IoPublish \/ Expose \/ Return \/ IoWake)

HandedOffEventuallySent ==
    \A r \in Rings : ring[r] = "HandedOff" ~> ring[r] = "Sent"

\* a committed slot, deferred or not, is eventually handed to the I/O
\* thread, and destroy drops none
CommittedEventuallyHandedOff ==
    \A r \in Rings, s \in Slots :
        owner[r][s] = "main" ~> owner[r][s] = "io"

StopEventuallyJoins ==
    phase = "Stopping" ~> phase \in {"Joined", "Freed"}

\* a buffer of received datagrams is eventually exposed to update
FilledEventuallyExposed ==
    \A b \in Buffers :
        (buf[b] = "Fill" /\ count[b] > 0) ~> (buf[b] = "Exposed" \/ phase # "Running")

\* every datagram that arrives eventually reaches update
ArrivedEventuallyDelivered ==
    \A n \in 1..MaxDatagrams :
        arrived >= n ~> (delivered >= n \/ phase # "Running")

=============================================================================
