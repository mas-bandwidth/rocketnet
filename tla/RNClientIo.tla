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
(* it and only then frees the transport.  destroy makes the last hand-off *)
(* first; when the other ring does not reach Sent it waits (destroyWait)  *)
(* on the transport clock (now) and, at Deadline,                         *)
(* drops the committed slots it cannot hand off (Timeout), then stops,    *)
(* joins and frees.  The deadline bounds that wait, not the join: a       *)
(* driver stuck inside send keeps the thread from its stop check, so the  *)
(* model assumes a live I/O thread reaches IoExit (see the report).       *)
(*                                                                         *)
(* StepDriven is the step-driven mode: no I/O thread is started, and the  *)
(* caller's update and send_packets run the I/O step on their own thread  *)
(* (StepBegin, CallerStep, StepEnd), never concurrently with the rest of   *)
(* the main thread and never blocking.                                     *)
(*                                                                         *)
(* Destinations.  The hand-off passes the transport's server address,    *)
(* and the first attempt that finds committed slots binds the Filling     *)
(* ring's destination; a deferred ring keeps it. Disconnect seals its     *)
(* burst for the server address, resets the protocol (which zeroes the    *)
(* server address) and attempts a hand-off with the address it saved;     *)
(* every later hand-off, destroy's included, passes the zeroed one.       *)
(* DestinationOverwrite is a defined switch, FALSE here, that a reversed  *)
(* witness replaces with TRUE: every attempt overwrites the destination.  *)
(*                                                                         *)
(* Six switches restore the defects the reversed witnesses must catch:    *)
(* Switches restore the defects the reversed witnesses must catch:       *)
(* ReopenBeforeSent lets the main thread reopen a ring the I/O thread is  *)
(* still sending; ReceiveIntoExposed lets the I/O thread write a          *)
(* datagram into the buffer update has exposed; SwapNeedsReady restores   *)
(* the one-step swap, where update returns its exposed buffer only in     *)
(* the same step that exposes a Ready one; HandOffWaits restores the      *)
(* hand-off that waits for the other ring to be Sent; FullWaitReadable    *)
(* restores the wait on readable with a full buffer, which returns at     *)
(* once while datagrams sit in the socket; StepModeStartsThread starts    *)
(* the I/O thread at create in the step-driven mode as well.  IoStall     *)
(* models a send of the ring that never completes (the ring stays Sending *)
(* for destroy's wait); Timeout bounds that wait at Deadline.             *)
(* SpecNoDeadline is the reversed witness: NextNoDeadline removes the      *)
(* Timeout action, so a stalled other ring leaves destroy waiting forever. *)
(* The deadline does not bound the join: a driver call stuck inside send  *)
(* never reaches the thread's stop check (docs/CLIENT-IO.md).             *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS NumSlots,            \* slots per send ring
          BufferCap,           \* datagrams one receive buffer holds
          MaxDatagrams,        \* datagrams that arrive in one behaviour
          ReopenBeforeSent,    \* defect: reopen a ring while it is Sending
          ReceiveIntoExposed,  \* defect: receive writes the exposed buffer
          SwapNeedsReady,      \* defect: update returns only when it exposes
          HandOffWaits,        \* defect: a hand-off waits for the other ring
          FullWaitReadable,    \* defect: a full buffer waits on readable too
          StepDriven,          \* caller runs non-blocking I/O steps; no thread
          StepModeStartsThread  \* defect: step mode also starts an I/O thread

\* defect: every hand-off attempt overwrites the Filling ring's destination,
\* the unconditional io->to[io->rings.fill] = *to; a reversed witness
\* replaces this definition (DestinationOverwrite <- ...)
DestinationOverwrite == FALSE

ASSUME NumSlots \in Nat \ {0} /\ BufferCap \in Nat \ {0}
ASSUME MaxDatagrams \in Nat
ASSUME /\ ReopenBeforeSent \in BOOLEAN /\ ReceiveIntoExposed \in BOOLEAN
       /\ SwapNeedsReady \in BOOLEAN /\ HandOffWaits \in BOOLEAN
       /\ FullWaitReadable \in BOOLEAN
       /\ StepDriven \in BOOLEAN /\ StepModeStartsThread \in BOOLEAN

\* destroy's wait bound, in clock steps (the code's two seconds on the
\* transport clock; the model needs only a small number to reach it)
Deadline == 1

Rings   == {"A", "B"}
Slots   == 1..NumSlots
Buffers == {0, 1}
Other(r) == IF r = "A" THEN "B" ELSE "A"

RingStates   == {"Filling", "HandedOff", "Sending", "Sent"}
Owners       == {"free", "game", "main", "io"}
BufferStates == {"Fill", "Ready", "Exposed", "Free"}
Phases       == {"Running", "Stopping", "Joined", "Freed"}
Addresses    == {"server", "zero"}   \* the dialed server, or the reset (zeroed) one

VARIABLES ring,      \* each send ring's state
          owner,     \* each slot's owner: free, a game thread, main (committed), io
          sent,      \* slots of a ring the I/O thread has sent
          fill,      \* the ring the game threads fill
          ioRing,    \* the ring the I/O thread is sending, or "none"
          deferred,  \* CLIENT_SEND_DEFERRED has moved (a hand-off deferred)
          mainWait,  \* the main thread is blocked in the hand-off's wait
          destroyWait, \* destroy is waiting for the other ring to be Sent
          now,       \* the transport clock, in steps, capped at Deadline
          destroying, \* destroy has begun: the last hand-off, Stop, Join, Free
          dropped,   \* committed slots destroy dropped at the deadline
          stalled,   \* rings whose in-flight send never completes
          spinCycle,      \* the wait spins at the deadline (reversed witness cycle)
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
          ioAlive,   \* the I/O thread has not exited (the I/O core, in step mode)
          inCall,    \* step mode: update or send_packets is running the I/O step
          lastExec,  \* who ran the last I/O core action: none, thread or caller
          addr,      \* protocol.server_address: "zero" once disconnect has reset it
          dest,      \* io->to: each ring's destination
          held,      \* io->to_held: the Filling ring's destination is bound
          commitTo   \* the address each slot was committed for, or "none"

destVars == <<addr, dest, held, commitTo>>
sendVars == <<ring, owner, sent, fill, ioRing, deferred, mainWait, addr, dest, held, commitTo>>
recvVars == <<pending, arrived, buf, count, snap, owed, delivered>>
destroyVars == <<destroyWait, now, destroying, dropped, stalled, spinCycle>>

vars == <<ring, owner, sent, fill, ioRing, deferred, mainWait, destroyWait,
          now, destroying, dropped, stalled, spinCycle, wake, waiting, waitReadable,
          pending, arrived, buf, count, snap, owed, delivered, phase, ioAlive, inCall,
          lastExec, addr, dest, held, commitTo>>

TypeOK ==
    /\ ring \in [Rings -> RingStates]
    /\ owner \in [Rings -> [Slots -> Owners]]
    /\ sent \in [Rings -> SUBSET Slots]
    /\ fill \in Rings
    /\ ioRing \in Rings \cup {"none"}
    /\ deferred \in BOOLEAN /\ mainWait \in BOOLEAN
    /\ destroyWait \in BOOLEAN /\ now \in 0..Deadline
    /\ destroying \in BOOLEAN /\ dropped \in SUBSET Slots
    /\ stalled \in SUBSET Rings /\ spinCycle \in BOOLEAN
    /\ wake \in BOOLEAN /\ waiting \in BOOLEAN /\ waitReadable \in BOOLEAN
    /\ pending \in 0..MaxDatagrams /\ arrived \in 0..MaxDatagrams
    /\ buf \in [Buffers -> BufferStates]
    /\ count \in [Buffers -> 0..BufferCap]
    /\ snap \in 0..BufferCap
    /\ owed \in BOOLEAN
    /\ delivered \in 0..MaxDatagrams
    /\ phase \in Phases
    /\ ioAlive \in BOOLEAN
    /\ inCall \in BOOLEAN
    /\ lastExec \in {"none", "thread", "caller"}
    /\ addr \in Addresses
    /\ dest \in [Rings -> Addresses]
    /\ held \in [Rings -> BOOLEAN]
    /\ commitTo \in [Rings -> [Slots -> Addresses \cup {"none"}]]

Init ==
    /\ ring = [r \in Rings |-> "Filling"]
    /\ owner = [r \in Rings |-> [s \in Slots |-> "free"]]
    /\ sent = [r \in Rings |-> {}]
    /\ fill = "A"
    /\ ioRing = "none"
    /\ deferred = FALSE /\ mainWait = FALSE
    /\ destroyWait = FALSE /\ now = 0
    /\ destroying = FALSE /\ dropped = {} /\ stalled = {} /\ spinCycle = FALSE
    /\ wake = FALSE /\ waiting = FALSE /\ waitReadable = TRUE
    /\ pending = 0 /\ arrived = 0
    /\ buf = [b \in Buffers |-> IF b = 0 THEN "Fill" ELSE "Free"]
    /\ count = [b \in Buffers |-> 0]
    /\ snap = 0
    /\ owed = FALSE
    /\ delivered = 0
    /\ phase = "Running"
    /\ ioAlive = TRUE
    /\ inCall = FALSE
    /\ lastExec = "none"
    /\ addr = "server"
    /\ dest = [r \in Rings |-> "zero"]
    /\ held = [r \in Rings |-> FALSE]
    /\ commitTo = [r \in Rings |-> [s \in Slots |-> "none"]]

FillBuffer    == CHOOSE b \in Buffers : buf[b] = "Fill"
HasExposed    == \E b \in Buffers : buf[b] = "Exposed"
ExposedBuffer == CHOOSE b \in Buffers : buf[b] = "Exposed"
HasReady      == \E b \in Buffers : buf[b] = "Ready"
HasFree       == \E b \in Buffers : buf[b] = "Free"

\* the main thread is running its own code, not blocked on the I/O thread
MainFree == phase = "Running" /\ ~mainWait /\ ~destroyWait

\* a hand-off is due: fills joined (the phase contract) and slots committed
HandOffDue ==
    /\ ring[fill] = "Filling"
    /\ \A s \in Slots : owner[fill][s] # "game"
    /\ \E s \in Slots : owner[fill][s] = "main"

(* ---- game threads and the main thread ---- *)

\* start_packet: a game thread takes a free slot of the Filling ring
Write(s) ==
    /\ phase = "Running"
    /\ ~destroying
    /\ addr = "server"
    /\ ring[fill] = "Filling"
    /\ owner[fill][s] = "free"
    /\ owner' = [owner EXCEPT ![fill][s] = "game"]
    /\ UNCHANGED <<ring, sent, fill, ioRing, deferred, mainWait, wake,
                   waiting, waitReadable, phase, ioAlive>>
    /\ UNCHANGED destVars
    /\ UNCHANGED recvVars
    /\ UNCHANGED destroyVars

\* finish_packet: the slot is sealed and committed
Commit(s) ==
    /\ owner[fill][s] = "game"
    /\ owner' = [owner EXCEPT ![fill][s] = "main"]
    /\ commitTo' = [commitTo EXCEPT ![fill][s] = addr]
    /\ UNCHANGED <<ring, sent, fill, ioRing, deferred, mainWait, wake,
                   waiting, waitReadable, phase, ioAlive>>
    /\ UNCHANGED <<addr, dest, held>>
    /\ UNCHANGED recvVars
    /\ UNCHANGED destroyVars

\* the destination a hand-off attempt leaves on the Filling ring: the one
\* bound by an earlier, deferred attempt, else the address it is passed
\* (rocketnet_client_io_hand_off); under DestinationOverwrite always the
\* address passed
BoundDest(to) ==
    IF held[fill] /\ ~DestinationOverwrite THEN dest[fill] ELSE to

\* the hand-off (send_packets, send_packet through it, or the end of
\* update) when the other ring is open: the committed ring goes to the I/O
\* thread with one release store and one wake
HandOffTo(to) ==
    /\ MainFree
    /\ HandOffDue
    /\ ring[Other(fill)] = "Filling"
    /\ ring' = [ring EXCEPT ![fill] = "HandedOff"]
    /\ owner' = [owner EXCEPT ![fill] =
                    [s \in Slots |-> IF owner[fill][s] = "main" THEN "io"
                                     ELSE owner[fill][s]]]
    /\ fill' = Other(fill)
    /\ wake' = TRUE
    /\ dest' = [dest EXCEPT ![fill] = BoundDest(to)]
    /\ held' = [held EXCEPT ![fill] = FALSE]
    /\ UNCHANGED <<sent, ioRing, deferred, mainWait, waiting, waitReadable,
                   phase, ioAlive, addr, commitTo>>
    /\ UNCHANGED recvVars
    /\ UNCHANGED destroyVars

\* send_packets, send_packet, update and destroy pass the transport's
\* server address (sdk/rocketnet_client_transport.cpp:1086 and :1245)
HandOff == HandOffTo(addr)

\* the hand-off when the other ring is still HandedOff or Sending: the
\* committed slots stay in the Filling ring and CLIENT_SEND_DEFERRED moves;
\* they leave with the next hand-off.  Under HandOffWaits the main thread
\* blocks on the I/O thread instead.
DeferTo(to) ==
    /\ MainFree
    /\ ~destroying
    /\ HandOffDue
    /\ ring[Other(fill)] \in {"HandedOff", "Sending"}
    /\ IF HandOffWaits THEN /\ mainWait' = TRUE
                            /\ UNCHANGED deferred
                       ELSE /\ deferred' = TRUE
                            /\ UNCHANGED mainWait
    /\ dest' = [dest EXCEPT ![fill] = BoundDest(to)]
    /\ held' = [held EXCEPT ![fill] = TRUE]
    /\ UNCHANGED <<ring, owner, sent, fill, ioRing, wake, waiting,
                   waitReadable, phase, ioAlive, addr, commitTo>>
    /\ UNCHANGED recvVars
    /\ UNCHANGED destroyVars

Defer == DeferTo(addr)

(* rocketnet_client_transport_disconnect: with the fills joined, the burst
   is sealed for the server address, the protocol reset zeroes the server
   address, the burst is committed into the Filling ring (one slot stands
   for the burst), and a hand-off is attempted with the address saved before
   reset. Once the address is zero no game thread writes again. *)
Disconnect(s) ==
    /\ MainFree
    /\ ~destroying
    /\ addr = "server"
    /\ ring[fill] = "Filling"
    /\ \A t \in Slots : owner[fill][t] # "game"
    /\ owner[fill][s] = "free"
    /\ ring[Other(fill)] # "Sent"
    /\ addr' = "zero"
    /\ commitTo' = [commitTo EXCEPT ![fill][s] = "server"]
    /\ \/ /\ ring[Other(fill)] = "Filling"
          /\ ring' = [ring EXCEPT ![fill] = "HandedOff"]
          /\ owner' = [owner EXCEPT ![fill] =
                          [t \in Slots |-> IF t = s \/ owner[fill][t] = "main" THEN "io"
                                           ELSE owner[fill][t]]]
          /\ fill' = Other(fill)
          /\ wake' = TRUE
          /\ dest' = [dest EXCEPT ![fill] = BoundDest("server")]
          /\ held' = [held EXCEPT ![fill] = FALSE]
          /\ UNCHANGED <<deferred, mainWait>>
       \/ /\ ring[Other(fill)] \in {"HandedOff", "Sending"}
          /\ owner' = [owner EXCEPT ![fill][s] = "main"]
          /\ IF HandOffWaits THEN /\ mainWait' = TRUE
                                  /\ UNCHANGED deferred
                             ELSE /\ deferred' = TRUE
                                  /\ UNCHANGED mainWait
          /\ dest' = [dest EXCEPT ![fill] = BoundDest("server")]
          /\ held' = [held EXCEPT ![fill] = TRUE]
          /\ UNCHANGED <<ring, fill, wake>>
    /\ UNCHANGED <<sent, ioRing, waiting, waitReadable, phase, ioAlive>>
    /\ UNCHANGED recvVars
    /\ UNCHANGED destroyVars

\* the defect's blocked main thread resumes once the other ring is Sent
WaitDone ==
    /\ mainWait
    /\ ring[Other(fill)] = "Sent"
    /\ mainWait' = FALSE
    /\ UNCHANGED <<ring, owner, sent, fill, ioRing, deferred, wake, waiting,
                   waitReadable, phase, ioAlive>>
    /\ UNCHANGED destVars
    /\ UNCHANGED recvVars
    /\ UNCHANGED destroyVars

\* a hand-off reopens a ring the I/O thread has finished (acquire load)
Reopen(r) ==
    /\ ~mainWait
    /\ ~destroyWait
    /\ r # fill
    /\ IF ReopenBeforeSent THEN ring[r] \in {"Sending", "Sent"}
                           ELSE ring[r] = "Sent"
    /\ ring' = [ring EXCEPT ![r] = "Filling"]
    /\ owner' = [owner EXCEPT ![r] = [s \in Slots |-> "free"]]
    /\ sent' = [sent EXCEPT ![r] = {}]
    /\ commitTo' = [commitTo EXCEPT ![r] = [s \in Slots |-> "none"]]
    /\ UNCHANGED <<fill, ioRing, deferred, mainWait, wake, waiting,
                   waitReadable, phase, ioAlive>>
    /\ UNCHANGED <<addr, dest, held>>
    /\ UNCHANGED recvVars
    /\ UNCHANGED destroyVars

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
    /\ UNCHANGED destroyVars

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
    /\ UNCHANGED destroyVars

\* destroy begins: it makes the last hand-off itself (DestroyWait when the
\* other ring is not open), then Stop, Join, Free
DestroyBegin ==
    /\ phase = "Running"
    /\ ~destroying
    /\ destroying' = TRUE
    /\ UNCHANGED <<ring, owner, sent, fill, ioRing, deferred, mainWait,
                   destroyWait, now, dropped, stalled, spinCycle, wake, waiting,
                   waitReadable, phase, ioAlive>>
    /\ UNCHANGED recvVars
    /\ UNCHANGED destVars

\* destroy's last hand-off: the Filling ring holds committed slots and the
\* other ring is still HandedOff or Sending, so destroy waits for it to be
\* Sent (code: the while loop at sdk/rocketnet_client_transport.cpp)
DestroyWait ==
    /\ MainFree
    /\ destroying
    /\ phase = "Running"
    /\ HandOffDue
    /\ ring[Other(fill)] \in {"HandedOff", "Sending"}
    /\ destroyWait' = TRUE
    /\ UNCHANGED <<ring, owner, sent, fill, ioRing, deferred, mainWait,
                   now, destroying, dropped, stalled, spinCycle, wake, waiting,
                   waitReadable, phase, ioAlive>>
    /\ UNCHANGED recvVars
    /\ UNCHANGED destVars

\* the wait ends on its own: the other ring reached Sent
DestroyWaitDone ==
    /\ destroyWait
    /\ ring[Other(fill)] = "Sent"
    /\ destroyWait' = FALSE
    /\ UNCHANGED <<ring, owner, sent, fill, ioRing, deferred, mainWait,
                   now, destroying, dropped, stalled, spinCycle, wake, waiting,
                   waitReadable, phase, ioAlive>>
    /\ UNCHANGED recvVars
    /\ UNCHANGED destVars

\* the wait's deadline: at Deadline, with the fix, destroy gives up on the
\* committed slots of the Filling ring, drops them (counted in dropped) and
\* continues to Stop, Join, Free (code: the time_callback check and the
\* SEND_RING_FULL add)
Timeout ==
    /\ destroying
    /\ destroyWait
    /\ now = Deadline
    /\ owner' = [r \in Rings |-> [s \in Slots |->
                    IF r = fill /\ owner[r][s] = "main" THEN "free"
                    ELSE owner[r][s]]]
    /\ dropped' = dropped \cup {s \in Slots : owner[fill][s] = "main"}
    /\ destroyWait' = FALSE
    /\ UNCHANGED <<ring, sent, fill, ioRing, deferred, mainWait, now,
                   destroying, stalled, spinCycle, wake, waiting,
                   waitReadable, phase, ioAlive>>
    /\ UNCHANGED recvVars
    /\ UNCHANGED destVars

\* destroy's wait reads the transport clock again: the clock advances to
\* the deadline and stops there (the loop's time_callback calls)
WaitTick ==
    /\ destroyWait
    /\ now < Deadline
    /\ now' = now + 1
    /\ UNCHANGED <<ring, owner, sent, fill, ioRing, deferred, mainWait,
                   destroyWait, destroying, dropped, stalled, spinCycle, wake,
                   waiting, waitReadable, phase, ioAlive>>
    /\ UNCHANGED recvVars
    /\ UNCHANGED destVars

\* destroy, first half: once the Filling ring holds no committed slot,
\* ask the I/O thread to stop and wake it
Stop ==
    /\ MainFree
    /\ destroying
    /\ \A s \in Slots : owner[fill][s] = "free"
    /\ phase' = "Stopping"
    /\ wake' = TRUE
    /\ UNCHANGED sendVars
    /\ UNCHANGED <<waiting, waitReadable, ioAlive>>
    /\ UNCHANGED <<pending, arrived, buf, count, snap, owed, delivered,
                   destroyWait, now, destroying, dropped, stalled, spinCycle>>

Join ==
    /\ phase = "Stopping"
    /\ ~ioAlive
    /\ phase' = "Joined"
    /\ UNCHANGED sendVars
    /\ UNCHANGED <<wake, waiting, waitReadable, ioAlive>>
    /\ UNCHANGED <<pending, arrived, buf, count, snap, owed, delivered,
                   destroyWait, now, destroying, dropped, stalled, spinCycle>>

Free ==
    /\ phase = "Joined"
    /\ phase' = "Freed"
    /\ UNCHANGED sendVars
    /\ UNCHANGED <<wake, waiting, waitReadable, ioAlive>>
    /\ UNCHANGED <<pending, arrived, buf, count, snap, owed, delivered,
                   destroyWait, now, destroying, dropped, stalled, spinCycle>>

(* ---- the network ---- *)

Arrive ==
    /\ arrived < MaxDatagrams
    /\ arrived' = arrived + 1
    /\ pending' = pending + 1
    /\ UNCHANGED sendVars
    /\ UNCHANGED <<wake, waiting, waitReadable, buf, count, snap, owed,
                   delivered, phase, ioAlive>>
    /\ UNCHANGED destroyVars

(* ---- the I/O core ---- *)

\* The core actions below are the body of rocketnet_client_io_step.  In
\* threaded mode the I/O thread runs them (IoThreadNext); in step-driven
\* mode the caller runs them inside update and send_packets (CallerStep),
\* and IoWait, the blocking wait, is never taken.  Create starts the I/O
\* thread unless the mode is step-driven; StepModeStartsThread is the
\* defect that starts it in both modes.
ThreadStarted == ~StepDriven \/ StepModeStartsThread

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
    /\ UNCHANGED destVars
    /\ UNCHANGED recvVars
    /\ UNCHANGED destroyVars

\* every slot sent: the ring is released to the main thread (release store)
IoFinishRing(r) ==
    /\ IoReady
    /\ ioRing = r
    /\ \A s \in Slots : owner[r][s] = "io" => s \in sent[r]
    /\ ring' = [ring EXCEPT ![r] = "Sent"]
    /\ ioRing' = "none"
    /\ UNCHANGED <<owner, sent, fill, deferred, mainWait, wake, waiting,
                   waitReadable, phase, ioAlive>>
    /\ UNCHANGED destVars
    /\ UNCHANGED recvVars
    /\ UNCHANGED destroyVars

(* The I/O thread's send can stall: the ring stays Sending (the state
   destroy's wait observes) and no slot leaves it. The modeled thread is
   still live and can reach IoExit after Stop. A driver blocked inside send
   lies outside this model and leaves the threaded join unbounded. *)
IoStall(r) ==
    /\ IoReady
    /\ destroying
    /\ ioRing = r
    /\ r \notin stalled
    /\ stalled' = stalled \cup {r}
    /\ ioRing' = "none"
    /\ UNCHANGED <<ring, owner, sent, fill, deferred, mainWait, destroyWait,
                   now, destroying, dropped, spinCycle, wake, waiting, waitReadable,
                   pending, arrived, buf, count, snap, owed, delivered,
                   phase, ioAlive, addr, dest, held, commitTo>>

\* read one datagram into the I/O thread's buffer while it has room. Every
\* arriving datagram becomes a record here, regardless of size, as the code
\* does (sdk/rocketnet_client_io.h rocketnet_client_io_step, step 2, has no
\* size branch); the main thread's prefilter (sdk/rocketnet_client_protocol.cpp:1248)
\* refuses and counts runts (PREFILTER_TOO_SHORT) and oversize datagrams
\* over the receive ceiling (PREFILTER_TOO_LARGE), so the model has no size
\* branch either.
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
    /\ UNCHANGED destroyVars

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
    /\ UNCHANGED destroyVars

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
    /\ UNCHANGED destroyVars

\* nothing to do: block.  The thread never blocks while it could publish,
\* blocks with an unpublished batch only after it has recorded the publish
\* as owed, and blocks with datagrams in the socket only when its buffer is
\* full.  The wait returns on readable only while the buffer has room
\* (wait's readable argument); a full buffer waits on the wake alone.
IoWait ==
    /\ ~StepDriven
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
    /\ UNCHANGED <<pending, arrived, buf, count, snap, owed, delivered,
                   destroyWait, now, destroying, dropped, stalled, spinCycle>>

IoWake ==
    /\ ioAlive /\ waiting
    /\ wake \/ (waitReadable /\ pending > 0)
    /\ waiting' = FALSE
    /\ wake' = FALSE
    /\ UNCHANGED sendVars
    /\ UNCHANGED <<waitReadable, phase, ioAlive>>
    /\ UNCHANGED <<pending, arrived, buf, count, snap, owed, delivered,
                   destroyWait, now, destroying, dropped, stalled, spinCycle>>

\* a woken I/O thread consumes a wake it does not need to block on
IoClearWake ==
    /\ IoReady
    /\ wake
    /\ wake' = FALSE
    /\ UNCHANGED sendVars
    /\ UNCHANGED <<waiting, waitReadable, phase, ioAlive>>
    /\ UNCHANGED <<pending, arrived, buf, count, snap, owed, delivered,
                   destroyWait, now, destroying, dropped, stalled, spinCycle>>

\* stop seen: the ring in flight is finished first, then the thread exits
IoExit ==
    /\ IoReady
    /\ phase = "Stopping"
    /\ ioRing = "none"
    /\ \A r \in Rings : ring[r] # "HandedOff"
    /\ ioAlive' = FALSE
    /\ UNCHANGED sendVars
    /\ UNCHANGED <<wake, waiting, waitReadable, phase>>
    /\ UNCHANGED <<pending, arrived, buf, count, snap, owed, delivered,
                   destroyWait, now, destroying, dropped, stalled, spinCycle>>

Done ==
    /\ phase = "Freed"
    /\ UNCHANGED vars

\* step mode: update or send_packets (or destroy, while it waits for the
\* other ring to be Sent and after Stop) begins the I/O step on the
\* caller's thread; the main thread runs nothing else until the step ends
StepBegin ==
    /\ StepDriven
    /\ ~inCall
    /\ ioAlive
    /\ ~mainWait
    /\ phase \in {"Running", "Stopping"}
    \* while destroy runs, the caller steps only for the wait (the code
    \* steps the core in its wait loop) and after Stop (the exit loop);
    \* otherwise the destroy actions run without being starved
    /\ (~destroying \/ destroyWait \/ phase = "Stopping")
    \* a destroy wait or the post-Stop exit steps only when there is I/O
    \* work, so an empty StepBegin/StepEnd cycle cannot starve the wait's
    \* check of Sent and of the deadline
    /\ (destroyWait \/ phase = "Stopping" =>
           ENABLED (\/ \E r \in Rings, s \in Slots : IoSend(r, s)
                    \/ \E r \in Rings : IoFinishRing(r)
                    \/ IoPublish \/ IoOwe \/ IoExit))
    /\ inCall' = TRUE
    /\ UNCHANGED <<ring, owner, sent, fill, ioRing, deferred, mainWait, wake,
                   waiting, waitReadable, phase, ioAlive, lastExec>>
    /\ UNCHANGED destVars
    /\ UNCHANGED recvVars
    /\ UNCHANGED destroyVars

\* the step is one pass that never blocks (rocketnet_client_io_step with
\* block false): it finishes the ring it takes, exits once stopped, and
\* ends its drain with a publish or an owe.  It may return with datagrams
\* still in the socket (a drain stops at a publish, and the code drains
\* only into the buffer it holds), so receive work does not hold it, and
\* the wake, which no step mode wait reads, is consumed or left.
StepWork ==
    \/ \E r \in Rings, s \in Slots : IoSend(r, s)
    \/ \E r \in Rings : IoFinishRing(r)
    \/ IoPublish \/ IoOwe \/ IoExit

StepEnd ==
    /\ inCall
    /\ ~ENABLED StepWork
    /\ inCall' = FALSE
    /\ UNCHANGED <<ring, owner, sent, fill, ioRing, deferred, mainWait, wake,
                   waiting, waitReadable, phase, ioAlive, lastExec>>
    /\ UNCHANGED destVars
    /\ UNCHANGED recvVars
    /\ UNCHANGED destroyVars

\* game threads write and commit at any time; the main thread's own
\* actions never overlap a caller step
OnGame(A) == A /\ UNCHANGED <<inCall, lastExec>>
OnMain(A) == ~inCall /\ A /\ UNCHANGED <<inCall, lastExec>>

\* destroy's last hand-off wait at the deadline with a stalled other ring is
\* the loop's spin: it alternates `spinCycle` so the reversed witness has an
\* infinite behaviour instead of a deadlock.  The fixed model also has
\* Timeout enabled there (its fair exit); the reversed witness removes that
\* action, so the spin cycle is all that is left.
Spin ==
    /\ destroying /\ destroyWait
    /\ now = Deadline
    /\ Other(fill) \in stalled
    /\ spinCycle' = ~spinCycle
    /\ UNCHANGED <<ring, owner, sent, fill, ioRing, deferred, mainWait,
                   destroyWait, now, destroying, dropped, stalled, wake,
                   waiting, waitReadable, pending, arrived, buf, count, snap,
                   owed, delivered, phase, ioAlive, inCall, lastExec>>
    /\ UNCHANGED destVars

MainNext ==
    \/ OnGame(\E s \in Slots : Write(s) \/ Commit(s))
    \/ OnMain(HandOff \/ Defer \/ WaitDone)
    \/ OnMain(\E s \in Slots : Disconnect(s))
    \/ OnMain(\E r \in Rings : Reopen(r))
    \/ OnMain(Expose \/ Return)
    \/ OnMain(DestroyBegin \/ DestroyWait \/ DestroyWaitDone \/ Timeout)
    \/ OnMain(Stop \/ Join \/ Free)
    \/ StepBegin \/ StepEnd

\* The reversed witness's next-state relation: the deadline action is absent,
\* not merely unfair.  Destroy's wait still observes a Sent ring
\* (DestroyWaitDone) and the clock still advances (WaitTick); with the ring
\* stalled there is no exit, and destroy never reaches Stop, Join or Free.
MainNextNoDeadline ==
    \/ OnGame(\E s \in Slots : Write(s) \/ Commit(s))
    \/ OnMain(HandOff \/ Defer \/ WaitDone)
    \/ OnMain(\E s \in Slots : Disconnect(s))
    \/ OnMain(\E r \in Rings : Reopen(r))
    \/ OnMain(Expose \/ Return)
    \/ OnMain(DestroyBegin \/ DestroyWait \/ DestroyWaitDone)
    \/ OnMain(Stop \/ Join \/ Free)
    \/ StepBegin \/ StepEnd

IoCoreNext ==
    \/ \E r \in Rings, s \in Slots : IoSend(r, s)
    \/ \E r \in Rings : IoFinishRing(r)
    \/ \E r \in Rings : IoStall(r)
    \/ IoReceive \/ IoPublish \/ IoOwe \/ IoWait \/ IoWake \/ IoClearWake
    \/ IoExit

\* the two executors of a core action A: the I/O thread, when create
\* started one, and the caller inside update or send_packets in step mode
\* (lastExec is recorded only in step mode, where the invariant reads it)
OnThread(A) == ThreadStarted /\ A /\ UNCHANGED inCall
               /\ lastExec' = IF StepDriven THEN "thread" ELSE lastExec
OnCaller(A) == StepDriven /\ inCall /\ A /\ lastExec' = "caller" /\ UNCHANGED inCall
Exec(A) == OnThread(A) \/ OnCaller(A)

IoThreadNext == OnThread(IoCoreNext)
CallerStep == OnCaller(IoCoreNext)
IoNext == IoThreadNext \/ CallerStep

Next == MainNext \/ IoNext
        \/ (Arrive /\ UNCHANGED <<inCall, lastExec>>)
        \/ OnMain(WaitTick)
        \/ (Done /\ UNCHANGED <<inCall, lastExec>>)

NextNoDeadline == MainNextNoDeadline \/ IoNext
        \/ (Arrive /\ UNCHANGED <<inCall, lastExec>>)
        \/ OnMain(WaitTick)
        \/ (Done /\ UNCHANGED <<inCall, lastExec>>)
        \/ Spin

Spec == Init /\ [][Next]_vars

\* Weak fairness of the I/O thread's actions, the join, a game thread's
\* finish of a packet it started, the reopen, the hand-off and update's
\* two receive steps: the game calls update every frame while the
\* transport runs, and update ends with a hand-off, so a deferred hand-off
\* is retried every frame.  A game thread mid-write disables the hand-off
\* only until it commits, and a ring has NumSlots slots, so weak fairness
\* is enough.  In step mode the I/O core runs only inside a caller step:
\* the game calls update every frame (StepBegin), and a step finishes its
\* ring, its exit and its publish or owe before it returns (StepEnd).
Fairness ==
    /\ WF_vars(Exec(\E r \in Rings, s \in Slots : IoSend(r, s)))
    /\ WF_vars(Exec(\E r \in Rings : IoFinishRing(r)))
    /\ WF_vars(Exec(IoReceive))
    /\ WF_vars(Exec(IoPublish))
    /\ WF_vars(Exec(IoOwe))
    /\ WF_vars(Exec(IoWake))
    /\ WF_vars(Exec(IoExit))
    /\ WF_vars(StepBegin)
    /\ WF_vars(StepEnd)
    /\ WF_vars(OnMain(Join))
    /\ WF_vars(OnGame(\E s \in Slots : Commit(s)))
    /\ WF_vars(OnMain(\E r \in Rings : Reopen(r)))
    /\ WF_vars(OnMain(HandOff))
    /\ WF_vars(OnMain(Expose))
    /\ WF_vars(OnMain(Return))
    /\ WF_vars(OnMain(DestroyWait))
    /\ WF_vars(OnMain(Stop))
    /\ WF_vars(OnMain(Free))
    /\ WF_vars(OnMain(WaitTick))

SpecCommon ==
    /\ Spec
    /\ Fairness
    /\ WF_vars(OnMain(DestroyWaitDone))

\* the committed run also assumes the deadline fires (Timeout).  Without
\* it destroy's last hand-off may wait forever.
SpecFair ==
    /\ SpecCommon
    /\ WF_vars(OnMain(Timeout))

\* The reversed witness keeps every progress assumption above, including
\* the fairness of DestroyWaitDone; only the deadline is gone: Timeout is
\* absent from the next-state relation (NextNoDeadline) and, because the
\* broken code has no deadline action at all, it asks no fairness of it.
\* With a stalled ring the wait has no exit however the scheduler behaves,
\* so DestroyCompletes falls.  This is the wait the fix bounds (code: the
\* while loop and the time_callback deadline at
\* sdk/rocketnet_client_transport.cpp).
SpecNoDeadline ==
    /\ Init /\ [][NextNoDeadline]_vars
    /\ Fairness
    /\ WF_vars(OnMain(DestroyWaitDone))

\* Step mode: the main thread's own actions are disabled while it runs a
\* step, so they are enabled only intermittently.  update makes its two
\* receive steps and a hand-off every frame and send_packets hands off
\* before its step, so they are strongly fair here; a step may return
\* with datagrams in the socket, and every step drains, so receive is too.
SpecStepFair ==
    /\ SpecFair
    /\ SF_vars(OnMain(\E r \in Rings : Reopen(r)))
    /\ SF_vars(OnMain(HandOff))
    /\ SF_vars(OnMain(Expose))
    /\ SF_vars(OnMain(Return))
    /\ SF_vars(OnMain(Join))
    /\ SF_vars(Exec(IoReceive))

(* ---- what the design must keep ---- *)

\* a slot of a ring is never written while the I/O thread owns that ring
NoReuseBeforeSent ==
    \A r \in Rings, s \in Slots :
        owner[r][s] = "game" => /\ ioRing # r
                                /\ ring[r] = "Filling"

\* every slot a ring sends goes to the destination it was committed with:
\* a deferred ring keeps the destination its first hand-off attempt bound,
\* and a later attempt passing the zeroed address does not overwrite it
SentToCommitted ==
    \A r \in Rings, s \in Slots : s \in sent[r] => dest[r] = commitTo[r][s]

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

\* step mode: no I/O-thread action is enabled, and every I/O core action
\* taken so far was taken by the caller, inside update or send_packets
NoIoThreadActionInStepMode ==
    StepDriven => /\ ~ENABLED IoThreadNext
                  /\ lastExec # "thread"

\* while the transport runs and destroy has not begun, received datagrams
\* in the I/O thread's buffer always have a next step toward update: a
\* publish, an expose, a return, or a wake of the blocked I/O thread.
\* destroy blocks the main thread in its last hand-off's wait, so update no
\* longer runs and the receive handshake has no step until destroy ends.
NoReceiveStall ==
    (phase = "Running" /\ ~destroying /\ count[FillBuffer] > 0) =>
        ENABLED (IoPublish \/ Expose \/ Return \/ IoWake)

\* a handed-off ring is eventually Sent, unless its in-flight send stalls
HandedOffEventuallySent ==
    \A r \in Rings : ring[r] = "HandedOff" ~> (ring[r] = "Sent" \/ r \in stalled)

\* a committed slot, deferred or not, is eventually handed to the I/O
\* thread, or dropped by destroy at the deadline and counted there
CommittedEventuallyHandedOff ==
    \A r \in Rings, s \in Slots :
        owner[r][s] = "main" ~> (owner[r][s] = "io" \/ s \in dropped)

StopEventuallyJoins ==
    phase = "Stopping" ~> phase \in {"Joined", "Freed"}

\* once destroy has begun it finishes: Stop, Join, Free.  The deadline
\* ends the last hand-off's wait (Timeout); a live I/O thread then reaches
\* IoExit and the join returns.  This claims the wait, not the join: a
\* driver call blocked inside send never reaches its stop check, so the
\* threaded join stays unbounded in that case (docs/CLIENT-IO.md).  The
\* reversed witness removes Timeout (NextNoDeadline) and stalls the other
\* ring's send, so the wait never ends and DestroyCompletes falls.
DestroyCompletes ==
    destroying ~> phase = "Freed"

\* a committed slot destroy drops is dropped only at the deadline, with the
\* deadline in force, and only from the Filling ring
DroppedAtDeadline ==
    \A s \in dropped : now = Deadline

\* a buffer of received datagrams is eventually exposed to update
FilledEventuallyExposed ==
    \A b \in Buffers :
        (buf[b] = "Fill" /\ count[b] > 0) ~> (buf[b] = "Exposed" \/ phase # "Running")

\* every datagram that arrives eventually reaches update
ArrivedEventuallyDelivered ==
    \A n \in 1..MaxDatagrams :
        arrived >= n ~> (delivered >= n \/ phase # "Running")

=============================================================================
