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
(* StepDriven is the step-driven mode: no I/O thread is started, and the  *)
(* caller's update and send_packets run the I/O step on their own thread  *)
(* (StepBegin, CallerStep, StepEnd), never concurrently with the rest of   *)
(* the main thread and never blocking.                                     *)
(*                                                                         *)
(* Six switches restore the defects the reversed witnesses must catch:    *)
(* ReopenBeforeSent lets the main thread reopen a ring the I/O thread is  *)
(* still sending; ReceiveIntoExposed lets the I/O thread write a          *)
(* datagram into the buffer update has exposed; SwapNeedsReady restores   *)
(* the one-step swap, where update returns its exposed buffer only in     *)
(* the same step that exposes a Ready one; HandOffWaits restores the      *)
(* hand-off that waits for the other ring to be Sent; FullWaitReadable    *)
(* restores the wait on readable with a full buffer, which returns at     *)
(* once while datagrams sit in the socket; StepModeStartsThread starts    *)
(* the I/O thread at create in the step-driven mode as well.              *)
(*                                                                         *)
(* The overlapped send (rocketnet-windows-client-overlapped-send): each    *)
(* ring's slot sends are posted in one step (IoSend: the code's post loop  *)
(* touches only slots the I/O thread owns, so no other action can observe  *)
(* it half done) and each is completed later, ok or with an error,         *)
(* by an outside event, the kernel (Complete).  A ring becomes Sent only   *)
(* when every posted send of it has completed (IoFinishRing), and while    *)
(* sends are outstanding the I/O thread receives, publishes and waits; the *)
(* ring's last completion ends that wait (IoWake).  An error completion    *)
(* releases its slot exactly as an ok one does, so one action models both. *)
(* Two defined switches, FALSE                                             *)
(* here and overridden by their witness configs, restore the defects:      *)
(* BlockingSend holds the send in the I/O step until it completes, the     *)
(* synchronous sendto the kernel can hold for a second; SentBeforeComplete *)
(* marks the ring Sent once every slot is posted, before the completions.  *)
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
          StepModeStartsThread \* defect: step mode also starts an I/O thread

ASSUME NumSlots \in Nat \ {0} /\ BufferCap \in Nat \ {0}
ASSUME MaxDatagrams \in Nat
ASSUME /\ ReopenBeforeSent \in BOOLEAN /\ ReceiveIntoExposed \in BOOLEAN
       /\ SwapNeedsReady \in BOOLEAN /\ HandOffWaits \in BOOLEAN
       /\ FullWaitReadable \in BOOLEAN
       /\ StepDriven \in BOOLEAN /\ StepModeStartsThread \in BOOLEAN

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
          ioAlive,   \* the I/O thread has not exited (the I/O core, in step mode)
          inCall,    \* step mode: update or send_packets is running the I/O step
          lastExec,  \* who ran the last I/O core action: none, thread or caller
          posted,    \* slots of a ring whose send is posted and not yet completed
          inSend     \* the I/O thread is held inside a send call (BlockingSend)

\* the overlapped send's defect switches (see the header); a witness config
\* overrides one with CONSTANT BlockingSend <- MCTrue
BlockingSend == FALSE
SentBeforeComplete == FALSE

ovVars   == <<posted, inSend>>
sendVars == <<ring, owner, sent, fill, ioRing, deferred, mainWait, posted,
              inSend>>
recvVars == <<pending, arrived, buf, count, snap, owed, delivered>>

vars == <<ring, owner, sent, fill, ioRing, deferred, mainWait, wake, waiting,
          waitReadable, pending, arrived, buf, count, snap, owed, delivered,
          phase, ioAlive, inCall, lastExec, posted, inSend>>

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
    /\ inCall \in BOOLEAN
    /\ lastExec \in {"none", "thread", "caller"}
    /\ posted \in [Rings -> SUBSET Slots]
    /\ inSend \in BOOLEAN

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
    /\ inCall = FALSE
    /\ lastExec = "none"
    /\ posted = [r \in Rings |-> {}]
    /\ inSend = FALSE

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
    /\ UNCHANGED ovVars

\* finish_packet: the slot is sealed and committed
Commit(s) ==
    /\ owner[fill][s] = "game"
    /\ owner' = [owner EXCEPT ![fill][s] = "main"]
    /\ UNCHANGED <<ring, sent, fill, ioRing, deferred, mainWait, wake,
                   waiting, waitReadable, phase, ioAlive>>
    /\ UNCHANGED recvVars
    /\ UNCHANGED ovVars

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
    /\ UNCHANGED ovVars

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
    /\ UNCHANGED ovVars

\* the defect's blocked main thread resumes once the other ring is Sent
WaitDone ==
    /\ mainWait
    /\ ring[Other(fill)] = "Sent"
    /\ mainWait' = FALSE
    /\ UNCHANGED <<ring, owner, sent, fill, ioRing, deferred, wake, waiting,
                   waitReadable, phase, ioAlive>>
    /\ UNCHANGED recvVars
    /\ UNCHANGED ovVars

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
    /\ UNCHANGED ovVars

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

(* ---- the I/O core ---- *)

\* The core actions below are the body of rocketnet_client_io_step.  In
\* threaded mode the I/O thread runs them (IoThreadNext); in step-driven
\* mode the caller runs them inside update and send_packets (CallerStep),
\* and IoWait, the blocking wait, is never taken.  Create starts the I/O
\* thread unless the mode is step-driven; StepModeStartsThread is the
\* defect that starts it in both modes.
ThreadStarted == ~StepDriven \/ StepModeStartsThread

IoReady == ioAlive /\ ~waiting /\ ~inSend

\* every slot the I/O thread owns in ring r has its send posted or completed
AllPosted(r) == \A s \in Slots : owner[r][s] = "io" => s \in posted[r] \cup sent[r]

\* the ring may be released: every posted send has completed (under the
\* SentBeforeComplete defect, every slot posted is enough)
RingDone(r) ==
    IF SentBeforeComplete THEN AllPosted(r)
    ELSE \A s \in Slots : owner[r][s] = "io" => s \in sent[r]

\* take the handed-off ring and post every slot's send (WSASendTo with an
\* OVERLAPPED, one per slot, in the post loop of rocketnet_client_io_step);
\* the calls return at once and each send completes later (Complete).  Under
\* BlockingSend the I/O step is held until the sends complete.
IoSend(r) ==
    /\ IoReady
    /\ ioRing = "none"
    /\ ring[r] = "HandedOff"
    /\ ioRing' = r
    /\ ring' = [ring EXCEPT ![r] = "Sending"]
    /\ posted' = [posted EXCEPT ![r] = {s \in Slots : owner[r][s] = "io"}]
    /\ inSend' = BlockingSend
    /\ UNCHANGED <<owner, sent, fill, deferred, mainWait, wake, waiting,
                   waitReadable, phase, ioAlive>>
    /\ UNCHANGED recvVars

