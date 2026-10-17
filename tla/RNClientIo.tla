------------------------------ MODULE RNClientIo ------------------------------
(***************************************************************************)
(* The client transport with its socket I/O on one I/O thread, as         *)
(* docs/CLIENT-IO.md designs it.  The code at the base of the design is   *)
(* sdk/rocketnet_client_transport.cpp: the payload ring the game threads  *)
(* fill (rocketnet_client_transport_start_packet, line 697), the flush    *)
(* that sends it per packet on the main thread                            *)
(* (rocketnet_client_transport_send_packets, line 987), the receive       *)
(* thread (created at line 407, its loop at line 1275) and the receive    *)
(* buffers update swaps under a mutex (line 515).                         *)
(*                                                                         *)
(* The design.  The payload ring becomes two rings, A and B.  Game        *)
(* threads write slots of the ring that is Filling; send_packets hands    *)
(* that ring to the I/O thread with one release store and one wake, and   *)
(* the main thread moves to the other ring.  The I/O thread sends every   *)
(* slot of the handed-off ring, then marks it Sent; only then does the    *)
(* main thread reopen it.  Receive runs on the same thread into two       *)
(* buffers exchanged by a publish and return handshake: the I/O thread    *)
(* fills its buffer and publishes it Ready at a batch end when a Free     *)
(* buffer is there to fill next.  update is two steps: at its start it    *)
(* exposes the Ready buffer if there is one (Expose), and at its end,     *)
(* every update, whether or not it exposed anything this time, it         *)
(* returns the buffer it holds as Free (Return).  An I/O thread that has  *)
(* an unpublished batch and no Free buffer records that a publish is      *)
(* owed before it blocks; Return wakes it when the owed flag is set.  No  *)
(* mutex is held by either thread.  The I/O thread waits on socket        *)
(* readable or the wake; stop sets the wake, the I/O thread finishes the  *)
(* ring in flight and exits, the main thread joins it and only then       *)
(* frees the transport.                                                   *)
(*                                                                         *)
(* Three switches restore the defects the reversed witnesses must catch:  *)
(* ReopenBeforeSent lets the main thread reopen a ring the I/O thread is  *)
(* still sending; ReceiveIntoExposed lets the I/O thread write a          *)
(* datagram into the buffer update has exposed; SwapNeedsReady restores   *)
(* the one-step swap, where update returns its exposed buffer only in     *)
(* the same step that exposes a Ready one, so with two buffers the        *)
(* receive path stalls after the first exposed batch.                     *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS NumSlots,            \* slots per send ring
          MaxHandOffs,         \* send_packets hand-offs in one behaviour
          MaxDatagrams,        \* datagrams that arrive in one behaviour
          ReopenBeforeSent,    \* defect: reopen a ring while it is Sending
          ReceiveIntoExposed,  \* defect: receive writes the exposed buffer
          SwapNeedsReady       \* defect: update returns only when it exposes

ASSUME NumSlots \in Nat \ {0} /\ MaxHandOffs \in Nat /\ MaxDatagrams \in Nat
ASSUME /\ ReopenBeforeSent \in BOOLEAN /\ ReceiveIntoExposed \in BOOLEAN
       /\ SwapNeedsReady \in BOOLEAN

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
          handoffs,  \* hand-offs so far
          wake,      \* the level-triggered wake (eventfd, pipe or event)
          waiting,   \* the I/O thread is blocked in its wait
          pending,   \* datagrams readable on the socket
          arrived,   \* datagrams that have arrived so far
          buf,       \* each receive buffer's state
          count,     \* datagrams in each receive buffer
          snap,      \* the exposed buffer's count when update exposed it
          owed,      \* the I/O thread holds a batch it cannot publish yet
          delivered, \* datagrams update has been exposed so far
          phase,     \* the transport's life
          ioAlive    \* the I/O thread has not exited

sendVars == <<ring, owner, sent, fill, ioRing, handoffs>>
recvVars == <<pending, arrived, buf, count, snap, owed, delivered>>

vars == <<ring, owner, sent, fill, ioRing, handoffs, wake, waiting, pending,
          arrived, buf, count, snap, owed, delivered, phase, ioAlive>>

TypeOK ==
    /\ ring \in [Rings -> RingStates]
    /\ owner \in [Rings -> [Slots -> Owners]]
    /\ sent \in [Rings -> SUBSET Slots]
    /\ fill \in Rings
    /\ ioRing \in Rings \cup {"none"}
    /\ handoffs \in 0..MaxHandOffs
    /\ wake \in BOOLEAN /\ waiting \in BOOLEAN
    /\ pending \in 0..MaxDatagrams /\ arrived \in 0..MaxDatagrams
    /\ buf \in [Buffers -> BufferStates]
    /\ count \in [Buffers -> 0..MaxDatagrams]
    /\ snap \in 0..MaxDatagrams
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
    /\ handoffs = 0
    /\ wake = FALSE /\ waiting = FALSE
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

(* ---- game threads and the main thread ---- *)

\* start_packet: a game thread takes a free slot of the Filling ring
Write(s) ==
    /\ phase = "Running"
    /\ ring[fill] = "Filling"
    /\ owner[fill][s] = "free"
    /\ owner' = [owner EXCEPT ![fill][s] = "game"]
    /\ UNCHANGED <<ring, sent, fill, ioRing, handoffs, wake, waiting, phase,
                   ioAlive>>
    /\ UNCHANGED recvVars

\* finish_packet: the slot is sealed and committed
Commit(s) ==
    /\ owner[fill][s] = "game"
    /\ owner' = [owner EXCEPT ![fill][s] = "main"]
    /\ UNCHANGED <<ring, sent, fill, ioRing, handoffs, wake, waiting, phase,
                   ioAlive>>
    /\ UNCHANGED recvVars

\* send_packets: fills are joined (the phase contract), the other ring is
\* open, the committed ring goes to the I/O thread with one release store
\* and one wake.  If the other ring is not open yet, send_packets waits.
HandOff ==
    /\ phase = "Running"
    /\ handoffs < MaxHandOffs
    /\ ring[fill] = "Filling"
    /\ \A s \in Slots : owner[fill][s] # "game"
    /\ \E s \in Slots : owner[fill][s] = "main"
    /\ ring[Other(fill)] = "Filling"
    /\ ring' = [ring EXCEPT ![fill] = "HandedOff"]
    /\ owner' = [owner EXCEPT ![fill] =
                    [s \in Slots |-> IF owner[fill][s] = "main" THEN "io"
                                     ELSE owner[fill][s]]]
    /\ fill' = Other(fill)
    /\ handoffs' = handoffs + 1
    /\ wake' = TRUE
    /\ UNCHANGED <<sent, ioRing, waiting, phase, ioAlive>>
    /\ UNCHANGED recvVars

