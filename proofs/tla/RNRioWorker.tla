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
(* Stop sets stopping and the wake.  Each stop pass                        *)
(* (rocketnet_rio_stop_pass) posts ready phases, commits any deferred      *)
(* sends, keeps the socket open while a committed send is in flight,       *)
(* closes the socket, which cancels every posted receive and every send    *)
(* still in flight (a cancelled send completes failed; the cancellations   *)
(* arrive later, CancelArrives), then dequeues each queue in calls of at   *)
(* most DrainBatch completions until a call comes back short.  A pass      *)
(* that cannot finish (a refused commit, a refused closesocket, sends in   *)
(* flight, cancellations not yet arrived) fails and the pass is run again. *)
(* The worker repeats it without bound.  InlineStop is create's failure    *)
(* path, before the worker exists: there the drain gives up after          *)
(* StopDrainWaits+1 failed passes that dequeued no receive completion,     *)
(* and then leaks the region rather than freeing memory the kernel         *)
(* may still write; that can follow a closesocket that succeeded when the  *)
(* cancellations are late (LeakOnlyAfterStalls).  Destroy frees the        *)
(* registered region only after                                            *)
(* stopped.  No fairness says a refused commit or close later succeeds.    *)
(*                                                                         *)
(* Rule switches, each FALSE in one reversed witness:                      *)
(*   SwitchOnIdle   main opens the next phase only when it is idle         *)
(*   BoundedDequeue the worker dequeues at most the fill bank's free room  *)
(*   DrainBeforeExit the worker exits only when nothing is kernel owned    *)
(*   WakeOnReturn   main wakes the worker when it frees a bank             *)
(*   DrainSendsBeforeClose the worker closes the socket only when no       *)
(*                  committed send is in flight                            *)
(*   BoundedStopDrain create's inline drain gives up after                 *)
(*                  StopDrainWaits+1 counted passes (FALSE: retries        *)
(*                  forever)                                               *)
(*   DrainLoop      a stop pass dequeues again after a full batch (FALSE:  *)
(*                  one batch per queue per pass)                          *)
(*   CountStalledOnly create's inline drain counts a failed pass against   *)
(*                  its bound only when the pass dequeued no receive       *)
(*                  completion (FALSE: every failed pass counts)           *)
(* Either of the last two keeps a draining region from leaking: with      *)
(* both FALSE, passes that each dequeue a batch use up the bound and the   *)
(* region leaks while completions still arrive                             *)
(* (MCRNRioWorkerBrokenOneBatchPerPass.cfg); with CountStalledOnly alone   *)
(* the same passes drain it (MCRNRioWorkerInlineStopBatchPerPass.cfg).     *)
(***************************************************************************)
EXTENDS Naturals, Sequences, FiniteSets

CONSTANTS Slices,          \* receives: rocketnet_server_rio_receives( max_clients )
          NumBanks,        \* ROCKETNET_RIO_RECEIVE_BANKS
          Cap,             \* receive bank capacity
          View,            \* the process view's capacity
          MaxDatagrams,    \* datagrams the network offers
          MaxSends,        \* packets the fill threads stage
          DrainBatch,      \* ROCKETNET_RIO_DEQUEUE_BATCH: completions per dequeue call
          StopDrainWaits,  \* ROCKETNET_RIO_DRAIN_WAITS: waits of create's inline drain
          InlineStop,      \* TRUE: create's failure path, stop before any worker
          SwitchOnIdle, BoundedDequeue, DrainBeforeExit, WakeOnReturn, DropHanded,
          DrainSendsBeforeClose, BoundedStopDrain, DrainLoop, CountStalledOnly

ASSUME Slices >= 1 /\ NumBanks >= 2 /\ Cap >= 1 /\ View >= Cap
ASSUME MaxDatagrams \in Nat /\ MaxSends \in Nat
ASSUME DrainBatch >= 1 /\ StopDrainWaits >= 1
ASSUME {InlineStop, SwitchOnIdle, BoundedDequeue, DrainBeforeExit, WakeOnReturn, DropHanded,
        DrainSendsBeforeClose, BoundedStopDrain, DrainLoop, CountStalledOnly} \subseteq BOOLEAN

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
    retired,     \* completed slices deliberately not re-posted after stop
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
    wpc, progress,
    stopwaits,    \* failed stop passes of create's inline drain (its loop index)
    countedbusy,  \* ghost: a counted failed pass had dequeued a receive completion
    stoprefused   \* the last stop-pass commit or closesocket was refused

rvars == <<arrived, accepted, posted, retry, retired, rcq, data, fill, free, ready, exposed, view, consumed>>
svars == <<sent, staged, state, phase, sready, inhand, pend, infl, scq, done, dropped, cancelled>>
cvars == <<wake, evt, armed, stopping, closed, exited, freed, wpc, progress, stopwaits, countedbusy, stoprefused>>
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
    /\ arrived = 0 /\ accepted = <<>> /\ posted = Slices /\ retry = 0 /\ retired = 0 /\ rcq = <<>>
    /\ data = [b \in Banks |-> <<>>] /\ fill = 1
    /\ free = [i \in 1..(NumBanks - 1) |-> i + 1] /\ ready = <<>>
    /\ exposed = <<>> /\ view = <<>> /\ consumed = <<>>
    /\ sent = 0 /\ staged = [p \in Phases |-> <<>>]
    /\ state = [p \in Phases |-> IF p = 0 THEN "fill" ELSE "idle"]
    /\ phase = 0 /\ sready = <<>> /\ inhand = NoPhase
    /\ pend = [p \in Phases |-> <<>>] /\ infl = [p \in Phases |-> <<>>]
    /\ scq = <<>> /\ done = <<>> /\ dropped = {} /\ cancelled = {}
    \* InlineStop: create failed after posting the receives, before the
    \* worker or any fill thread exists, and called stop at once
    /\ wake = InlineStop /\ evt = FALSE /\ armed = FALSE
    /\ stopping = InlineStop /\ closed = FALSE /\ exited = FALSE /\ freed = FALSE
    /\ wpc = (IF InlineStop THEN "stop" ELSE "top") /\ progress = FALSE
    /\ stopwaits = 0 /\ countedbusy = FALSE /\ stoprefused = FALSE

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
    /\ UNCHANGED <<retry, retired, data, fill, free, ready, exposed, view, consumed>>
    /\ UNCHANGED svars
    /\ UNCHANGED <<wake, stopping, closed, exited, freed, wpc, progress, stopwaits, countedbusy, stoprefused>>

\* no slice is posted: the kernel drops the datagram (an OS count, not ours)
ArriveDropped ==
    /\ arrived < MaxDatagrams
    /\ posted = 0
    /\ arrived' = arrived + 1
    /\ UNCHANGED <<accepted, posted, retry, retired, rcq, data, fill, free, ready, exposed, view, consumed>>
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

\* once the socket is closed, the kernel completes one cancelled receive:
\* its slice reaches the receive completion queue as a failed completion
\* (0).  Cancellations arrive asynchronously after closesocket returns, so
\* a stop pass may find none, some or all of them queued.  posted is the
\* closed socket's receives whose cancellation has not arrived yet.
CancelArrives ==
    /\ closed
    /\ posted > 0
    /\ posted' = posted - 1
    /\ rcq' = Append(rcq, 0)
    /\ UNCHANGED <<arrived, accepted, retry, retired, data, fill, free, ready, exposed, view, consumed>>
    /\ UNCHANGED svars /\ UNCHANGED cvars

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
    /\ UNCHANGED <<evt, armed, stopping, closed, exited, freed, wpc, progress, stopwaits, countedbusy, stoprefused>>

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
    /\ UNCHANGED <<arrived, accepted, posted, retry, retired, rcq, data, fill>>
    /\ UNCHANGED svars
    /\ UNCHANGED <<evt, armed, stopping, closed, exited, freed, wpc, progress, stopwaits, countedbusy, stoprefused>>

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
    /\ UNCHANGED <<evt, armed, closed, exited, freed, wpc, progress, stopwaits, countedbusy, stoprefused>>

\* rocketnet_server_rio_destroy after stopped: frees the registered region
Destroy ==
    /\ exited
    /\ ~freed
    /\ freed' = TRUE
    /\ UNCHANGED rvars /\ UNCHANGED svars
    /\ UNCHANGED <<wake, evt, armed, stopping, closed, exited, wpc, progress, stopwaits, countedbusy, stoprefused>>

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
    /\ UNCHANGED <<wake, evt, armed, stopping, closed, exited, freed, stopwaits, countedbusy, stoprefused>>

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
    /\ UNCHANGED <<wake, evt, armed, stopping, closed, exited, freed, progress, stopwaits, countedbusy, stoprefused>>

\* RIOSendEx with RIO_MSG_DEFER for each finished slot: the kernel owns them
WPost ==
    /\ wpc = "post"
    /\ pend' = [pend EXCEPT ![inhand] = staged[inhand]]
    /\ inhand' = NoPhase
    /\ wpc' = "commit"
    /\ progress' = TRUE
    /\ UNCHANGED rvars
    /\ UNCHANGED <<sent, staged, state, phase, sready, infl, scq, done, dropped, cancelled>>
    /\ UNCHANGED <<wake, evt, armed, stopping, closed, exited, freed, stopwaits, countedbusy, stoprefused>>

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
    /\ UNCHANGED <<wake, evt, armed, stopping, closed, exited, freed, progress, stopwaits, countedbusy, stoprefused>>

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
    /\ UNCHANGED <<wake, evt, armed, stopping, closed, exited, freed, stopwaits, countedbusy, stoprefused>>

\* Retry uses one stopping read before its whole loop. Each completion
\* checks stopping separately after copying; a Stop during this aggregate
\* pass can therefore leave a re-posted prefix and a retired suffix.
\* stopDuring summarizes that one Stop interleaving, including its wake.
\* It does not introduce a second worker or a new fairness assumption.
WDequeue ==
    /\ wpc = "dequeue"
    /\ LET room == IF BoundedDequeue THEN Cap - Len(data[fill]) ELSE Len(rcq)
           k == IF Len(rcq) < room THEN Len(rcq) ELSE room
       IN \E stopDuring \in BOOLEAN, attemptRetry \in BOOLEAN, repost \in 0..k :
             /\ stopDuring => ~stopping
             /\ stopping => (~attemptRetry /\ repost = 0)
             /\ (~stopping /\ ~stopDuring) => (attemptRetry /\ repost = k)
             /\ ~attemptRetry => repost = 0
             /\ LET attempted == IF attemptRetry THEN retry ELSE 0
                    got == NonZero(SubSeq(rcq, 1, k))
                    keep == IF Len(data[fill]) + Len(got) <= Cap THEN got
                            ELSE SubSeq(got, 1, Cap - Len(data[fill]))
                IN \E refused \in 0..(attempted + repost) :
                     /\ data' = [data EXCEPT ![fill] = data[fill] \o keep]
                     /\ rcq' = SubSeq(rcq, k + 1, Len(rcq))
                     /\ posted' = posted + attempted + repost - refused
                     /\ retry' = retry - attempted + refused
                     /\ retired' = retired + k - repost
                     /\ progress' = (progress \/ k > 0)
             /\ stopping' = (stopping \/ stopDuring)
             /\ wake' = (wake \/ stopDuring)
    /\ wpc' = "publish"
    /\ UNCHANGED <<arrived, accepted, fill, free, ready, exposed, view, consumed>>
    /\ UNCHANGED svars
    /\ UNCHANGED <<evt, armed, closed, exited, freed, stopwaits, countedbusy, stoprefused>>

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
    /\ UNCHANGED <<arrived, accepted, posted, retry, retired, rcq, exposed, view, consumed>>
    /\ UNCHANGED <<sent, staged, phase, sready, inhand, pend, infl, scq, done, dropped, cancelled>>
    /\ UNCHANGED <<wake, evt, armed, stopping, closed, exited, freed, progress, stopwaits, countedbusy, stoprefused>>

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
    /\ UNCHANGED <<wake, stopping, closed, exited, freed, progress, stopwaits, countedbusy, stoprefused>>

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
    /\ UNCHANGED <<armed, stopping, closed, exited, freed, progress, stopwaits, countedbusy, stoprefused>>

\* ------------------------------------------------------------------------
\* The stop pass, rocketnet_rio_stop_pass.  The worker runs it at the top of
\* every loop once stopping is set; create's failure path runs it inline
\* through rocketnet_server_rio_stopped (InlineStop).

\* create's inline drain, rocketnet_rio_create_failed_drain: it runs the
\* pass, and after a failed pass waits and runs it again.  stopwaits is its
\* `stalled`: the failed passes during which posted_receives did not fall,
\* that is, that dequeued no receive completion (CountStalledOnly).  A pass
\* may start while at most StopDrainWaits have stalled, so the drain gives
\* up at StopDrainWaits + 1 stalled passes, and a pass that made progress
\* costs nothing: a drain that keeps accounting for cancelled receives runs
\* until it has them all.  CountStalledOnly FALSE is the drain before that
\* rule, which counted every failed pass.  The worker's loop has no bound.
Bounded == InlineStop /\ BoundedStopDrain
PassMayStart == ~Bounded \/ stopwaits <= StopDrainWaits

\* the pass returned false; fell says whether it dequeued a receive
\* completion.  The worker waits 1 ms on the notification
\* event and runs it again; create (rocketnet_rio_create_drain_wait) waits
\* on that event for at most ROCKETNET_RIO_DRAIN_NOTIFY_MILLISECONDS when
\* the socket is closed and receives are still counted posted, not at all
\* otherwise, and runs it again.  Neither wait changes the model's state.
PassFails(fell) ==
    /\ wpc' = "stop"
    /\ stopwaits' = IF Bounded /\ ~(CountStalledOnly /\ fell) THEN stopwaits + 1 ELSE stopwaits
    /\ countedbusy' = (countedbusy \/ (Bounded /\ ~CountStalledOnly /\ fell))

\* Under the mutex: post every handed phase and the finished fill phase
\* through WPost and WCommit, then go to the close.  DropHanded retains the
\* old reversed witness.
WStop ==
    /\ wpc = "stop"
    /\ PassMayStart
    /\ IF ~DropHanded /\ sready # <<>>
       THEN /\ inhand' = Head(sready)
            /\ sready' = Tail(sready)
            /\ state' = [state EXCEPT ![Head(sready)] = "posted"]
            /\ wpc' = "post"
            /\ UNCHANGED dropped
       ELSE IF ~DropHanded /\ state[phase] = "fill" /\ staged[phase] # <<>>
            THEN /\ inhand' = phase
                 /\ state' = [state EXCEPT ![phase] = "posted"]
                 /\ wpc' = "post"
                 /\ UNCHANGED <<sready, dropped>>
            ELSE /\ dropped' = IF DropHanded
                               THEN dropped \cup UNION {Range(staged[p]) : p \in Range(sready)}
                                            \cup (IF inhand # NoPhase THEN Range(staged[inhand]) ELSE {})
                               ELSE dropped
                 /\ sready' = <<>>
                 /\ inhand' = NoPhase
                 /\ wpc' = "close"
                 /\ UNCHANGED state
    /\ UNCHANGED rvars
    /\ UNCHANGED <<sent, staged, phase, pend, infl, scq, done, cancelled>>
    /\ UNCHANGED <<wake, evt, armed, stopping, closed, exited, freed, progress, stopwaits, countedbusy, stoprefused>>

\* rocketnet_rio_close_socket.  A deferred send is committed first; a
\* refused commit fails the pass with the socket open.  Under
\* DrainSendsBeforeClose a committed send in flight fails the pass after the
\* send completions available are retired.  closesocket itself may be
\* refused, which fails the pass with the socket open.  A successful close
\* cancels every posted receive and every committed send still in flight;
\* the cancellations arrive later, CancelArrives for a receive and
\* SendComplete (failed) for a send, so the drain of this pass may find
\* none of them yet.  Without DrainBeforeExit the worker exits right after
\* the close, before the cancelled requests are dequeued.
SendsInFlight == \E p \in Phases : infl[p] # <<>>

WClose ==
    /\ wpc = "close"
    /\ IF Uncommitted
       THEN /\ \/ /\ CommitSuccess
                  /\ wpc' = "close"
                  /\ stoprefused' = FALSE
                  /\ UNCHANGED <<stopwaits, countedbusy>>
               \/ /\ UNCHANGED <<pend, infl>>
                  /\ PassFails(FALSE)
                  /\ stoprefused' = TRUE
            /\ UNCHANGED <<closed, posted, rcq, scq, done>>
       ELSE IF DrainSendsBeforeClose /\ ~closed /\ SendsInFlight
       THEN /\ done' = done \o [i \in 1..Len(scq) |-> scq[i][2]]
            /\ scq' = <<>>
            /\ PassFails(FALSE)
            /\ UNCHANGED <<closed, posted, rcq, pend, infl, stoprefused>>
       ELSE IF closed
       THEN /\ wpc' = "drains"
            /\ UNCHANGED <<closed, posted, rcq, scq, done, pend, infl, stopwaits, countedbusy, stoprefused>>
       ELSE \/ /\ PassFails(FALSE)
               /\ stoprefused' = TRUE
               /\ UNCHANGED <<closed, posted, rcq, scq, done, pend, infl>>
            \/ /\ closed' = TRUE
               /\ stoprefused' = FALSE
               /\ wpc' = IF DrainBeforeExit THEN "drains" ELSE "exited"
               /\ UNCHANGED <<posted, rcq, scq, done, pend, infl, stopwaits, countedbusy>>
    /\ exited' = (wpc' = "exited")
    /\ UNCHANGED <<arrived, accepted, retry, retired, data, fill, free, ready, exposed, view, consumed>>
    /\ UNCHANGED <<sent, staged, state, phase, sready, inhand, dropped, cancelled>>
    /\ UNCHANGED <<wake, evt, armed, stopping, freed, progress>>

\* One RIODequeueCompletion call into RIORESULT results[DEQUEUE_BATCH]:
\* it returns at most DrainBatch completions.  The pass calls again only
\* when the call came back full (`count < ROCKETNET_RIO_DEQUEUE_BATCH`
\* breaks the loop), so a queue of exactly DrainBatch takes a second call
\* that returns 0.  DrainLoop FALSE is the old pass: one call per queue.
Taken(q) == IF Len(q) < DrainBatch THEN Len(q) ELSE DrainBatch

\* the send completion queue, first
WDrainSend ==
    /\ wpc = "drains"
    /\ LET n == Taken(scq)
       IN /\ done' = done \o [i \in 1..n |-> scq[i][2]]
          /\ scq' = SubSeq(scq, n + 1, Len(scq))
          /\ wpc' = IF DrainLoop /\ n = DrainBatch THEN "drains" ELSE "drainr"
    /\ UNCHANGED rvars
    /\ UNCHANGED <<sent, staged, state, phase, sready, inhand, pend, infl, dropped, cancelled>>
    /\ UNCHANGED <<wake, evt, armed, stopping, closed, exited, freed, progress, stopwaits, countedbusy, stoprefused>>

\* then the receive completion queue.  After its last call the pass returns
\* true only when no send is outstanding (deferred, in flight or queued)
\* and no receive is posted or queued; the worker then sets exited.  The
\* pass's place in its dequeue loop is "drainr" before any call returned a
\* completion and "drainrp" after a full batch, so a pass that fails knows
\* whether it dequeued one: posted_receives fell during it.
WDrainReceive ==
    /\ wpc \in {"drainr", "drainrp"}
    /\ LET n == Taken(rcq)
           fell == n > 0 \/ wpc = "drainrp"
       IN /\ rcq' = SubSeq(rcq, n + 1, Len(rcq))
          /\ retired' = retired + n
          /\ IF DrainLoop /\ n = DrainBatch
             THEN /\ wpc' = "drainrp"
                  /\ UNCHANGED <<exited, stopwaits, countedbusy>>
             ELSE IF posted = 0 /\ rcq' = <<>> /\ scq = <<>> /\ ~KernelOwnsSend
                  THEN /\ wpc' = "exited"
                       /\ exited' = TRUE
                       /\ UNCHANGED <<stopwaits, countedbusy>>
                  ELSE /\ PassFails(fell)
                       /\ UNCHANGED exited
    /\ UNCHANGED <<arrived, accepted, posted, retry, data, fill, free, ready, exposed, view, consumed>>
    /\ UNCHANGED svars
    /\ UNCHANGED <<wake, evt, armed, stopping, closed, freed, progress, stoprefused>>

\* create's failure path after StopDrainWaits + 1 counted passes: it leaks
\* the transport instead of freeing memory the kernel may still write.
WLeak ==
    /\ Bounded
    /\ wpc = "stop"
    /\ stopwaits = StopDrainWaits + 1
    /\ wpc' = "leaked"
    /\ UNCHANGED rvars /\ UNCHANGED svars
    /\ UNCHANGED <<wake, evt, armed, stopping, closed, exited, freed, progress, stopwaits, countedbusy, stoprefused>>

Next ==
    \/ Arrive \/ ArriveDropped \/ \E p \in Phases : SendComplete(p) \/ CancelArrives
    \/ Stage \/ SendPackets \/ ReceivePackets \/ TryFail
    \/ Stop \/ Destroy \/ Poll
    \/ WTop \/ WTake \/ WPost \/ WCommit \/ WRetire \/ WDequeue \/ WPublish
    \/ WArm \/ WWait \/ WStop \/ WClose \/ WDrainSend \/ WDrainReceive \/ WLeak

Spec == Init /\ [][Next]_vars

WorkerStep == WTop \/ WTake \/ WPost \/ WCommit \/ WRetire \/ WDequeue \/ WPublish
              \/ WArm \/ WWait \/ WStop \/ WClose \/ WDrainSend \/ WDrainReceive \/ WLeak

\* The kernel completes committed sends and delivers every cancellation of
\* a closed socket's receives; the worker (or create's inline
\* drain) runs; main's tries succeed if attempted infinitely often.  While
\* running, a refused commit is retried until it succeeds (strong fairness
\* on WCommit and WRetire before stop).  No fairness covers the stop pass's
\* commit or closesocket: either may be refused forever.
SpecFair ==
    /\ Spec
    /\ \A p \in Phases : WF_vars(SendComplete(p))
    /\ WF_vars(CancelArrives)
    /\ WF_vars(WorkerStep)
    /\ SF_vars(~stopping /\ WCommit /\ ~Uncommitted') /\ SF_vars(~stopping /\ WRetire /\ ~Uncommitted')
    /\ SF_vars(WDequeue /\ retry' = 0)
    /\ SF_vars(SendPackets) /\ SF_vars(ReceivePackets)
    /\ WF_vars(Destroy)

-----------------------------------------------------------------------------

TypeOK ==
    /\ arrived \in 0..MaxDatagrams
    /\ posted \in 0..Slices /\ retry \in 0..Slices /\ retired \in 0..Slices
    /\ data \in [Banks -> Seq(Ids)]
    /\ \A b \in Banks : Len(data[b]) <= Cap
    /\ fill \in Banks
    /\ state \in [Phases -> {"fill", "ready", "posted", "idle"}]
    /\ phase \in Phases
    /\ inhand \in Phases \cup {NoPhase}
    /\ cancelled \subseteq SendIds
    /\ stopwaits \in 0..(StopDrainWaits + 1)
    /\ countedbusy \in BOOLEAN
    /\ stoprefused \in BOOLEAN

\* Every slice stays accounted through stop, close and drain. A retry
\* entry is unposted and remains accounted even when stop skips its retry.
\* While running, none is retired: refusal cannot shrink receive depth.
SlicesAccounted ==
    /\ posted + Len(rcq) + retry + retired = Slices
    /\ (~stopping => retired = 0)

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

\* create's bounded inline drain leaks only after StopDrainWaits + 1
\* stalled passes, each of which dequeued no receive completion (a refused
\* commit or closesocket, or a closed socket whose cancellations have not
\* arrived), and only while something is still kernel owned or queued: a
\* pass that dequeues a completion is never counted against the bound.
\* The code guarantees no more: closesocket can succeed and the
\* cancellations arrive after the last wait (NotReachedLateCancelLeak).
LeakOnlyAfterStalls ==
    wpc = "leaked" =>
        /\ ~countedbusy
        /\ posted > 0 \/ rcq # <<>> \/ scq # <<>> \/ KernelOwnsSend

\* Reachability target, expected to fail: create's inline drain leaks after
\* a closesocket that succeeded, its cancellations late.
NotReachedLateCancelLeak == ~(wpc = "leaked" /\ closed /\ ~stoprefused)

\* Liveness on create's failure path (InlineStop): stop completes and
\* destroy frees the region, or the bounded drain gives up and leaks it,
\* with no fairness on a refused commit or closesocket.
StopCompletes == stopping ~> (freed \/ wpc = "leaked")

\* Liveness on the worker's path, which retries the stop pass without
\* bound: stop completes and destroy frees the region whenever the kernel
\* does not refuse every later commit or closesocket.  No fairness assumes
\* a refusal ends; a kernel that refuses forever falsifies the premise.
StopCompletesUnlessRefused == ([]<>~stoprefused) => (stopping ~> freed)

=============================================================================