\* the kernel completes a posted send, ok or with an error; either way the
\* slot is done (an error moves send_errors in the code and releases the
\* slot the same way, so the model needs no second action).  One socket's
\* datagram sends complete in the order they were posted, so the oldest
\* posted slot (the lowest; IoSend posts in slot order) completes first.
Complete(r, s) ==
    /\ s \in posted[r]
    /\ \A t \in posted[r] : s <= t
    /\ posted' = [posted EXCEPT ![r] = posted[r] \ {s}]
    /\ sent' = [sent EXCEPT ![r] = sent[r] \cup {s}]
    /\ inSend' = (inSend /\ posted'[r] # {})
    /\ UNCHANGED <<ring, owner, fill, ioRing, deferred, mainWait, wake,
                   waiting, waitReadable, phase, ioAlive>>
    /\ UNCHANGED recvVars

\* every posted send completed: the ring is released to the main thread
\* (release store)
IoFinishRing(r) ==
    /\ IoReady
    /\ ioRing = r
    /\ RingDone(r)
    /\ ring' = [ring EXCEPT ![r] = "Sent"]
    /\ ioRing' = "none"
    /\ UNCHANGED <<owner, sent, fill, deferred, mainWait, wake, waiting,
                   waitReadable, phase, ioAlive, posted, inSend>>
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
    /\ ~StepDriven
    /\ IoReady
    /\ phase = "Running"
    /\ \/ ioRing = "none"
       \/ ioRing # "none" /\ AllPosted(ioRing) /\ posted[ioRing] # {}
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