\* the next send_packets reopens a ring the I/O thread has finished
Reopen(r) ==
    /\ r # fill
    /\ IF ReopenBeforeSent THEN ring[r] \in {"Sending", "Sent"}
                           ELSE ring[r] = "Sent"
    /\ ring' = [ring EXCEPT ![r] = "Filling"]
    /\ owner' = [owner EXCEPT ![r] = [s \in Slots |-> "free"]]
    /\ sent' = [sent EXCEPT ![r] = {}]
    /\ UNCHANGED <<fill, ioRing, handoffs, wake, waiting, phase, ioAlive>>
    /\ UNCHANGED recvVars

\* update, first step: expose the Ready buffer (acquire) when there is
\* one.  The design holds no buffer at this point (the last update
\* returned it).  Under SwapNeedsReady this is the one-step swap of the
\* defect: the held buffer is returned here, and only here.
Expose ==
    /\ phase = "Running"
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
    /\ UNCHANGED <<wake, waiting, pending, arrived, count, owed, phase,
                   ioAlive>>

\* update, last step, every update: the buffer it exposed is returned as
\* Free (seq_cst store), then the owed flag is read (seq_cst load) and the
\* I/O thread is woken when a publish is owed.
Return ==
    /\ ~SwapNeedsReady
    /\ phase = "Running"
    /\ HasExposed
    /\ buf' = [buf EXCEPT ![ExposedBuffer] = "Free"]
    /\ wake' = (wake \/ owed)
    /\ UNCHANGED sendVars
    /\ UNCHANGED <<waiting, pending, arrived, count, snap, owed, delivered,
                   phase, ioAlive>>

\* destroy, first half: ask the I/O thread to stop and wake it
Stop ==
    /\ phase = "Running"
    /\ phase' = "Stopping"
    /\ wake' = TRUE
    /\ UNCHANGED sendVars
    /\ UNCHANGED <<waiting, ioAlive>>
    /\ UNCHANGED recvVars

Join ==
    /\ phase = "Stopping"
    /\ ~ioAlive
    /\ phase' = "Joined"
    /\ UNCHANGED sendVars
    /\ UNCHANGED <<wake, waiting, ioAlive>>
    /\ UNCHANGED recvVars

Free ==
    /\ phase = "Joined"
    /\ phase' = "Freed"
    /\ UNCHANGED sendVars
    /\ UNCHANGED <<wake, waiting, ioAlive>>
    /\ UNCHANGED recvVars

(* ---- the network ---- *)

Arrive ==
    /\ arrived < MaxDatagrams
    /\ arrived' = arrived + 1
    /\ pending' = pending + 1
    /\ UNCHANGED sendVars
    /\ UNCHANGED <<wake, waiting, buf, count, snap, owed, delivered, phase,
                   ioAlive>>

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
    /\ UNCHANGED <<owner, fill, handoffs, wake, waiting, phase, ioAlive>>
    /\ UNCHANGED recvVars

