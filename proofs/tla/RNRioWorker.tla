----------------------------- MODULE RNRioWorker -----------------------------
(***************************************************************************)
(* The Windows Registered I/O server transport with its I/O worker,       *)
(* sdk/rocketnet_server_rio_windows.cpp.                                   *)
(*                                                                         *)
(* RIO request and completion queues are unsynchronized: the request      *)
(* queue carries both sends and receives, so one thread must issue both.   *)
(* One I/O worker therefore owns the request queue and both completion    *)
(* queues.  It dequeues receive completions into the one receive bank it   *)
(* owns (the fill bank), re-posts each slice, and publishes the fill bank  *)
(* onto a FIFO of ready banks in exchange for a free bank.  It takes send  *)
(* phases that main has handed to it, posts their finished slots with      *)
(* RIO_MSG_DEFER and one commit, retires send completions and returns a    *)
(* drained phase to main as idle.                                          *)
(*                                                                         *)
(* Main (rocketnet_server_rio_receive_packets) try-locks, returns the     *)
(* banks it exposed last call to the free FIFO and exposes the ready banks *)
(* that fit the process view; the views stay valid until its next call.   *)
(* Main (rocketnet_server_rio_send_packets) try-locks, hands the fill      *)
(* phase to the worker, opening the next of three phases to the fill       *)
(* threads, only when the worker has returned that phase idle; otherwise   *)
(* the fill phase stays open for a later flush.  Only bank indices         *)
(* and phase states change under the mutex; main never waits: every main   *)
(* try may fail, and a failed try retains everything main owns.            *)
(*                                                                         *)
(* When the fill bank is full and no bank is free the worker stops         *)
(* dequeuing (completions wait in the queue, slices stay unposted) and     *)
(* waits for main's wake.  When idle it arms RIONotify on the queues it    *)
(* can serve and waits on the notification event and the wake.  RIONotify *)
(* on a non-empty queue sets the event at once (the documented rule).      *)
(*                                                                         *)
(* Stop sets stopping and the wake.  The worker posts ready phases,        *)
(* commits any deferred sends (a refused commit is retried), retires send  *)
(* completions until no committed send is in flight, closes the socket,    *)
(* which cancels every posted receive and every send still in flight (a    *)
(* cancelled send completes failed), drains both queues and only then      *)
(* reports stopped.  Destroy frees the registered region only after        *)
(* stopped.                                                                *)
(*                                                                         *)
(* Rule switches, each FALSE in one reversed witness:                      *)
(*   SwitchOnIdle   main opens the next phase only when it is idle         *)
(*   BoundedDequeue the worker dequeues at most the fill bank's free room  *)
(*   DrainBeforeExit the worker exits only when nothing is kernel owned    *)
(*   WakeOnReturn   main wakes the worker when it frees a bank             *)
(*   DrainSendsBeforeClose the worker closes the socket only when no       *)
(*                  committed send is in flight                            *)
(***************************************************************************)
EXTENDS Naturals, Sequences, FiniteSets

CONSTANTS Slices,          \* ROCKETNET_RIO_RECEIVES
          NumBanks,        \* ROCKETNET_RIO_RECEIVE_BANKS
          Cap,             \* receive bank capacity
          View,            \* the process view's capacity
          MaxDatagrams,    \* datagrams the network offers
          MaxSends,        \* packets the fill threads stage
          SwitchOnIdle, BoundedDequeue, DrainBeforeExit, WakeOnReturn, DropHanded,
          DrainSendsBeforeClose

ASSUME Slices >= 1 /\ NumBanks >= 2 /\ Cap >= 1 /\ View >= Cap
ASSUME MaxDatagrams \in Nat /\ MaxSends \in Nat
ASSUME {SwitchOnIdle, BoundedDequeue, DrainBeforeExit, WakeOnReturn, DropHanded,
        DrainSendsBeforeClose} \subseteq BOOLEAN

Banks == 1..NumBanks
Ids == 1..MaxDatagrams
SendIds == 1..MaxSends
Phases == {0, 1, 2}    \* ROCKETNET_RIO_SEND_PHASES
NoPhase == 3           \* inhand of -1: no phase in hand
After(p) == (p + 1) % 3   \* the phase opened after p

VARIABLES
    \* receive
    arrived,     \* datagrams offered by the network
    accepted,    \* datagrams a posted slice took, in completion order
    posted,      \* slices posted to the kernel
    retry,       \* slices whose re-post was refused
    rcq,         \* the receive completion queue: ids, 0 a cancelled slice
    data,        \* per bank, its datagrams
    fill,        \* the worker's fill bank
    free,        \* free bank FIFO
    ready,       \* ready bank FIFO
    exposed,     \* banks main exposed in its last call
    view,        \* the datagrams of the exposed banks when exposed
    consumed,    \* datagrams main processed, in order
    \* send
    sent,        \* staged packets so far (the next id is sent + 1 ... see staged)
    staged,      \* per phase, the packets staged into it
    state,       \* per phase: "fill", "ready", "posted", "idle"
    phase,       \* main's fill phase
    sready,      \* phases handed to the worker, oldest first
    inhand,      \* the phase the worker is posting, -1 none
    pend,        \* per phase, posted and uncommitted sends
    infl,        \* per phase, committed sends the kernel owns
    scq,         \* the send completion queue: <<phase, id>>
    done,        \* send ids retired by a completion
    dropped,     \* send ids dropped at stop
    cancelled,   \* send ids the close cancelled in flight (completed failed)
    \* control
    wake,        \* the worker's wake event, pending
    evt,         \* the RIO notification event, pending
    armed,       \* RIONotify armed on the receive queue
    stopping, closed, exited, freed,
    wpc, progress

rvars == <<arrived, accepted, posted, retry, rcq, data, fill, free, ready, exposed, view, consumed>>
svars == <<sent, staged, state, phase, sready, inhand, pend, infl, scq, done, dropped, cancelled>>
cvars == <<wake, evt, armed, stopping, closed, exited, freed, wpc, progress>>
vars == <<rvars, svars, cvars>>

Range(s) == {s[i] : i \in 1..Len(s)}
RECURSIVE Concat(_)
Concat(s) == IF s = <<>> THEN <<>> ELSE data[Head(s)] \o Concat(Tail(s))
RECURSIVE Sum(_)
Sum(s) == IF s = <<>> THEN 0 ELSE Len(data[Head(s)]) + Sum(Tail(s))
NonZero(s) == SelectSeq(s, LAMBDA x : x # 0)
OfPhase(p) == {i \in 1..Len(scq) : scq[i][1] = p}
Outstanding(p) == pend[p] # <<>> \/ infl[p] # <<>> \/ OfPhase(p) # {} \/ inhand = p
KernelOwnsSend == \E p \in Phases : pend[p] # <<>> \/ infl[p] # <<>>
Uncommitted == \E p \in Phases : pend[p] # <<>>

Init ==
    /\ arrived = 0 /\ accepted = <<>> /\ posted = Slices /\ retry = 0 /\ rcq = <<>>
    /\ data = [b \in Banks |-> <<>>] /\ fill = 1
    /\ free = [i \in 1..(NumBanks - 1) |-> i + 1] /\ ready = <<>>
    /\ exposed = <<>> /\ view = <<>> /\ consumed = <<>>
    /\ sent = 0 /\ staged = [p \in Phases |-> <<>>]
    /\ state = [p \in Phases |-> IF p = 0 THEN "fill" ELSE "idle"]
    /\ phase = 0 /\ sready = <<>> /\ inhand = NoPhase
    /\ pend = [p \in Phases |-> <<>>] /\ infl = [p \in Phases |-> <<>>]
    /\ scq = <<>> /\ done = <<>> /\ dropped = {} /\ cancelled = {}
    /\ wake = FALSE /\ evt = FALSE /\ armed = FALSE
    /\ stopping = FALSE /\ closed = FALSE /\ exited = FALSE /\ freed = FALSE
    /\ wpc = "top" /\ progress = FALSE

\* ------------------------------------------------------------------------
\* The kernel and the network.

\* a datagram completes a posted slice (an armed receive queue notifies)
Arrive ==
    /\ arrived < MaxDatagrams
    /\ ~closed
    /\ posted > 0
    /\ arrived' = arrived + 1
    /\ accepted' = Append(accepted, arrived + 1)
    /\ posted' = posted - 1
    /\ rcq' = Append(rcq, arrived + 1)
    /\ evt' = (evt \/ armed)
    /\ armed' = FALSE
    /\ UNCHANGED <<retry, data, fill, free, ready, exposed, view, consumed>>
    /\ UNCHANGED svars
    /\ UNCHANGED <<wake, stopping, closed, exited, freed, wpc, progress>>

\* no slice is posted: the kernel drops the datagram (an OS count, not ours)
ArriveDropped ==
    /\ arrived < MaxDatagrams
    /\ posted = 0
    /\ arrived' = arrived + 1
    /\ UNCHANGED <<accepted, posted, retry, rcq, data, fill, free, ready, exposed, view, consumed>>
    /\ UNCHANGED svars /\ UNCHANGED cvars

\* a committed send completes.  Send completions do not arm the event in
\* this model: the worker waits only when no send is outstanding.  Once the
\* socket is closed, a send still in flight completes cancelled (failed).
SendComplete(p) ==
    /\ infl[p] # <<>>
    /\ scq' = Append(scq, <<p, Head(infl[p])>>)
    /\ infl' = [infl EXCEPT ![p] = Tail(infl[p])]
    /\ cancelled' = IF closed THEN cancelled \cup {Head(infl[p])} ELSE cancelled
    /\ UNCHANGED rvars
    /\ UNCHANGED <<sent, staged, state, phase, sready, inhand, pend, done, dropped>>
    /\ UNCHANGED cvars

\* ------------------------------------------------------------------------
\* Main: fill threads, send_packets, receive_packets, stop, stopped, destroy.

\* a fill thread stages into the open fill phase (start_packet + finish)
Stage ==
    /\ ~stopping
    /\ sent < MaxSends
    /\ state[phase] = "fill"
    /\ sent' = sent + 1
    /\ staged' = [staged EXCEPT ![phase] = Append(staged[phase], sent + 1)]
    /\ UNCHANGED rvars
    /\ UNCHANGED <<state, phase, sready, inhand, pend, infl, scq, done, dropped, cancelled>>
    /\ UNCHANGED cvars

CanOpen(p) == IF SwitchOnIdle THEN state[p] = "idle" ELSE state[p] \in {"idle", "posted"}

\* send_packets, a successful try: hand a non-empty fill phase only when the
\* next phase can be opened, and open it.  Otherwise the fill phase stays
\* open and keeps its packets for a later flush.
SendPackets ==
    /\ ~stopping
    /\ staged[phase] # <<>>
    /\ CanOpen(After(phase))
    /\ state' = [state EXCEPT ![phase] = "ready", ![After(phase)] = "fill"]
    /\ sready' = Append(sready, phase)
    /\ staged' = [staged EXCEPT ![After(phase)] = <<>>]
    /\ phase' = After(phase)
    /\ wake' = TRUE
    /\ UNCHANGED rvars
    /\ UNCHANGED <<sent, inhand, pend, infl, scq, done, dropped, cancelled>>
    /\ UNCHANGED <<evt, armed, stopping, closed, exited, freed, wpc, progress>>

RECURSIVE Fit(_, _)
\* the longest prefix of ready banks whose datagrams fit the view
Fit(s, room) == IF s = <<>> \/ Len(data[Head(s)]) > room THEN 0
                ELSE 1 + Fit(Tail(s), room - Len(data[Head(s)]))

\* receive_packets, a successful try: return the exposed banks, expose the
\* ready banks that fit, and wake the worker if the free FIFO was empty
ReceivePackets ==
    /\ ~stopping
    /\ LET n == Fit(ready, View)
           taken == SubSeq(ready, 1, n)
       IN /\ free' = free \o exposed
          /\ exposed' = taken
          /\ ready' = SubSeq(ready, n + 1, Len(ready))
          /\ view' = Concat(taken)
          /\ consumed' = consumed \o Concat(taken)
          /\ wake' = (wake \/ (WakeOnReturn /\ free = <<>> /\ exposed # <<>>))
    /\ UNCHANGED <<arrived, accepted, posted, retry, rcq, data, fill>>
    /\ UNCHANGED svars
    /\ UNCHANGED <<evt, armed, stopping, closed, exited, freed, wpc, progress>>

\* a failed try: everything main owns is retained.  This over-approximates
\* contention: the try may fail at any time.
TryFail ==
    /\ ~stopping
    /\ UNCHANGED vars

\* rocketnet_server_rio_stop: stopping, then the wake
Stop ==
    /\ ~stopping
    /\ stopping' = TRUE
    /\ wake' = TRUE
    /\ UNCHANGED rvars /\ UNCHANGED svars
    /\ UNCHANGED <<evt, armed, closed, exited, freed, wpc, progress>>

\* rocketnet_server_rio_destroy after stopped: frees the registered region
Destroy ==
    /\ exited
    /\ ~freed
    /\ freed' = TRUE
    /\ UNCHANGED rvars /\ UNCHANGED svars
    /\ UNCHANGED <<wake, evt, armed, stopping, closed, exited, wpc, progress>>

\* main polls stopped(), or idles after destroy
Poll ==
    /\ stopping
    /\ UNCHANGED vars

\* ------------------------------------------------------------------------
\* The I/O worker.

WTop ==
    /\ wpc = "top"
    /\ wpc' = IF stopping THEN "stop" ELSE "take"
    /\ progress' = FALSE
    /\ UNCHANGED rvars /\ UNCHANGED svars
    /\ UNCHANGED <<wake, evt, armed, stopping, closed, exited, freed>>

\* under the mutex: take the oldest handed phase; main now sees it posted
WTake ==
    /\ wpc = "take"
    /\ IF sready # <<>>
       THEN /\ inhand' = Head(sready)
            /\ sready' = Tail(sready)
            /\ state' = [state EXCEPT ![Head(sready)] = "posted"]
            /\ wpc' = "post"
       ELSE /\ UNCHANGED <<inhand, sready, state>>
            /\ wpc' = "retire"
    /\ UNCHANGED rvars
    /\ UNCHANGED <<sent, staged, phase, pend, infl, scq, done, dropped, cancelled>>
    /\ UNCHANGED <<wake, evt, armed, stopping, closed, exited, freed, progress>>

\* RIOSendEx with RIO_MSG_DEFER for each finished slot: the kernel owns them
WPost ==
    /\ wpc = "post"
    /\ pend' = [pend EXCEPT ![inhand] = staged[inhand]]
    /\ inhand' = NoPhase
    /\ wpc' = "commit"
    /\ progress' = TRUE
    /\ UNCHANGED rvars
    /\ UNCHANGED <<sent, staged, state, phase, sready, infl, scq, done, dropped, cancelled>>
    /\ UNCHANGED <<wake, evt, armed, stopping, closed, exited, freed>>

\* RIO_MSG_COMMIT_ONLY: success moves every deferred send to the kernel's
\* queue; a refusal keeps them deferred for the next pass
CommitSuccess ==
    /\ infl' = [p \in Phases |-> infl[p] \o pend[p]]
    /\ pend' = [p \in Phases |-> <<>>]

WCommit ==
    /\ wpc = "commit"
    /\ \/ CommitSuccess
       \/ /\ Uncommitted
          /\ UNCHANGED <<pend, infl>>
    /\ wpc' = IF stopping THEN "stop" ELSE "retire"
    /\ UNCHANGED rvars
    /\ UNCHANGED <<sent, staged, state, phase, sready, inhand, scq, done, dropped, cancelled>>
    /\ UNCHANGED <<wake, evt, armed, stopping, closed, exited, freed, progress>>

\* dequeue every send completion; a refused commit is retried here
WRetire ==
    /\ wpc = "retire"
    /\ \/ CommitSuccess
       \/ UNCHANGED <<pend, infl>>
    /\ done' = done \o [i \in 1..Len(scq) |-> scq[i][2]]
    /\ scq' = <<>>
    /\ progress' = (progress \/ scq # <<>>)
    /\ wpc' = "dequeue"
    /\ UNCHANGED rvars
    /\ UNCHANGED <<sent, staged, state, phase, sready, inhand, dropped, cancelled>>
    /\ UNCHANGED <<wake, evt, armed, stopping, closed, exited, freed>>

\* retry refused re-posts, then dequeue receive completions into the fill
\* bank: every available completion up to its free room under the rule,
\* every available completion otherwise (the overflow is dropped).  Each dequeued slice is re-posted; a re-post
\* may be refused and wait in the retry list.
WDequeue ==
    /\ wpc = "dequeue"
    /\ LET room == IF BoundedDequeue THEN Cap - Len(data[fill]) ELSE Len(rcq)
           k == IF Len(rcq) < room THEN Len(rcq) ELSE room
       IN \E refused \in 0..Slices :
             /\ refused <= k + retry
             /\ LET got == NonZero(SubSeq(rcq, 1, k))
                    keep == IF Len(data[fill]) + Len(got) <= Cap THEN got
                            ELSE SubSeq(got, 1, Cap - Len(data[fill]))
                IN /\ data' = [data EXCEPT ![fill] = data[fill] \o keep]
                   /\ rcq' = SubSeq(rcq, k + 1, Len(rcq))
                   /\ posted' = posted + k + retry - refused
                   /\ retry' = refused
                   /\ progress' = (progress \/ k > 0)
    /\ wpc' = "publish"
    /\ UNCHANGED <<arrived, accepted, fill, free, ready, exposed, view, consumed>>
    /\ UNCHANGED svars
    /\ UNCHANGED <<wake, evt, armed, stopping, closed, exited, freed>>

\* under the mutex: publish a non-empty fill bank for a free one, and return
\* every drained posted phase to main as idle
WPublish ==
    /\ wpc = "publish"
    /\ IF data[fill] # <<>> /\ free # <<>>
       THEN /\ ready' = Append(ready, fill)
            /\ fill' = Head(free)
            /\ free' = Tail(free)
            /\ data' = [data EXCEPT ![Head(free)] = <<>>]
       ELSE UNCHANGED <<ready, fill, free, data>>
    /\ state' = [p \in Phases |-> IF state[p] = "posted" /\ ~Outstanding(p) THEN "idle" ELSE state[p]]
    /\ wpc' = IF progress THEN "top" ELSE "arm"
    /\ UNCHANGED <<arrived, accepted, posted, retry, rcq, exposed, view, consumed>>
    /\ UNCHANGED <<sent, staged, phase, sready, inhand, pend, infl, scq, done, dropped, cancelled>>
    /\ UNCHANGED <<wake, evt, armed, stopping, closed, exited, freed, progress>>

\* nothing progressed: arm RIONotify on the receive queue when the fill bank
\* has room (a full bank waits on the wake alone).  A non-empty queue sets
\* the event at once.
WArm ==
    /\ wpc = "arm"
    /\ IF Len(data[fill]) < Cap
       THEN IF rcq # <<>> THEN evt' = TRUE /\ armed' = FALSE
                          ELSE armed' = TRUE /\ UNCHANGED evt
       ELSE UNCHANGED <<evt, armed>>
    /\ wpc' = "wait"
    /\ UNCHANGED rvars /\ UNCHANGED svars
    /\ UNCHANGED <<wake, stopping, closed, exited, freed, progress>>

\* WaitForMultipleObjects on the event and the wake.  Sends in flight, a
\* refused commit or a refused re-post use a short timed wait instead.
WWait ==
    /\ wpc = "wait"
    /\ \/ /\ evt
          /\ evt' = FALSE
          /\ UNCHANGED wake
       \/ /\ wake
          /\ wake' = FALSE
          /\ UNCHANGED evt
       \/ /\ KernelOwnsSend \/ retry > 0 \/ scq # <<>>
          /\ UNCHANGED <<evt, wake>>
    /\ wpc' = "top"
    /\ UNCHANGED rvars /\ UNCHANGED svars
    /\ UNCHANGED <<armed, stopping, closed, exited, freed, progress>>

\* Stop posts every handed phase and the finished fill phase through WPost
\* and WCommit before closing. DropHanded retains the old reversed witness.
WStop ==
    /\ wpc = "stop"
    /\ IF ~DropHanded /\ sready # <<>>
       THEN /\ inhand' = Head(sready)
            /\ sready' = Tail(sready)
            /\ state' = [state EXCEPT ![Head(sready)] = "posted"]
            /\ wpc' = "post"
            /\ UNCHANGED <<pend, infl, dropped>>
       ELSE IF ~DropHanded /\ state[phase] = "fill" /\ staged[phase] # <<>>
            THEN /\ inhand' = phase
                 /\ state' = [state EXCEPT ![phase] = "posted"]
                 /\ wpc' = "post"
                 /\ UNCHANGED <<sready, pend, infl, dropped>>
            ELSE /\ dropped' = IF DropHanded
                               THEN dropped \cup UNION {Range(staged[p]) : p \in Range(sready)}
                                            \cup (IF inhand # NoPhase THEN Range(staged[inhand]) ELSE {})
                               ELSE dropped
                 /\ sready' = <<>>
                 /\ inhand' = NoPhase
                 /\ \/ CommitSuccess
                    \/ /\ Uncommitted
                       /\ UNCHANGED <<pend, infl>>
                 /\ wpc' = "close"
                 /\ UNCHANGED state
    /\ UNCHANGED rvars
    /\ UNCHANGED <<sent, staged, phase, scq, done, cancelled>>
    /\ UNCHANGED <<wake, evt, armed, stopping, closed, exited, freed, progress>>

\* Closing the socket cancels every posted receive and every committed send
\* still in flight.  Under DrainSendsBeforeClose the close waits for a
\* successful commit and then for every committed send to complete,
\* retiring send completions on each pass; otherwise it closes once
\* committed, and SendComplete completes the sends still in flight as
\* cancelled.
SendsInFlight == \E p \in Phases : infl[p] # <<>>

WClose ==
    /\ wpc = "close"
    /\ IF Uncommitted
       THEN /\ wpc' = "stop"      \* the close waits for a successful commit
            /\ UNCHANGED <<closed, posted, rcq, scq, done>>
       ELSE IF DrainSendsBeforeClose /\ ~closed /\ SendsInFlight
       THEN /\ done' = done \o [i \in 1..Len(scq) |-> scq[i][2]]
            /\ scq' = <<>>
            /\ wpc' = "stop"      \* the close waits for the sends in flight
            /\ UNCHANGED <<closed, posted, rcq>>
       ELSE /\ IF ~closed
               THEN /\ rcq' = rcq \o [i \in 1..posted |-> 0]
                    /\ posted' = 0
               ELSE UNCHANGED <<rcq, posted>>
            /\ closed' = TRUE
            /\ wpc' = "drain"
            /\ UNCHANGED <<scq, done>>
    /\ UNCHANGED <<arrived, accepted, retry, data, fill, free, ready, exposed, view, consumed>>
    /\ UNCHANGED <<sent, staged, state, phase, sready, inhand, pend, infl, dropped, cancelled>>
    /\ UNCHANGED <<wake, evt, armed, stopping, exited, freed, progress>>

\* drain both queues; exit only when nothing is kernel owned.  Without
\* DrainBeforeExit the worker exits right after the close, before the
\* cancelled requests are dequeued.
WDrain ==
    /\ wpc = "drain"
    /\ IF ~DrainBeforeExit
       THEN /\ exited' = TRUE /\ wpc' = "exited"
            /\ UNCHANGED <<rcq, scq, done>>
       ELSE /\ rcq' = <<>>
            /\ done' = done \o [i \in 1..Len(scq) |-> scq[i][2]]
            /\ scq' = <<>>
            /\ IF posted = 0 /\ ~KernelOwnsSend
               THEN /\ exited' = TRUE /\ wpc' = "exited"
               ELSE /\ UNCHANGED exited /\ wpc' = "drain"
    /\ UNCHANGED <<arrived, accepted, posted, retry, data, fill, free, ready, exposed, view, consumed>>
    /\ UNCHANGED <<sent, staged, state, phase, sready, inhand, pend, infl, dropped, cancelled>>
    /\ UNCHANGED <<wake, evt, armed, stopping, closed, freed, progress>>

Next ==
    \/ Arrive \/ ArriveDropped \/ \E p \in Phases : SendComplete(p)
    \/ Stage \/ SendPackets \/ ReceivePackets \/ TryFail
    \/ Stop \/ Destroy \/ Poll
    \/ WTop \/ WTake \/ WPost \/ WCommit \/ WRetire \/ WDequeue \/ WPublish
    \/ WArm \/ WWait \/ WStop \/ WClose \/ WDrain

Spec == Init /\ [][Next]_vars

WorkerStep == WTop \/ WTake \/ WPost \/ WCommit \/ WRetire \/ WDequeue \/ WPublish
              \/ WArm \/ WWait \/ WStop \/ WClose \/ WDrain

\* The kernel completes committed sends; the worker runs; main's tries
\* succeed if attempted infinitely often; a refused commit eventually
\* succeeds (strong fairness on the commit steps' success).
SpecFair ==
    /\ Spec
    /\ \A p \in Phases : WF_vars(SendComplete(p))
    /\ WF_vars(WorkerStep)
    /\ SF_vars(WCommit /\ ~Uncommitted') /\ SF_vars(WRetire /\ ~Uncommitted')
    /\ SF_vars(WStop /\ ~Uncommitted')
    /\ SF_vars(WDequeue /\ retry' = 0)
    /\ SF_vars(SendPackets) /\ SF_vars(ReceivePackets)
    /\ WF_vars(Destroy)

-----------------------------------------------------------------------------

TypeOK ==
    /\ arrived \in 0..MaxDatagrams
    /\ posted \in 0..Slices /\ retry \in 0..Slices
    /\ data \in [Banks -> Seq(Ids)]
    /\ \A b \in Banks : Len(data[b]) <= Cap
    /\ fill \in Banks
    /\ state \in [Phases -> {"fill", "ready", "posted", "idle"}]
    /\ phase \in Phases
    /\ inhand \in Phases \cup {NoPhase}
    /\ cancelled \subseteq SendIds

\* Slice accounting until the socket closes: every slice is posted,
\* completed in the queue, or waiting for a re-post.
SlicesAccounted == closed \/ posted + Len(rcq) + retry = Slices

\* Every bank has exactly one owner: the worker's fill bank, the ready FIFO,
\* main's exposed banks or the free FIFO.
ExclusiveOwnership ==
    LET owners == <<fill>> \o ready \o exposed \o free
    IN Len(owners) = NumBanks /\ Range(owners) = Banks

\* The views main reads are stable until its next call.
ExposedStable == Concat(exposed) = view

\* Exact delivery of every accepted datagram, until stop.
ExactDelivery ==
    ~stopping => consumed \o Concat(ready) \o data[fill] \o NonZero(rcq) = accepted

\* The fill threads never write a frame the kernel owns or a phase the
\* worker holds: the open fill phase has no send outstanding.
FillPhaseOwnedByMain ==
    ~stopping =>
        /\ state[phase] = "fill"
        /\ ~Outstanding(phase)
        /\ inhand # phase
        /\ phase \notin Range(sready)

\* No send is retired twice.
NoSendDuplicated ==
    \A i, j \in 1..Len(done) : i # j => done[i] # done[j]

\* The registered region is freed only when the kernel owns no request and
\* every completion has been dequeued.
FreedOnlyWhenDrained ==
    freed => posted = 0 /\ ~KernelOwnsSend /\ rcq = <<>> /\ scq = <<>>

\* Every staged send is retired at exit; stop never silently drops a phase,
\* and the close cancels no send in flight: every send retired delivered.
StopSendsHanded == exited => dropped = {}
StopSendsFinished == exited => Range(done) = 1..sent /\ cancelled = {}

\* Liveness: every accepted datagram reaches main unless stop begins.
EveryDatagramConsumed ==
    \A i \in Ids : (\E j \in 1..Len(accepted) : accepted[j] = i)
                   ~> (stopping \/ \E j \in 1..Len(consumed) : consumed[j] = i)

\* Liveness: a staged packet is sent unless stop begins.
EveryStagedSent ==
    \A s \in SendIds : (sent >= s) ~> (stopping \/ \E j \in 1..Len(done) : done[j] = s)

\* Liveness: stop completes, and destroy frees the region.
StopCompletes == stopping ~> freed

=============================================================================