\* the wait returns on the wake, on readable when it waits on it, or once
\* the posted ring's sends have completed (the OVERLAPPED event in the wait
\* set; an earlier completion also ends the wait, and the step then waits
\* again, which the model folds into this one return)
IoWake ==
    /\ ioAlive /\ waiting
    /\ \/ wake \/ (waitReadable /\ pending > 0)
       \/ ioRing # "none" /\ RingDone(ioRing)
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
    /\ \A r \in Rings : ring[r] # "HandedOff" /\ posted[r] = {}
    /\ ioAlive' = FALSE
    /\ UNCHANGED sendVars
    /\ UNCHANGED <<wake, waiting, waitReadable, phase>>
    /\ UNCHANGED recvVars

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
    /\ inCall' = TRUE
    /\ UNCHANGED <<ring, owner, sent, fill, ioRing, deferred, mainWait, wake,
                   waiting, waitReadable, phase, ioAlive, lastExec>>
    /\ UNCHANGED recvVars
    /\ UNCHANGED ovVars

\* the step is one pass that never blocks (rocketnet_client_io_step with
\* block false): it finishes the ring it takes, exits once stopped, and
\* ends its drain with a publish or an owe.  It may return with datagrams
\* still in the socket (a drain stops at a publish, and the code drains
\* only into the buffer it holds), so receive work does not hold it, and
\* the wake, which no step mode wait reads, is consumed or left.
StepWork ==
    \/ \E r \in Rings : IoSend(r)
    \/ \E r \in Rings : IoFinishRing(r)
    \/ IoPublish \/ IoOwe \/ IoExit

StepEnd ==
    /\ inCall
    /\ ~ENABLED StepWork
    /\ inCall' = FALSE
    /\ UNCHANGED <<ring, owner, sent, fill, ioRing, deferred, mainWait, wake,
                   waiting, waitReadable, phase, ioAlive, lastExec>>
    /\ UNCHANGED recvVars
    /\ UNCHANGED ovVars

\* game threads write and commit at any time; the main thread's own
\* actions never overlap a caller step
OnGame(A) == A /\ UNCHANGED <<inCall, lastExec>>
OnMain(A) == ~inCall /\ A /\ UNCHANGED <<inCall, lastExec>>

MainNext ==
    \/ OnGame(\E s \in Slots : Write(s) \/ Commit(s))
    \/ OnMain(HandOff \/ Defer \/ WaitDone)
    \/ OnMain(\E r \in Rings : Reopen(r))
    \/ OnMain(Expose \/ Return \/ Stop \/ Join \/ Free)
    \/ StepBegin \/ StepEnd

IoCoreNext ==
    \/ \E r \in Rings : IoSend(r)
    \/ \E r \in Rings : IoFinishRing(r)
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

\* the kernel, outside both threads
CompleteNext == \E r \in Rings, s \in Slots : Complete(r, s)

Next == MainNext \/ IoNext
        \/ (Arrive /\ UNCHANGED <<inCall, lastExec>>)
        \/ (CompleteNext /\ UNCHANGED <<inCall, lastExec>>)
        \/ (Done /\ UNCHANGED <<inCall, lastExec>>)

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
SpecFair ==
    /\ Spec
    /\ WF_vars(Exec(\E r \in Rings : IoSend(r)))
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
    /\ WF_vars(CompleteNext /\ UNCHANGED <<inCall, lastExec>>)
    /\ WF_vars(OnMain(Free))

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

\* while the transport runs, received datagrams in the I/O thread's buffer
\* always have a next step toward update: a publish, an expose, a return,
\* or a wake of the blocked I/O thread
NoReceiveStall ==
    (phase = "Running" /\ count[FillBuffer] > 0) =>
        ENABLED (IoPublish \/ Expose \/ Return \/ IoWake)

\* the I/O thread is never held inside a send: a send is posted and the
\* step goes on (rocketnet_client_io_step's post, then its reap)
IoNeverBlocksOnSend == ~inSend

\* no ring is reopened, and the transport is never freed, while a send of
\* that ring is outstanding: the kernel still reads the slot's bytes
NoFreeBeforeComplete ==
    /\ \A r \in Rings : posted[r] # {} => ring[r] = "Sending"
    /\ phase = "Freed" => \A r \in Rings : posted[r] = {}

\* every posted send eventually completes, ok or with an error
PostedEventuallyCompletes ==
    \A r \in Rings, s \in Slots : s \in posted[r] ~> s \notin posted[r]

\* destroy still reaches the free once sends are posted
StopEventuallyFrees ==
    phase = "Stopping" ~> phase = "Freed"

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
