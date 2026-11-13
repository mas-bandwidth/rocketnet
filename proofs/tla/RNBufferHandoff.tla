--------------------------- MODULE RNBufferHandoff ---------------------------
(***************************************************************************)
(* The N-buffer handoff of sdk/rocketnet_buffer_handoff.h between the     *)
(* game thread (main) and the client transport's I/O thread (worker), as  *)
(* docs/decisions/BUFFER-HANDOFF-DECISION-20261021.md decides it.  N    *)
(* buffers; every                                                          *)
(* buffer is owned by exactly one of the producer, the ready queue, the   *)
(* consumer and the free queue.  Only indices move, and only inside a     *)
(* mutex held for the exchange alone: data is written, read and cleared   *)
(* outside it.  Main only try-locks: a busy mutex defers its exchange to  *)
(* its next frame, so main never waits.  The worker locks, and sleeps on  *)
(* a level-triggered wake when it can make no progress.                   *)
(*                                                                         *)
(* Direction "send": main produces (game threads fill payload rings,      *)
(* rocketnet_client_io_hand_off publishes) and the worker consumes (takes *)
(* a ready ring, sends it outside the mutex, releases it).  Stop hands    *)
(* main's last ring to the worker with the quit store; the worker sends   *)
(* every ready ring, then that one, then exits.                           *)
(*                                                                         *)
(* Direction "receive": the worker produces (datagrams into its buffer,   *)
(* published when no ready buffer is waiting: the triple buffer,          *)
(* MaxReady 1) and main consumes.  Expose returns a deferred held buffer  *)
(* and takes the ready one; Return releases a consumed held buffer and   *)
(* takes nothing.  A worker that cannot publish                           *)
(* records the publish as owed under the mutex; main's next exchange      *)
(* reads the flag under the same mutex and wakes the worker after it      *)
(* drops the mutex.  The worker's wait returns on the wake, or on         *)
(* readable while its buffer has room.  Stop: the worker exits.           *)
(*                                                                         *)
(* Defect switches, FALSE in the passing runs, each restored by one       *)
(* reversed witness: MainBlocks (main acquires the mutex, waiting when it  *)
(* is held), WorkUnderLock (the producer writes data while it holds the   *)
(* mutex), WriteAfterPublish (the worker keeps writing a buffer after it  *)
(* published it), SkipWake (main does not wake the worker it owes),       *)
(* ReleaseBeforeSent (the send worker frees a ring before it has sent it)  *)
(* DropFinal (the send worker exits without the ring stop handed it),    *)
(* DiscardOnStop (queued and final send buffers are discarded on quit),   *)
(* DestinationOverwrite (every hand-off attempt overwrites the Filling    *)
(* ring's destination, a held one too) and SkipRebind (the family race's  *)
(* commit leaves a held ring bound to the losing server).                 *)
(*                                                                         *)
(* Destinations (send).  addr is the server address main passes to the    *)
(* hand-off; each item records the address it was committed with.  The   *)
(* first attempt that finds committed items binds the Filling ring's      *)
(* destination (io->to, io->to_held), before it try-acquires the mutex;   *)
(* an attempt that returns LATE, on a busy mutex (MainLate) or with no     *)
(* ring free (ProducerExchange), leaves it bound, and a later attempt    *)
(* keeps it.  A publish clears the binding.  Two address changes:        *)
(* Reset, the disconnect's protocol reset (later hand-offs pass the       *)
(* zeroed address "Z" and nothing more is committed), and Rebind, the     *)
(* family race's commit from server "A" to the kept server "B", which      *)
(* rebinds a held ring bound to "A" (rocketnet_client_io_rebind_held).    *)
(* Items committed before the race commit were for both families, so an   *)
(* "A" item may leave for "B" (RNClientRace PostCommitSlotsKept).         *)
(***************************************************************************)
EXTENDS Naturals, Sequences

CONSTANTS N,                \* buffers
          Cap,              \* items one buffer holds
          MaxItems,         \* items produced in one behaviour
          MaxReady,         \* ready buffers the producer may queue
          Direction,        \* "send" or "receive"
          MainBlocks, WorkUnderLock, WriteAfterPublish, SkipWake,
          ReleaseBeforeSent, DropFinal, DiscardOnStop,
          DestinationOverwrite, SkipRebind

ASSUME N >= 3 /\ Cap >= 1 /\ MaxReady >= 1 /\ MaxReady < N
ASSUME Direction \in {"send", "receive"}

NONE == N
Buffers == 0 .. N - 1
Threads == {"main", "worker"}
Addresses == { "A", "B", "Z" }
NoDest == "none"
Producer == IF Direction = "send" THEN "main" ELSE "worker"
Consumer == IF Direction = "send" THEN "worker" ELSE "main"

VARIABLES
    lock,           \* "none", "main" or "worker": who holds the mutex
    pc,             \* per thread: "run", "locked", "unlock", "waitlock", "done"
    pheld,          \* the producer's buffer, or NONE
    cheld,          \* the consumer's buffer, or NONE
    ready,          \* the ready queue of buffer indices, oldest first
    free,           \* the free queue of buffer indices
    owed,           \* the producer holds a publish back (under the mutex)
    wakeOnUnlock,   \* main read owed (or published) and wakes after unlock
    content,        \* the items in each buffer
    produced,       \* items produced: the next is produced + 1
    consumed,       \* items consumed, in order
    view,           \* the consumer's buffer as it was taken
    wake,           \* the worker's level-triggered wake
    asleep,         \* the worker is in its wait
    idle,           \* the worker's last pass did nothing (it may wait)
    quit,           \* main requested stop
    final,          \* send: the ring stop handed the worker, or NONE
    exited,         \* the worker exited
    lockedWork,     \* a data action ran while its thread held the mutex
    addr,           \* send: the server address main passes to a hand-off
    dest,           \* send: each ring's destination (io->to)
    held,           \* send: the ring's destination is bound (io->to_held)
    itemTo,         \* send: the address each item was committed with
    sentTo          \* send: the destination each consumed item was sent to

vars == << lock, pc, pheld, cheld, ready, free, owed, wakeOnUnlock, content, produced,
           consumed, view, wake, asleep, idle, quit, final, exited, lockedWork,
           addr, dest, held, itemTo, sentTo >>
destVars == << addr, dest, held, itemTo, sentTo >>

Range(s) == { s[i] : i \in 1 .. Len(s) }
Room(b) == b # NONE /\ Len(content[b]) < Cap

Init ==
    /\ lock = "none"
    /\ pc = [ t \in Threads |-> "run" ]
    /\ pheld = 0
    /\ cheld = NONE
    /\ ready = << >>
    /\ free = [ i \in 1 .. N - 1 |-> i ]
    /\ owed = FALSE
    /\ wakeOnUnlock = FALSE
    /\ content = [ b \in Buffers |-> << >> ]
    /\ produced = 0
    /\ consumed = << >>
    /\ view = << >>
    /\ wake = FALSE
    /\ asleep = FALSE
    /\ idle = FALSE
    /\ quit = FALSE
    /\ final = NONE
    /\ exited = FALSE
    /\ lockedWork = FALSE
    /\ addr = "A"
    /\ dest = [ b \in Buffers |-> NoDest ]
    /\ held = [ b \in Buffers |-> FALSE ]
    /\ itemTo = << >>
    /\ sentTo = << >>

(* the producer writes one item into its buffer, outside the mutex; with  *)
(* WorkUnderLock it may also write while it holds it                       *)
Produce(t) ==
    /\ t = Producer
    /\ pc[t] = "run" \/ ( WorkUnderLock /\ pc[t] = "locked" )
    /\ ~quit /\ ~exited
    /\ t = "worker" => ~asleep
    /\ Room(pheld)
    /\ produced < MaxItems
    /\ Direction = "send" => addr # "Z"
    /\ content' = [ content EXCEPT ![pheld] = Append( @, produced + 1 ) ]
    /\ produced' = produced + 1
    /\ lockedWork' = ( lockedWork \/ pc[t] = "locked" )
    /\ idle' = IF t = "worker" THEN FALSE ELSE idle
    /\ itemTo' = IF Direction = "send" THEN Append( itemTo, addr ) ELSE itemTo
    /\ UNCHANGED << lock, pc, pheld, cheld, ready, free, owed, wakeOnUnlock, consumed, view, wake, asleep, quit, final, exited >>
    /\ UNCHANGED << addr, dest, held, sentTo >>

(* WriteAfterPublish: the worker writes into a buffer it already          *)
(* published, which update now holds (receive)                            *)
WriteReady ==
    /\ WriteAfterPublish /\ Direction = "receive"
    /\ pc["worker"] = "run" /\ ~asleep /\ ~exited /\ ~quit
    /\ produced < MaxItems
    /\ Room(cheld)
    /\ content' = [ content EXCEPT ![cheld] = Append( @, produced + 1 ) ]
    /\ produced' = produced + 1
    /\ UNCHANGED << lock, pc, pheld, cheld, ready, free, owed, wakeOnUnlock, consumed, view, wake, asleep, idle, quit, final, exited, lockedWork >>
    /\ UNCHANGED destVars

(* the send worker sends each item of ring b to the ring's destination   *)
(* (rocketnet_client_io_send_ring: sends[i].to = &io->to[ring])           *)
SendTo(b) == [ i \in 1 .. Len( content[b] ) |-> dest[b] ]

(* a hand-off attempt that finds committed items binds the Filling ring's *)
(* destination to addr, unless an earlier attempt bound it; under         *)
(* DestinationOverwrite it overwrites a bound one too                      *)
(* (rocketnet_client_io_hand_off, before the try-acquire)                 *)
Attempt == Direction = "send" /\ pheld # NONE /\ content[pheld] # << >>
BindDest == IF Attempt /\ ( ~held[pheld] \/ DestinationOverwrite )
            THEN [ dest EXCEPT ![pheld] = addr ] ELSE dest
BindHeld == IF Attempt THEN [ held EXCEPT ![pheld] = TRUE ] ELSE held

(* the consumer reads its buffer and clears it, outside the mutex         *)
Consume(t) ==
    /\ t = Consumer
    /\ pc[t] = "run"
    /\ ~exited
    /\ t = "worker" => ~asleep
    /\ cheld # NONE
    /\ content[cheld] # << >>
    /\ consumed' = IF Direction = "send" /\ quit /\ DiscardOnStop
                   THEN consumed ELSE consumed \o content[cheld]
    /\ sentTo' = IF Direction = "send" /\ ~( quit /\ DiscardOnStop )
                 THEN sentTo \o SendTo( cheld ) ELSE sentTo
    /\ content' = [ content EXCEPT ![cheld] = << >> ]
    /\ view' = << >>
    /\ idle' = IF t = "worker" THEN FALSE ELSE idle
    /\ UNCHANGED << lock, pc, pheld, cheld, ready, free, owed, wakeOnUnlock, produced, wake, asleep, quit, final, exited, lockedWork >>
    /\ UNCHANGED << addr, dest, held, itemTo >>

(* main's try-lock: it takes a free mutex or skips this frame; with       *)
(* MainBlocks it waits for a held one                                     *)
(* the consumer exchanges only once it has finished with its buffer      *)
\* (IF, not \/: TLC branches on a disjunction inside an action)
Done(b) == IF b = NONE THEN TRUE ELSE content[b] = << >>
Finished(t) == IF t # Consumer THEN TRUE ELSE ( Done(cheld) \/ ( t = "worker" /\ ReleaseBeforeSent ) )

MainTryLock ==
    /\ pc["main"] = "run"
    /\ ~quit
    /\ Finished("main")
    /\ IF lock = "none"
       THEN /\ lock' = "main"
            /\ pc' = [ pc EXCEPT !["main"] = "locked" ]
       ELSE /\ MainBlocks
            /\ lock' = lock
            /\ pc' = [ pc EXCEPT !["main"] = "waitlock" ]
    /\ dest' = BindDest
    /\ held' = BindHeld
    /\ UNCHANGED << pheld, cheld, ready, free, owed, wakeOnUnlock, content, produced, consumed, view, wake, asleep, idle, quit, final, exited, lockedWork >>
    /\ UNCHANGED << addr, itemTo, sentTo >>

(* send: the try-acquire finds the mutex busy and the hand-off returns    *)
(* LATE; the binding the attempt made stays, and the items leave with a   *)
(* later hand-off.  (On receive a busy try-lock moves nothing, so it is  *)
(* not a step.)                                                           *)
MainLate ==
    /\ pc["main"] = "run"
    /\ ~quit
    /\ ~MainBlocks
    /\ lock # "none"
    /\ Attempt
    /\ dest' = BindDest
    /\ held' = BindHeld
    /\ UNCHANGED << lock, pc, pheld, cheld, ready, free, owed, wakeOnUnlock, content, produced, consumed, view, wake, asleep, idle, quit, final, exited, lockedWork >>
    /\ UNCHANGED << addr, itemTo, sentTo >>

MainLockWait ==
    /\ pc["main"] = "waitlock"
    /\ lock = "none"
    /\ lock' = "main"
    /\ pc' = [ pc EXCEPT !["main"] = "locked" ]
    /\ UNCHANGED << pheld, cheld, ready, free, owed, wakeOnUnlock, content, produced, consumed, view, wake, asleep, idle, quit, final, exited, lockedWork >>
    /\ UNCHANGED destVars

WorkerLock ==
    /\ pc["worker"] = "run"
    /\ ~asleep /\ ~exited
    /\ Finished("worker")
    /\ lock = "none"
    /\ lock' = "worker"
    /\ pc' = [ pc EXCEPT !["worker"] = "locked" ]
    /\ UNCHANGED << pheld, cheld, ready, free, owed, wakeOnUnlock, content, produced, consumed, view, wake, asleep, idle, quit, final, exited, lockedWork >>
    /\ UNCHANGED destVars

(* rocketnet_buffer_handoff_publish: index moves only                     *)
CanPublish == Len(free) > 0 /\ Len(ready) < MaxReady

(* the producer's exchange (main on send, the worker on receive)          *)
ProducerExchange(t) ==
    /\ t = Producer
    /\ pc[t] = "locked"
    /\ IF pheld # NONE /\ content[pheld] # << >>
       THEN IF CanPublish
            THEN /\ ready' = Append( ready, pheld )
                 /\ pheld' = Head( free )
                 /\ free' = Tail( free )
                 /\ owed' = FALSE
                 /\ wakeOnUnlock' = ( t = "main" )
                 /\ idle' = IF t = "worker" THEN FALSE ELSE idle
                 \* send: the published ring keeps its destination and is
                 \* no longer held; the next Filling ring is unbound (its
                 \* stale io->to is read only after an attempt binds it)
                 /\ held' = IF Direction = "send" THEN [ held EXCEPT ![pheld] = FALSE ] ELSE held
                 /\ dest' = IF Direction = "send" THEN [ dest EXCEPT ![Head( free )] = NoDest ] ELSE dest
            ELSE /\ owed' = TRUE
                 /\ UNCHANGED << ready, pheld, free, wakeOnUnlock, dest, held >>
                 /\ idle' = IF t = "worker" THEN TRUE ELSE idle
       ELSE /\ UNCHANGED << ready, pheld, free, owed, wakeOnUnlock, dest, held >>
            /\ idle' = IF t = "worker" THEN TRUE ELSE idle
    /\ pc' = [ pc EXCEPT ![t] = "unlock" ]
    /\ UNCHANGED << lock, cheld, content, produced, consumed, view, wake, asleep, quit, final, exited, lockedWork >>
    /\ UNCHANGED << addr, itemTo, sentTo >>

(* The consumer releases its consumed, cleared buffer.  With take TRUE, *)
(* it also takes the oldest ready one (Expose, or the send worker).      *)
(* Receive Return uses take FALSE only while a buffer is held; after it *)
(* succeeds another Return is disabled until an Expose takes a buffer.   *)
(* Main's exchanges read and clear owed (rocketnet_buffer_handoff_owed) *)
(* and wake only after unlocking.  The worker's send-side exchange never  *)
(* calls rocketnet_buffer_handoff_owed: it leaves owed as it is, and the  *)
(* next successful publish clears it.                                     *)
ConsumerExchange(t, take) ==
    /\ t = Consumer
    /\ pc[t] = "locked"
    /\ ( take \/ ( Direction = "receive" /\ cheld # NONE ) )
    /\ LET freed == IF cheld # NONE THEN Append( free, cheld ) ELSE free
           taken == IF take /\ ready # << >> THEN Head( ready ) ELSE NONE
       IN /\ free' = freed
          /\ cheld' = taken
          /\ ready' = IF take /\ ready # << >> THEN Tail( ready ) ELSE ready
          /\ view' = IF taken # NONE THEN content[taken] ELSE << >>
          /\ idle' = IF t = "worker" THEN taken = NONE ELSE idle
    /\ owed' = IF t = "main" THEN FALSE ELSE owed
    /\ wakeOnUnlock' = ( t = "main" /\ owed /\ ~SkipWake )
    /\ pc' = [ pc EXCEPT ![t] = "unlock" ]
    /\ UNCHANGED << lock, pheld, content, produced, consumed, wake, asleep, quit, final, exited, lockedWork >>
    /\ UNCHANGED destVars

(* the wake is set after the mutex is dropped, never under it             *)
Unlock(t) ==
    /\ pc[t] = "unlock"
    /\ lock = t
    /\ lock' = "none"
    /\ pc' = [ pc EXCEPT ![t] = "run" ]
    /\ wake' = ( wake \/ ( t = "main" /\ wakeOnUnlock ) )
    /\ wakeOnUnlock' = FALSE
    /\ UNCHANGED << pheld, cheld, ready, free, owed, content, produced, consumed, view, asleep, idle, quit, final, exited, lockedWork >>
    /\ UNCHANGED destVars

(* the worker waits when its last pass did nothing                         *)
WorkerWait ==
    /\ pc["worker"] = "run"
    /\ ~asleep /\ ~exited /\ ~quit
    /\ idle
    /\ asleep' = TRUE
    /\ UNCHANGED << lock, pc, pheld, cheld, ready, free, owed, wakeOnUnlock, content, produced, consumed, view, wake, idle, quit, final, exited, lockedWork >>
    /\ UNCHANGED destVars

(* the wait returns on the wake (consumed), or on readable while the      *)
(* receive buffer has room and a datagram waits in the socket             *)
WorkerWakes ==
    /\ asleep
    /\ \/ wake
       \/ ( Direction = "receive" /\ Room(pheld) /\ produced < MaxItems )
    /\ asleep' = FALSE
    /\ wake' = FALSE
    /\ idle' = FALSE
    /\ UNCHANGED << lock, pc, pheld, cheld, ready, free, owed, wakeOnUnlock, content, produced, consumed, view, quit, final, exited, lockedWork >>
    /\ UNCHANGED destVars

(* stop: main stores quit and wakes the worker; on send its ring passes   *)
(* to the worker with the store                                           *)
MainStop ==
    /\ pc["main"] = "run"
    /\ ~quit
    /\ quit' = TRUE
    /\ wake' = TRUE
    /\ pc' = [ pc EXCEPT !["main"] = "done" ]
    /\ IF Direction = "send"
       THEN /\ final' = pheld
            /\ pheld' = NONE
       ELSE UNCHANGED << final, pheld >>
    \* rocketnet_client_io_stop binds an unbound ring with committed items to
    \* the address it is passed: the server address, or once that is zero
    \* io->to of the Filling ring (rocketnet_client_transport_fill_destination)
    /\ dest' = IF Attempt /\ ( ~held[pheld] \/ DestinationOverwrite ) /\ addr # "Z"
               THEN [ dest EXCEPT ![pheld] = addr ] ELSE dest
    /\ UNCHANGED << lock, cheld, ready, free, owed, wakeOnUnlock, content, produced, consumed, view, asleep, idle, exited, lockedWork >>
    /\ UNCHANGED << addr, held, itemTo, sentTo >>

(* exit: send drains the ready queue, then sends the final ring; receive  *)
(* exits at once                                                          *)
WorkerExit ==
    /\ pc["worker"] = "run"
    /\ ~exited /\ quit
    /\ asleep' = FALSE
    /\ IF Direction = "send"
       THEN /\ Done(cheld)
            /\ ready = << >>
            /\ consumed' = IF final # NONE /\ ~DropFinal /\ ~DiscardOnStop THEN consumed \o content[final] ELSE consumed
            /\ sentTo' = IF final # NONE /\ ~DropFinal /\ ~DiscardOnStop THEN sentTo \o SendTo( final ) ELSE sentTo
            /\ content' = IF final # NONE THEN [ content EXCEPT ![final] = << >> ] ELSE content
       ELSE UNCHANGED << consumed, content, sentTo >>
    /\ exited' = TRUE
    /\ pc' = [ pc EXCEPT !["worker"] = "done" ]
    /\ UNCHANGED << lock, pheld, cheld, ready, free, owed, wakeOnUnlock, produced, view, wake, idle, quit, final, lockedWork >>
    /\ UNCHANGED << addr, dest, held, itemTo >>

(* send: disconnect resets the protocol, zeroing the server address.  It  *)
(* first commits its burst and attempts a hand-off with the saved address *)
(* (a hand-off attempt before this step, or a LATE that bound it), so     *)
(* the Filling ring is empty or bound here.  Nothing is committed after.  *)
Reset ==
    /\ Direction = "send"
    /\ pc["main"] = "run" /\ ~quit
    /\ addr \in { "A", "B" }
    /\ ~Attempt \/ held[pheld]
    /\ addr' = "Z"
    /\ UNCHANGED << lock, pc, pheld, cheld, ready, free, owed, wakeOnUnlock, content, produced, consumed, view, wake, asleep, idle, quit, final, exited, lockedWork >>
    /\ UNCHANGED << dest, held, itemTo, sentTo >>

(* send: the family race's commit keeps server "B"; a Filling ring a      *)
(* deferred hand-off bound to the losing server "A" is bound to "B"       *)
(* (rocketnet_client_race_commit, rocketnet_client_io_rebind_held);       *)
(* SkipRebind leaves it bound to "A".  Rebind runs only from addr = "A":  *)
(* a commit after a Reset (addr "Z") is not explored here; RNClientRace   *)
(* carries the commit's rebind on its own, and this model only the rebind *)
(* of a ring bound to the losing server while the protocol still names    *)
(* it.                                                                    *)
Rebind ==
    /\ Direction = "send"
    /\ pc["main"] = "run" /\ ~quit
    /\ addr = "A"
    /\ addr' = "B"
    /\ dest' = IF held[pheld] /\ dest[pheld] = "A" /\ ~SkipRebind
               THEN [ dest EXCEPT ![pheld] = "B" ] ELSE dest
    /\ UNCHANGED << lock, pc, pheld, cheld, ready, free, owed, wakeOnUnlock, content, produced, consumed, view, wake, asleep, idle, quit, final, exited, lockedWork >>
    /\ UNCHANGED << held, itemTo, sentTo >>

(* both threads are done: the behaviour ends here                        *)
Terminated ==
    /\ pc["main"] = "done" /\ exited
    /\ UNCHANGED vars

Next ==
    \/ Terminated
    \/ \E t \in Threads : Produce(t) \/ Consume(t) \/ ProducerExchange(t) \/ ConsumerExchange(t, TRUE) \/ Unlock(t)
    \/ ConsumerExchange("main", FALSE)
    \/ WriteReady
    \/ MainTryLock \/ MainLockWait \/ WorkerLock \/ WorkerWait \/ WorkerWakes
    \/ MainStop \/ WorkerExit
    \/ MainLate \/ Reset \/ Rebind

Spec == Init /\ [][Next]_vars

(* the worker runs whenever it can; main keeps calling update and          *)
(* send_packets (its try-lock succeeds infinitely often when the mutex is *)
(* free infinitely often) and finishes what it started; stop is never     *)
(* forced                                                                 *)
SpecFair ==
    /\ Spec
    /\ \A t \in Threads : WF_vars( Consume(t) ) /\ WF_vars( ProducerExchange(t) ) /\ WF_vars( ConsumerExchange(t, TRUE) ) /\ WF_vars( Unlock(t) )
    /\ WF_vars( Produce("worker") )
    /\ SF_vars( WorkerLock ) /\ WF_vars( WorkerWait ) /\ WF_vars( WorkerWakes ) /\ SF_vars( WorkerExit )
    /\ SF_vars( MainTryLock ) /\ WF_vars( MainLockWait )

----------------------------------------------------------------------------

TypeOK ==
    /\ lock \in { "none", "main", "worker" }
    /\ pc \in [ Threads -> { "run", "locked", "unlock", "waitlock", "done" } ]
    /\ pheld \in Buffers \cup { NONE }
    /\ cheld \in Buffers \cup { NONE }
    /\ final \in Buffers \cup { NONE }
    /\ produced \in 0 .. MaxItems
    /\ addr \in Addresses
    /\ dest \in [ Buffers -> Addresses \cup { NoDest } ]
    /\ held \in [ Buffers -> BOOLEAN ]

(* every buffer has exactly one owner                                     *)
OneOwner ==
    LET owners(b) == ( IF pheld = b THEN 1 ELSE 0 ) + ( IF cheld = b THEN 1 ELSE 0 )
                   + ( IF final = b THEN 1 ELSE 0 )
                   + Len( SelectSeq( ready, LAMBDA x : x = b ) ) + Len( SelectSeq( free, LAMBDA x : x = b ) )
    IN \A b \in Buffers : owners(b) = 1

(* a free buffer holds nothing: it was consumed before it was released    *)
FreeIsEmpty == \A i \in 1 .. Len(free) : content[free[i]] = << >>

(* the consumer's buffer does not change under it                         *)
ViewStable == cheld # NONE => content[cheld] = view

(* items are consumed once each, in the order produced                    *)
InOrder == consumed = [ i \in 1 .. Len(consumed) |-> i ]

(* the mutex covers index moves only                                      *)
NoWorkUnderLock == ~lockedWork

(* main never waits for the mutex                                         *)
MainNeverWaits == pc["main"] # "waitlock"

(* send: each item is sent to the destination it was committed with; an  *)
(* item committed while the family race was open ("A") may leave for the  *)
(* kept server "B"                                                         *)
SentToCommitted ==
    \A i \in 1 .. Len( sentTo ) :
        LET to == itemTo[consumed[i]] IN
        sentTo[i] = to \/ ( to = "A" /\ sentTo[i] = "B" )

(* send: once the worker exits everything main produced was sent          *)
ExitSentAll == ( Direction = "send" /\ exited ) => Len(consumed) = produced

(* send: a stop ends with the worker exited and everything sent           *)
StopSendsAll == quit ~> ( exited /\ ( Direction = "send" => Len(consumed) = produced ) )

(* receive: every datagram that arrives is delivered to update unless     *)
(* update stops first                                                     *)
ArrivedDelivered == ( produced = MaxItems ) ~> ( Len(consumed) = MaxItems \/ quit )

=============================================================================