\* every slot sent: the ring is released to the main thread (release store)
IoFinishRing(r) ==
    /\ IoReady
    /\ ioRing = r
    /\ \A s \in Slots : owner[r][s] = "io" => s \in sent[r]
    /\ ring' = [ring EXCEPT ![r] = "Sent"]
    /\ ioRing' = "none"
    /\ UNCHANGED <<owner, sent, fill, handoffs, wake, waiting, phase, ioAlive>>
    /\ UNCHANGED recvVars

\* read one datagram into the I/O thread's buffer
IoReceive ==
    /\ IoReady
    /\ pending > 0
    /\ LET target == IF ReceiveIntoExposed /\ HasExposed THEN ExposedBuffer
                     ELSE FillBuffer IN
         count' = [count EXCEPT ![target] = count[target] + 1]
    /\ pending' = pending - 1
    /\ UNCHANGED sendVars
    /\ UNCHANGED <<wake, waiting, arrived, buf, snap, owed, delivered, phase,
                   ioAlive>>

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
    /\ UNCHANGED <<wake, waiting, pending, arrived, snap, delivered, phase,
                   ioAlive>>

\* batch end with no Free buffer: record that a publish is owed (seq_cst
\* store) before the wait; the wait re-checks for a Free buffer after it
IoOwe ==
    /\ IoReady
    /\ count[FillBuffer] > 0
    /\ ~HasFree
    /\ ~owed
    /\ owed' = TRUE
    /\ UNCHANGED sendVars
    /\ UNCHANGED <<wake, waiting, pending, arrived, buf, count, snap,
                   delivered, phase, ioAlive>>

\* nothing to do: block on socket readable or the wake.  The thread never
\* blocks while it could publish, and blocks with an unpublished batch only
\* after it has recorded the publish as owed.
IoWait ==
    /\ IoReady
    /\ phase = "Running"
    /\ ioRing = "none"
    /\ \A r \in Rings : ring[r] # "HandedOff"
    /\ pending = 0
    /\ ~wake
    /\ ~(count[FillBuffer] > 0 /\ HasFree)
    /\ count[FillBuffer] > 0 => owed
    /\ waiting' = TRUE
    /\ UNCHANGED sendVars
    /\ UNCHANGED <<wake, phase, ioAlive>>
    /\ UNCHANGED recvVars

IoWake ==
    /\ ioAlive /\ waiting
    /\ wake \/ pending > 0
    /\ waiting' = FALSE
    /\ wake' = FALSE
    /\ UNCHANGED sendVars
    /\ UNCHANGED <<phase, ioAlive>>
    /\ UNCHANGED recvVars

\* a woken I/O thread consumes a wake it does not need to block on
IoClearWake ==
    /\ IoReady
    /\ wake
    /\ wake' = FALSE
    /\ UNCHANGED sendVars
    /\ UNCHANGED <<waiting, phase, ioAlive>>
    /\ UNCHANGED recvVars

\* stop seen: the ring in flight is finished first, then the thread exits
IoExit ==
    /\ IoReady
    /\ phase = "Stopping"
    /\ ioRing = "none"
    /\ \A r \in Rings : ring[r] # "HandedOff"
    /\ ioAlive' = FALSE
    /\ UNCHANGED sendVars
    /\ UNCHANGED <<wake, waiting, phase>>
    /\ UNCHANGED recvVars

Done ==
    /\ phase = "Freed"
    /\ UNCHANGED vars

MainNext ==
    \/ \E s \in Slots : Write(s) \/ Commit(s)
    \/ HandOff
    \/ \E r \in Rings : Reopen(r)
    \/ Expose \/ Return \/ Stop \/ Join \/ Free

IoNext ==
    \/ \E r \in Rings, s \in Slots : IoSend(r, s)
    \/ \E r \in Rings : IoFinishRing(r)
    \/ IoReceive \/ IoPublish \/ IoOwe \/ IoWait \/ IoWake \/ IoClearWake
    \/ IoExit

Next == MainNext \/ IoNext \/ Arrive \/ Done

Spec == Init /\ [][Next]_vars

\* Weak fairness of the I/O thread's actions, the join, and update's two
\* steps (the game calls update every frame while the transport runs).
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

\* while the transport runs, received datagrams in the I/O thread's buffer
\* always have a next step toward update: a publish, an expose, a return,
\* or a wake of the blocked I/O thread
NoReceiveStall ==
    (phase = "Running" /\ count[FillBuffer] > 0) =>
        ENABLED (IoPublish \/ Expose \/ Return \/ IoWake)

HandedOffEventuallySent ==
    \A r \in Rings : ring[r] = "HandedOff" ~> ring[r] = "Sent"

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
