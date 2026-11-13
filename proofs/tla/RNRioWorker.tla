----------------------------- MODULE RNRioWorker -----------------------------
(***************************************************************************)
(* The Windows Registered I/O server transport with its I/O worker,       *)
(* sdk/rocketnet_server_rio_windows.cpp.                                   *)
(*                                                                         *)
(* RIO request and completion queues are unsynchronized: the request      *)
(* queue carries both sends and receives, so one thread must issue both.   *)
(* One I/O worker therefore owns the request queue and both completion    *)
(* queues.  It dequeues receive completions into its receive bank (the     *)
(* worker's bank of the receive handoff, sdk/rocketnet_receive_handoff.h,  *)
(* over three banks), re-posts each slice, and at the end of a pass that   *)
(* banked commits the bank's count under the mutex, moving to the free     *)
(* bank when main has started on its own (RNReceiveHandoff Commit).  It    *)
(* takes send phases that main has handed to it, posts their finished      *)
(* slots with RIO_MSG_DEFER and one commit, retires send completions and   *)
(* returns a drained phase to main as idle.                                *)
(*                                                                         *)
(* Main (rocketnet_rio_lane_receive_packets) empties a finished tail,      *)
(* try-locks and begins (RNReceiveHandoff Call: when the worker has left   *)
(* main's bank and the tail is finished, main's bank becomes the tail and  *)
(* main starts on the worker's), then returns the tail's unread datagrams  *)
(* and its own bank's up to the committed count it last saw, within the   *)
(* process view; the views stay valid until its next call.  A failed try   *)
(* returns what main already held.                                         *)
(* Main (rocketnet_server_rio_send_packets) try-locks, hands the fill      *)
(* phase to the worker, opening the next of three phases to the fill       *)
(* threads, only when the worker has returned that phase idle; otherwise   *)
(* the fill phase stays open for a later flush.  Only bank indices         *)
(* and phase states change under the mutex; main never waits: every main   *)
(* try may fail, and a failed try retains everything main owns.            *)
(*                                                                         *)
(* When the worker's bank is full and main has not started on it the       *)
(* worker stops dequeuing (completions wait in the queue, slices stay      *)
(* unposted) and waits for main's wake, which main sets when it starts on  *)
(* a full bank.  At stop, main counts once what was banked and never       *)
(* returned (StopCount).  When idle it arms RIONotify on the queues it     *)
(* can serve and waits on the notification event and the wake.  RIONotify *)
(* on a non-empty queue sets the event at once (the documented rule).      *)
(* The code's wait also times out (50 ms, 1 ms after a refused commit or   *)
(* re-post); the model keeps only the timeouts that matter while sends are *)
(* outstanding or a re-post waits (WWait), so a missing wake shows here as *)
(* a worker that never wakes and in the code as a wait of up to 50 ms.     *)
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
(* cancellations are late (LeakOnlyAfterStalls).  When the bound is spent  *)
(* with closesocket refused and nothing posted or outstanding (create      *)
(* failed before it posted a receive), the kernel owns no request on the   *)
(* region: the drain abandons the socket open, runs one more pass and      *)
(* frees the rest of the lane (WLeak's abandon branch, AbandonUnposted).   *)
(* Destroy frees the registered region only after                          *)
(* stopped.  No fairness says a refused commit or close later succeeds.    *)
(* A completion queue the kernel reports corrupt (RIO_CORRUPT_CQ) is       *)
(* outside the model: the code then never reports stopped and never frees  *)
(* the region (rocketnet_rio_corrupt_queue), so StopCompletesUnlessRefused *)
(* assumes no corrupt queue.                                               *)
(*                                                                         *)
(* Rule switches, each FALSE in one reversed witness:                      *)
(*   SwitchOnIdle   main opens the next phase only when it is idle         *)
(*   BoundedDequeue the worker dequeues at most its bank's free room       *)
(*   DrainBeforeExit the worker exits only when nothing is kernel owned    *)
(*   WakeOnReturn   main wakes the worker when it starts on a full bank    *)
(*                  (FALSE: the model's idle wait has no timeout, so the   *)
(*                  worker never wakes; the code's wait times out after    *)
(*                  50 ms, so without the wake a datagram waits up to      *)
(*                  50 ms, about three updates at 60 Hz, not forever)      *)
(*   ReadOnlyLeft   (TRUE in its witness) main reads only a bank the       *)
(*                  worker has left, as the old ready FIFO did: a          *)
(*                  datagram committed before a call waits for the next    *)
(*                  one (DeliveredByNextCall)                              *)
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
(*   CountRefusedPost a send post the kernel refuses (RIOSendEx false) is  *)
(*                  counted SEND_FAILED, its slot free again (an operator; *)
(*                  FALSE: lost uncounted)                                 *)
(*   AbandonUnposted create's inline drain, its bound spent with nothing   *)
(*                  posted or outstanding, abandons the socket and frees   *)
(*                  the rest of the lane (an operator; FALSE: it leaks the *)
(*                  whole lane with nothing kernel owned)                  *)
(* Either of the last two keeps a draining region from leaking: with      *)
(* both FALSE, passes that each dequeue a batch use up the bound and the   *)
(* region leaks while completions still arrive                             *)
(* (MCRNRioWorkerBrokenOneBatchPerPass.cfg); with CountStalledOnly alone   *)
(* the same passes drain it (MCRNRioWorkerInlineStopBatchPerPass.cfg).     *)
(***************************************************************************)
EXTENDS Naturals, Sequences, FiniteSets

CONSTANTS Slices,          \* receives: rocketnet_server_rio_receives( max_clients )
          Cap,             \* receive bank capacity
          View,            \* the process view's capacity
          MaxDatagrams,    \* datagrams the network offers
          MaxSends,        \* packets the fill threads stage
          DrainBatch,      \* ROCKETNET_RIO_DEQUEUE_BATCH: completions per dequeue call
          StopDrainWaits,  \* ROCKETNET_RIO_DRAIN_WAITS: waits of create's inline drain
          InlineStop,      \* TRUE: create's failure path, stop before any worker
          SwitchOnIdle, BoundedDequeue, DrainBeforeExit, WakeOnReturn, DropHanded,
          DrainSendsBeforeClose, BoundedStopDrain, DrainLoop, CountStalledOnly

\* Slices is the receives posted when stop begins: every one on the worker's
\* path, and on create's failure path the ones posted before create failed,
\* which may be none (a failure before or at the first post).
ASSUME Slices >= (IF InlineStop THEN 0 ELSE 1) /\ Cap >= 1 /\ View >= Cap
ASSUME MaxDatagrams \in Nat /\ MaxSends \in Nat
ASSUME DrainBatch >= 1 /\ StopDrainWaits >= 1
ASSUME {InlineStop, SwitchOnIdle, BoundedDequeue, DrainBeforeExit, WakeOnReturn, DropHanded,
        DrainSendsBeforeClose, BoundedStopDrain, DrainLoop, CountStalledOnly} \subseteq BOOLEAN

\* Rule switch (an operator, overridden in a cfg with <-): a refused send
\* post is counted SEND_FAILED.  FALSE in MCRNRioWorkerBrokenUncountedPost.cfg.
CountRefusedPost == TRUE

\* Rule switch (an operator, overridden in a cfg with <-): create's inline
\* drain, its bound spent with closesocket refused and nothing posted or
\* outstanding, abandons the socket and frees the rest of the lane.  FALSE
\* in MCRNRioWorkerBrokenLeakUnposted.cfg: it leaks the whole lane with
\* nothing kernel owned; must fail LeakOnlyAfterStalls.
AbandonUnposted == TRUE

\* Rule switch (an operator, overridden in a cfg with <-): each pass retires
\* the send completions and returns the drained phases idle before it takes
\* a phase (WRetireFirst, WTake), so no phase main could reuse is still held
\* posted when a new phase is posted.  FALSE in
\* MCRNRioWorkerBrokenTakeBeforeRetire.cfg: the old order, which posts a phase
\* while an earlier drained one is still held; must fail DrainedReturnedAtPost.
RetireFirst == TRUE

\* The datagrams that fail the stateless prefilter (an operator, overridden
\* in a cfg with <-; none by default).  PrefilterInWorker is the rule: the
\* worker runs the prefilter on each completion before it takes a bank slot,
\* counts a refusal in its drop tally and re-posts the slice, so a refused
\* datagram never takes room a player's datagram needs.  FALSE in
\* MCRNRioWorkerBrokenBankGarbage.cfg: garbage is banked and main refuses
\* it later.
Garbage == {}
PrefilterInWorker == TRUE
Refused == IF PrefilterInWorker THEN Garbage ELSE {}
Pass(s) == SelectSeq(s, LAMBDA x : x \notin Refused)

\* Rule switch (an operator, overridden in a cfg with <-): main reads the
\* bank the worker is filling, up to its committed count.  TRUE in
\* MCRNRioWorkerBrokenReadOnlyLeft.cfg: main reads only a bank the worker
\* has left, the old ready FIFO's rule; must fail DeliveredByNextCall.
ReadOnlyLeft == FALSE

\* ROCKETNET_RIO_RECEIVE_BANKS, ROCKETNET_RECEIVE_HANDOFF_BUFFERS
Banks == 0..2
NoBank == 3
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
    data,        \* per bank, its datagrams (num_packets is its length)
    wbank,       \* the handoff's worker bank
    mbank,       \* the handoff's main bank
    tbank,       \* the handoff's tail
    fbank,       \* the handoff's free bank, NoBank while there is none
    committed,   \* per bank, the committed count (under the mutex)
    mcur, mlim,  \* main's alone: main_cursor, main_limit
    tcur, tend,  \* main's alone: tail_cursor, tail_end
    view,        \* main's views from its last call: <<bank, slot, datagram>>
    consumed,    \* datagrams main returned, in order
    drained,     \* datagrams the stop pass dequeued and never banked
    stopdrops,   \* the datagrams StopCount counted RECEIVE_DROP_STOPPED
    stopcounted, \* main made the StopCount
    late,        \* ghost: a call broke DeliveredByNextCall
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
    failed,      \* send ids whose post the kernel refused (counted SEND_FAILED)
    \* control
    wake,        \* the worker's wake event, pending
    evt,         \* the RIO notification event, pending
    armed,       \* RIONotify armed on the receive queue
    stopping, closed, exited, freed,
    wpc, progress,
    stopwaits,    \* failed stop passes of create's inline drain (its loop index)
    countedbusy,  \* ghost: a counted failed pass had dequeued a receive completion
    stoprefused,  \* the last stop-pass commit or closesocket was refused
    waited,       \* ghost: the posted phases every send of which the kernel had finished when this pass began
    abandoned     \* create's inline drain abandoned the socket open (closesocket refused, nothing posted)

mainvars == <<wbank, mbank, tbank, fbank, committed, mcur, mlim, tcur, tend, view, consumed, stopdrops, stopcounted, late>>
hvars == <<mainvars, drained>>
rvars == <<arrived, accepted, posted, retry, retired, rcq, data, hvars>>
svars == <<sent, staged, state, phase, sready, inhand, pend, infl, scq, done, dropped, cancelled, failed>>
cvars == <<wake, evt, armed, stopping, closed, exited, freed, wpc, progress, stopwaits, countedbusy, stoprefused, waited, abandoned>>
vars == <<rvars, svars, cvars>>

Range(s) == {s[i] : i \in 1..Len(s)}
Min(a, b) == IF a < b THEN a ELSE b
\* slots m+1 .. n of s (empty when n <= m)
Slots(s, m, n) == IF n <= m THEN <<>> ELSE SubSeq(s, m + 1, n)
TailFinished == tcur >= tend
\* what the worker banked that main has not returned, oldest first: the
\* tail's rest, main's bank past its cursor (committed or not), and the
\* worker's bank when it has moved on
Unread == Slots(data[tbank], tcur, tend) \o Slots(data[mbank], mcur, Len(data[mbank]))
          \o (IF wbank # mbank THEN data[wbank] ELSE <<>>)
NonZero(s) == SelectSeq(s, LAMBDA x : x # 0)
OfPhase(p) == {i \in 1..Len(scq) : scq[i][1] = p}
Outstanding(p) == pend[p] # <<>> \/ infl[p] # <<>> \/ OfPhase(p) # {} \/ inhand = p
KernelOwnsSend == \E p \in Phases : pend[p] # <<>> \/ infl[p] # <<>>
Uncommitted == \E p \in Phases : pend[p] # <<>>

Init ==
    /\ arrived = 0 /\ accepted = <<>> /\ posted = Slices /\ retry = 0 /\ retired = 0 /\ rcq = <<>>
    \* rocketnet_receive_handoff_init: main and the worker share bank 0,
    \* bank 1 is free and bank 2 is main's finished, empty tail
    /\ data = [b \in Banks |-> <<>>]
    /\ wbank = 0 /\ mbank = 0 /\ tbank = 2 /\ fbank = 1
    /\ committed = [b \in Banks |-> 0]
    /\ mcur = 0 /\ mlim = 0 /\ tcur = 0 /\ tend = 0
    /\ view = <<>> /\ consumed = <<>> /\ drained = <<>>
    /\ stopdrops = <<>> /\ stopcounted = FALSE /\ late = FALSE
    /\ sent = 0 /\ staged = [p \in Phases |-> <<>>]
    /\ state = [p \in Phases |-> IF p = 0 THEN "fill" ELSE "idle"]
    /\ phase = 0 /\ sready = <<>> /\ inhand = NoPhase
    /\ pend = [p \in Phases |-> <<>>] /\ infl = [p \in Phases |-> <<>>]
    /\ scq = <<>> /\ done = <<>> /\ dropped = {} /\ cancelled = {} /\ failed = {}
    \* InlineStop: create failed after posting the receives, before the
    \* worker or any fill thread exists, and called stop at once
    /\ wake = InlineStop /\ evt = FALSE /\ armed = FALSE
    /\ stopping = InlineStop /\ closed = FALSE /\ exited = FALSE /\ freed = FALSE
    /\ wpc = (IF InlineStop THEN "stop" ELSE "top") /\ progress = FALSE
    /\ stopwaits = 0 /\ countedbusy = FALSE /\ stoprefused = FALSE /\ waited = {}
    /\ abandoned = FALSE

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
    /\ UNCHANGED <<retry, retired, data, hvars>>
    /\ UNCHANGED svars
    /\ UNCHANGED <<wake, stopping, closed, exited, freed, wpc, progress, stopwaits, countedbusy, stoprefused, waited, abandoned>>

\* no slice is posted: the kernel drops the datagram (an OS count, not ours)
ArriveDropped ==
    /\ arrived < MaxDatagrams
    /\ posted = 0
    /\ arrived' = arrived + 1
    /\ UNCHANGED <<accepted, posted, retry, retired, rcq, data, hvars>>
    /\ UNCHANGED svars /\ UNCHANGED cvars

\* a committed send completes.  The code's idle wait arms RIONotify on the
\* send queue while sends are outstanding (rocketnet_rio_idle_wait), so a
\* send completion sets the notification event; the model leaves evt alone
\* here and instead lets WWait end whenever a send is kernel owned or its
\* completion queued, which covers that notification.  Once the socket is
\* closed, a send still in flight completes cancelled (failed).
\* The completion carries no status: on an open socket the kernel may also
\* complete a send failed (a non-zero Status or a short BytesTransferred),
\* which rocketnet_rio_retire_send counts SEND_FAILED instead of
\* PACKETS_SENT and otherwise retires exactly as a good one (the slot and
\* the phase's outstanding count are freed the same way).  So the model
\* checks that each send is retired once, not that it was delivered.
SendComplete(p) ==
    /\ infl[p] # <<>>
    /\ scq' = Append(scq, <<p, Head(infl[p])>>)
    /\ infl' = [infl EXCEPT ![p] = Tail(infl[p])]
    /\ cancelled' = IF closed THEN cancelled \cup {Head(infl[p])} ELSE cancelled
    /\ UNCHANGED rvars
    /\ UNCHANGED <<sent, staged, state, phase, sready, inhand, pend, done, dropped, failed>>
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
    /\ UNCHANGED <<arrived, accepted, retry, retired, data, hvars>>
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
    /\ UNCHANGED <<state, phase, sready, inhand, pend, infl, scq, done, dropped, cancelled, failed>>
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
    /\ UNCHANGED <<sent, inhand, pend, infl, scq, done, dropped, cancelled, failed>>
    /\ UNCHANGED <<evt, armed, stopping, closed, exited, freed, wpc, progress, stopwaits, countedbusy, stoprefused, waited, abandoned>>

\* rocketnet_rio_lane_receive_packets, one call (RNReceiveHandoff Call; busy:
\* the try-acquire failed).  The last call's views have expired, so a
\* finished tail is emptied.  On a successful try main begins
\* (rocketnet_receive_handoff_begin): when the worker has left main's bank
\* and the tail is finished, the tail is freed, main's bank becomes the tail
\* with its final count and main starts on the worker's bank; main's limit
\* is its bank's committed count.  Then main returns the tail's unread
\* datagrams and its own bank's up to the limit, within the view.  Main
\* wakes the worker when it starts on a full bank (WakeOnReturn).  late
\* records a call that took the mutex with no backlog and room to spare and
\* still left a committed datagram unreturned.
ReceiveCall(busy) ==
    /\ ~stopping
    /\ LET d0 == IF TailFinished THEN [data EXCEPT ![tbank] = <<>>] ELSE data
           adv == ~busy /\ mbank # wbank /\ TailFinished
           t2 == IF adv THEN mbank ELSE tbank
           m2 == IF adv THEN wbank ELSE mbank
           tc == IF adv THEN mcur ELSE tcur
           te == IF adv THEN committed[mbank] ELSE tend
           mc == IF adv THEN 0 ELSE mcur
           ml == IF busy THEN mlim
                 ELSE IF ReadOnlyLeft /\ m2 = wbank THEN (IF adv THEN 0 ELSE mlim)
                 ELSE committed[m2]
           nt == Min(View, te - tc)
           nm == Min(View - nt, ml - mc)
           got == [i \in 1..nt |-> <<t2, tc + i, d0[t2][tc + i]>>]
                  \o [i \in 1..nm |-> <<m2, mc + i, d0[m2][mc + i]>>]
           backlog == ~(TailFinished /\ mcur >= mlim)
       IN /\ data' = d0
          /\ fbank' = IF adv THEN tbank ELSE fbank
          /\ tbank' = t2 /\ mbank' = m2
          /\ tcur' = tc + nt /\ tend' = te
          /\ mcur' = mc + nm /\ mlim' = ml
          /\ view' = got
          /\ consumed' = consumed \o [i \in 1..Len(got) |-> got[i][3]]
          /\ late' = (late \/ (~busy /\ ~backlog /\ Len(got) < View
                               /\ (te - tc - nt) + (committed[m2] - mc - nm) > 0))
          /\ wake' = (wake \/ (WakeOnReturn /\ adv /\ ml >= Cap))
    /\ UNCHANGED <<arrived, accepted, posted, retry, retired, rcq, wbank, committed, drained, stopdrops, stopcounted>>
    /\ UNCHANGED svars
    /\ UNCHANGED <<evt, armed, stopping, closed, exited, freed, wpc, progress, stopwaits, countedbusy, stoprefused, waited, abandoned>>

\* a successful try
ReceivePackets == ReceiveCall(FALSE)

\* a failed try: main returns what it already held, up to its earlier
\* limit.  This over-approximates contention: the try may fail at any time.
TryFail == ReceiveCall(TRUE)

\* rocketnet_server_rio_stop: stopping, then the wake
Stop ==
    /\ ~stopping
    /\ stopping' = TRUE
    /\ wake' = TRUE
    /\ UNCHANGED rvars /\ UNCHANGED svars
    /\ UNCHANGED <<evt, armed, closed, exited, freed, wpc, progress, stopwaits, countedbusy, stoprefused, waited, abandoned>>

\* rocketnet_server_rio_destroy after stopped: frees the registered region
Destroy ==
    /\ exited
    /\ ~freed
    /\ freed' = TRUE
    /\ UNCHANGED rvars /\ UNCHANGED svars
    /\ UNCHANGED <<wake, evt, armed, stopping, closed, exited, wpc, progress, stopwaits, countedbusy, stoprefused, waited, abandoned>>

\* main polls stopped(), or idles after destroy
Poll ==
    /\ stopping
    /\ UNCHANGED vars

\* rocketnet_rio_count_stopped: main, once, when stopped() first finds the
\* worker exited, so main owns every bank: what was banked and never
\* returned is counted RECEIVE_DROP_STOPPED, and main's cursors move past
\* it (main takes nothing after stop).
StopCount ==
    /\ exited
    /\ ~stopcounted
    /\ stopcounted' = TRUE
    /\ stopdrops' = Unread
    /\ tcur' = tend
    /\ mcur' = Len(data[mbank])
    /\ mlim' = Len(data[mbank])
    /\ UNCHANGED <<arrived, accepted, posted, retry, retired, rcq, data, drained>>
    /\ UNCHANGED <<wbank, mbank, tbank, fbank, committed, tend, view, consumed, late>>
    /\ UNCHANGED svars /\ UNCHANGED cvars

\* ------------------------------------------------------------------------
\* The I/O worker.

WTop ==
    /\ wpc = "top"
    /\ wpc' = IF stopping THEN "stop" ELSE IF RetireFirst THEN "first" ELSE "take"
    /\ waited' = IF stopping THEN {} ELSE {p \in Phases : state[p] = "posted" /\ pend[p] = <<>> /\ infl[p] = <<>> /\ inhand # p}
    /\ progress' = FALSE
    /\ UNCHANGED rvars /\ UNCHANGED svars
    /\ UNCHANGED <<wake, evt, armed, stopping, closed, exited, freed, stopwaits, countedbusy, stoprefused, abandoned>>

\* every posted phase with nothing outstanding, returned to main as idle
DrainedIdle == [p \in Phases |-> IF state[p] = "posted" /\ ~Outstanding(p) THEN "idle" ELSE state[p]]

\* under the mutex: return the drained phases (RetireFirst), then take the
\* oldest handed phase; main now sees it posted
WTake ==
    /\ wpc = "take"
    /\ LET base == IF RetireFirst THEN DrainedIdle ELSE state
       IN IF sready # <<>>
          THEN /\ inhand' = Head(sready)
               /\ sready' = Tail(sready)
               /\ state' = [base EXCEPT ![Head(sready)] = "posted"]
               /\ wpc' = "post"
          ELSE /\ UNCHANGED <<inhand, sready>>
               /\ state' = base
               /\ wpc' = "retire"
    /\ UNCHANGED rvars
    /\ UNCHANGED <<sent, staged, phase, pend, infl, scq, done, dropped, cancelled, failed>>
    /\ UNCHANGED <<wake, evt, armed, stopping, closed, exited, freed, progress, stopwaits, countedbusy, stoprefused, waited, abandoned>>

\* RIOSendEx with RIO_MSG_DEFER for each finished slot: the kernel owns
\* the ones it accepted.  The kernel may refuse any of them (refused): a
\* refused post has no completion, its slot is free again and the packet
\* is counted SEND_FAILED (failed).  CountRefusedPost FALSE loses the
\* refused packet uncounted.
WPost ==
    /\ wpc = "post"
    /\ \E refused \in SUBSET Range(staged[inhand]) :
          /\ pend' = [pend EXCEPT ![inhand] = SelectSeq(staged[inhand], LAMBDA x : x \notin refused)]
          /\ failed' = IF CountRefusedPost THEN failed \cup refused ELSE failed
    /\ inhand' = NoPhase
    /\ wpc' = "commit"
    /\ progress' = TRUE
    /\ UNCHANGED rvars
    /\ UNCHANGED <<sent, staged, state, phase, sready, infl, scq, done, dropped, cancelled>>
    /\ UNCHANGED <<wake, evt, armed, stopping, closed, exited, freed, stopwaits, countedbusy, stoprefused, waited, abandoned>>

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
    /\ UNCHANGED <<sent, staged, state, phase, sready, inhand, scq, done, dropped, cancelled, failed>>
    /\ UNCHANGED <<wake, evt, armed, stopping, closed, exited, freed, progress, stopwaits, countedbusy, stoprefused, waited, abandoned>>

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
    /\ UNCHANGED <<sent, staged, state, phase, sready, inhand, dropped, cancelled, failed>>
    /\ UNCHANGED <<wake, evt, armed, stopping, closed, exited, freed, stopwaits, countedbusy, stoprefused, waited, abandoned>>

\* Before it takes a phase the worker retires every send completion (and
\* retries a refused commit), so the take below returns the phases that
\* drained to main as idle: a flush then finds its next phase idle unless
\* the kernel really is behind.
WRetireFirst ==
    /\ wpc = "first"
    /\ \/ CommitSuccess
       \/ UNCHANGED <<pend, infl>>
    /\ done' = done \o [i \in 1..Len(scq) |-> scq[i][2]]
    /\ scq' = <<>>
    /\ progress' = (progress \/ scq # <<>>)
    /\ wpc' = "take"
    /\ UNCHANGED rvars
    /\ UNCHANGED <<sent, staged, state, phase, sready, inhand, dropped, cancelled, failed>>
    /\ UNCHANGED <<wake, evt, armed, stopping, closed, exited, freed, stopwaits, countedbusy, stoprefused, waited, abandoned>>

\* Retry uses one stopping read before its whole loop. Each completion
\* checks stopping separately after copying; a Stop during this aggregate
\* pass can therefore leave a re-posted prefix and a retired suffix.
\* stopDuring summarizes that one Stop interleaving, including its wake.
\* It does not introduce a second worker or a new fairness assumption.
\* The prefilter's refusals (Refused, in rocketnet_rio_complete_receive) take
\* no room, so the worker dequeues while its bank has room and it has
\* taken at most one bank of completions (Slices): it stops with the queue
\* empty, the room used up or that budget spent.
Banked(k) == Len(Pass(NonZero(SubSeq(rcq, 1, k))))
DequeueCounts ==
    IF ~BoundedDequeue THEN {Len(rcq)}
    ELSE LET room == Cap - Len(data[wbank])
         IN {k \in 0..Len(rcq) :
                /\ k <= Slices
                /\ (room = 0 => k = 0)
                /\ Banked(k) <= room
                /\ (k = Len(rcq) \/ Banked(k) = room \/ k = Slices)}

WDequeue ==
    /\ wpc = "dequeue"
    /\ \E k \in DequeueCounts :
          \E stopDuring \in BOOLEAN, attemptRetry \in BOOLEAN, repost \in 0..k :
             /\ stopDuring => ~stopping
             /\ stopping => (~attemptRetry /\ repost = 0)
             /\ (~stopping /\ ~stopDuring) => (attemptRetry /\ repost = k)
             /\ ~attemptRetry => repost = 0
             /\ LET attempted == IF attemptRetry THEN retry ELSE 0
                    got == Pass(NonZero(SubSeq(rcq, 1, k)))
                    keep == IF Len(data[wbank]) + Len(got) <= Cap THEN got
                            ELSE SubSeq(got, 1, Cap - Len(data[wbank]))
                IN \E refused \in 0..(attempted + repost) :
                     /\ data' = [data EXCEPT ![wbank] = data[wbank] \o keep]
                     /\ rcq' = SubSeq(rcq, k + 1, Len(rcq))
                     /\ posted' = posted + attempted + repost - refused
                     /\ retry' = retry - attempted + refused
                     /\ retired' = retired + k - repost
                     /\ progress' = (progress \/ k > 0)
             /\ stopping' = (stopping \/ stopDuring)
             /\ wake' = (wake \/ stopDuring)
    /\ wpc' = "publish"
    /\ UNCHANGED <<arrived, accepted, hvars>>
    /\ UNCHANGED svars
    /\ UNCHANGED <<evt, armed, closed, exited, freed, stopwaits, countedbusy, stoprefused, waited, abandoned>>

\* under the mutex: a bank holding datagrams commits its count
\* (rocketnet_receive_handoff_commit, RNReceiveHandoff Commit) and, when
\* main has started on it, the worker moves to the free bank, which main
\* emptied before freeing it; then every drained posted phase goes back to
\* main as idle
WPublish ==
    /\ wpc = "publish"
    /\ IF data[wbank] # <<>>
       THEN IF mbank = wbank
            THEN /\ committed' = [committed EXCEPT ![wbank] = Len(data[wbank]), ![fbank] = 0]
                 /\ wbank' = fbank
                 /\ fbank' = NoBank
            ELSE /\ committed' = [committed EXCEPT ![wbank] = Len(data[wbank])]
                 /\ UNCHANGED <<wbank, fbank>>
       ELSE UNCHANGED <<committed, wbank, fbank>>
    /\ state' = [p \in Phases |-> IF state[p] = "posted" /\ ~Outstanding(p) THEN "idle" ELSE state[p]]
    /\ wpc' = IF progress THEN "top" ELSE "arm"
    /\ UNCHANGED <<arrived, accepted, posted, retry, retired, rcq, data, drained>>
    /\ UNCHANGED <<mbank, tbank, mcur, mlim, tcur, tend, view, consumed, stopdrops, stopcounted, late>>
    /\ UNCHANGED <<sent, staged, phase, sready, inhand, pend, infl, scq, done, dropped, cancelled, failed>>
    /\ UNCHANGED <<wake, evt, armed, stopping, closed, exited, freed, progress, stopwaits, countedbusy, stoprefused, waited, abandoned>>

\* nothing progressed: arm RIONotify on the receive queue when its bank
\* has room (a full bank waits on the wake alone).  A non-empty queue sets
\* the event at once.
WArm ==
    /\ wpc = "arm"
    /\ IF Len(data[wbank]) < Cap
       THEN IF rcq # <<>> THEN evt' = TRUE /\ armed' = FALSE
                          ELSE armed' = TRUE /\ UNCHANGED evt
       ELSE UNCHANGED <<evt, armed>>
    /\ wpc' = "wait"
    /\ UNCHANGED rvars /\ UNCHANGED svars
    /\ UNCHANGED <<wake, stopping, closed, exited, freed, progress, stopwaits, countedbusy, stoprefused, waited, abandoned>>

\* WaitForMultipleObjects on the event and the wake (rocketnet_rio_idle_wait).
\* The third branch ends the wait with neither set while a send is kernel
\* owned or its completion queued (the code's RIONotify on the send queue
\* sets the event when one completes) or a re-post was refused (the code's
\* 1 ms wait; a refused commit leaves a send pending, so it is kernel owned
\* here).  The model's wait has no other timeout: the code waits at most
\* 50 ms even with nothing to do, which the model leaves out, so a run
\* where the worker waits forever is, in the code, a wait of up to 50 ms
\* (about three updates at 60 Hz) before the next pass.
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
    /\ UNCHANGED <<armed, stopping, closed, exited, freed, progress, stopwaits, countedbusy, stoprefused, waited, abandoned>>

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
    /\ UNCHANGED <<sent, staged, phase, pend, infl, scq, done, cancelled, failed>>
    /\ UNCHANGED <<wake, evt, armed, stopping, closed, exited, freed, progress, stopwaits, countedbusy, stoprefused, waited, abandoned>>

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
    /\ UNCHANGED <<arrived, accepted, retry, retired, data, hvars>>
    /\ UNCHANGED <<sent, staged, state, phase, sready, inhand, dropped, cancelled, failed>>
    /\ UNCHANGED <<wake, evt, armed, stopping, freed, progress, waited, abandoned>>

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
    /\ UNCHANGED <<sent, staged, state, phase, sready, inhand, pend, infl, dropped, cancelled, failed>>
    /\ UNCHANGED <<wake, evt, armed, stopping, closed, exited, freed, progress, stopwaits, countedbusy, stoprefused, waited, abandoned>>

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
          /\ drained' = drained \o Pass(NonZero(SubSeq(rcq, 1, n)))
          /\ IF DrainLoop /\ n = DrainBatch
             THEN /\ wpc' = "drainrp"
                  /\ UNCHANGED <<exited, stopwaits, countedbusy>>
             ELSE IF posted = 0 /\ rcq' = <<>> /\ scq = <<>> /\ ~KernelOwnsSend
                  THEN /\ wpc' = "exited"
                       /\ exited' = TRUE
                       /\ UNCHANGED <<stopwaits, countedbusy>>
                  ELSE /\ PassFails(fell)
                       /\ UNCHANGED exited
    /\ UNCHANGED <<arrived, accepted, posted, retry, data, mainvars>>
    /\ UNCHANGED svars
    /\ UNCHANGED <<wake, evt, armed, stopping, closed, freed, progress, stoprefused, waited, abandoned>>

\* The socket is open (closesocket was refused) and the kernel owns no
\* request on the region: posted_receives (posted + Len(rcq)) and the
\* outstanding sends are zero.  On create's failure path this holds only
\* when create failed before it posted a receive (Slices = 0): before the
\* close nothing dequeues a receive.
OpenWithNothingPosted == ~closed /\ posted = 0 /\ rcq = <<>> /\ scq = <<>> /\ ~KernelOwnsSend

\* create's failure path after StopDrainWaits + 1 counted passes
\* (rocketnet_rio_create_failed_drain gives up).  With the socket open and
\* nothing posted or outstanding (AbandonUnposted) it abandons the socket
\* (rio->socket = INVALID_SOCKET, counted
\* rocketnet_server_rio_create_abandoned_sockets), and one more pass
\* (rocketnet_rio_lane_stopped) finds nothing to close and drains, so the
\* lane stops and create frees the rest of it.  Otherwise it leaks the
\* transport instead of freeing memory the kernel may still write.
WLeak ==
    /\ Bounded
    /\ wpc = "stop"
    /\ stopwaits = StopDrainWaits + 1
    /\ IF AbandonUnposted /\ OpenWithNothingPosted
       THEN /\ abandoned' = TRUE
            /\ closed' = TRUE
            /\ wpc' = "drains"
       ELSE /\ wpc' = "leaked"
            /\ UNCHANGED <<abandoned, closed>>
    /\ UNCHANGED rvars /\ UNCHANGED svars
    /\ UNCHANGED <<wake, evt, armed, stopping, exited, freed, progress, stopwaits, countedbusy, stoprefused, waited>>

Next ==
    \/ Arrive \/ ArriveDropped \/ \E p \in Phases : SendComplete(p) \/ CancelArrives
    \/ Stage \/ SendPackets \/ ReceivePackets \/ TryFail
    \/ Stop \/ Destroy \/ Poll \/ StopCount
    \/ WTop \/ WRetireFirst \/ WTake \/ WPost \/ WCommit \/ WRetire \/ WDequeue \/ WPublish
    \/ WArm \/ WWait \/ WStop \/ WClose \/ WDrainSend \/ WDrainReceive \/ WLeak

Spec == Init /\ [][Next]_vars

WorkerStep == WTop \/ WRetireFirst \/ WTake \/ WPost \/ WCommit \/ WRetire \/ WDequeue \/ WPublish
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
    /\ SF_vars(~stopping /\ WRetireFirst /\ ~Uncommitted')
    /\ SF_vars(WDequeue /\ retry' = 0)
    /\ SF_vars(SendPackets) /\ SF_vars(ReceivePackets)
    /\ WF_vars(Destroy)

-----------------------------------------------------------------------------

TypeOK ==
    /\ arrived \in 0..MaxDatagrams
    /\ posted \in 0..Slices /\ retry \in 0..Slices /\ retired \in 0..Slices
    /\ data \in [Banks -> Seq(Ids)]
    /\ \A b \in Banks : Len(data[b]) <= Cap
    /\ wbank \in Banks /\ mbank \in Banks /\ tbank \in Banks
    /\ fbank \in Banks \cup {NoBank}
    /\ committed \in [Banks -> 0..Cap]
    /\ {mcur, mlim, tcur, tend} \subseteq 0..Cap
    /\ stopcounted \in BOOLEAN /\ late \in BOOLEAN
    /\ state \in [Phases -> {"fill", "ready", "posted", "idle"}]
    /\ phase \in Phases
    /\ inhand \in Phases \cup {NoPhase}
    /\ cancelled \subseteq SendIds
    /\ failed \subseteq SendIds
    /\ stopwaits \in 0..(StopDrainWaits + 1)
    /\ countedbusy \in BOOLEAN
    /\ stoprefused \in BOOLEAN
    /\ waited \subseteq Phases
    /\ abandoned \in BOOLEAN

\* Every slice stays accounted through stop, close and drain. A retry
\* entry is unposted and remains accounted even when stop skips its retry.
\* While running, none is retired: refusal cannot shrink receive depth.
SlicesAccounted ==
    /\ posted + Len(rcq) + retry + retired = Slices
    /\ (~stopping => retired = 0)

\* The handoff's roles (RNReceiveHandoff FreeIffShared, NoSharedSlot,
\* FreeIsEmpty): main and the worker may share a bank, the tail and the free
\* bank are nobody else's and a free bank exists exactly while they share;
\* main reads below the committed count and the worker writes at or past
\* it, a bank the worker left keeps its final count while main reads it,
\* and the worker moves only to an empty bank.
ExclusiveOwnership ==
    /\ tbank # mbank /\ tbank # wbank
    /\ (fbank # NoBank) <=> (mbank = wbank)
    /\ fbank # NoBank => fbank \notin {mbank, wbank, tbank}
    /\ mcur <= mlim
    /\ (~stopcounted => mlim <= committed[mbank])
    /\ committed[wbank] <= Len(data[wbank])
    /\ (mbank # wbank => Len(data[mbank]) = committed[mbank])
    /\ tcur <= tend
    /\ (TailFinished \/ tend = Len(data[tbank]))
    /\ (fbank # NoBank => data[fbank] = <<>>)

\* The views main returned stay valid until its next call: no slot a view
\* points at is written or emptied.
ExposedStable ==
    \A k \in 1..Len(view) :
        /\ view[k][2] <= Len(data[view[k][1]])
        /\ data[view[k][1]][view[k][2]] = view[k][3]

\* Exact delivery of every accepted datagram: returned, banked and unread
\* (counted RECEIVE_DROP_STOPPED at the StopCount), dequeued by the stop
\* pass (drained: the stop pass counts each RECEIVE_DROP_STOPPED, and
\* also counts there a datagram the prefilter would refuse, which drained
\* leaves out as ExactDelivery ranges over the datagrams that pass), or
\* still in the queue, each once and in order, through stop.
ExactDelivery ==
    consumed \o stopdrops \o (IF stopcounted THEN <<>> ELSE Unread) \o drained \o Pass(NonZero(rcq))
        = Pass(accepted)

\* RNReceiveHandoff DeliveredByNextCall on the rio lane: a call that took the
\* mutex with no backlog and room to spare returned every datagram the
\* worker had committed.
DeliveredByNextCall == ~late

\* A pass posts a new phase only after it returned to main every posted
\* phase whose sends the kernel had all finished when the pass began.
DrainedReturnedAtPost == wpc = "post" => \A p \in waited : state[p] # "posted"

\* No datagram that fails the prefilter is ever banked or reaches main.
NoGarbageBanked ==
    /\ \A b \in Banks : Range(data[b]) \cap Garbage = {}
    /\ Range(consumed) \cap Garbage = {}

\* The fill threads never write a frame the kernel owns or a phase the
\* worker holds: the open fill phase has no send outstanding.
FillPhaseOwnedByMain ==
    ~stopping =>
        /\ state[phase] = "fill"
        /\ ~Outstanding(phase)
        /\ inhand # phase
        /\ phase \notin Range(sready)

\* No send is retired twice, and none both retired and refused.
NoSendDuplicated ==
    /\ \A i, j \in 1..Len(done) : i # j => done[i] # done[j]
    /\ Range(done) \cap failed = {}

\* The registered region is freed only when the kernel owns no request and
\* every completion has been dequeued.
FreedOnlyWhenDrained ==
    freed => posted = 0 /\ ~KernelOwnsSend /\ rcq = <<>> /\ scq = <<>>

\* Every staged send is retired or counted refused at exit; stop never
\* silently drops a phase, and the close cancels no send in flight.  This
\* is ownership and the one count, not delivery: a retired send is counted
\* PACKETS_SENT or, when the kernel completed it failed on the open socket,
\* SEND_FAILED (SendComplete carries no status).  A refused post is lost,
\* counted SEND_FAILED.
StopSendsHanded == exited => dropped = {}
StopSendsFinished == exited => Range(done) \cup failed = 1..sent /\ cancelled = {}

\* Reachability target, expected to fail: the worker exits with a packet
\* of the stop burst refused at its post.
NotReachedRefusedPostAtExit == ~(exited /\ failed # {})

\* Liveness: every accepted datagram reaches main unless stop begins.
EveryDatagramConsumed ==
    \A i \in Ids \ Refused : (\E j \in 1..Len(accepted) : accepted[j] = i)
                   ~> (stopping \/ \E j \in 1..Len(consumed) : consumed[j] = i)

\* Liveness: a staged packet is sent, or its post refused and counted,
\* unless stop begins.
EveryStagedSent ==
    \A s \in SendIds : (sent >= s) ~> (stopping \/ s \in failed \/ \E j \in 1..Len(done) : done[j] = s)

\* create's bounded inline drain leaks only after StopDrainWaits + 1
\* stalled passes, each of which dequeued no receive completion (a refused
\* commit or closesocket, or a closed socket whose cancellations have not
\* arrived), and only while something is still kernel owned or queued: a
\* pass that dequeues a completion is never counted against the bound.
\* The code guarantees no more: closesocket can succeed and the
\* cancellations arrive after the last wait (NotReachedLateCancelLeak).
\* With closesocket refused and nothing posted or outstanding the drain
\* abandons the socket and frees the lane instead (WLeak, AbandonUnposted);
\* MCRNRioWorkerBrokenLeakUnposted.cfg leaks there and fails this.
LeakOnlyAfterStalls ==
    wpc = "leaked" =>
        /\ ~countedbusy
        /\ posted > 0 \/ rcq # <<>> \/ scq # <<>> \/ KernelOwnsSend

\* Reachability target, expected to fail: create's inline drain leaks after
\* a closesocket that succeeded, its cancellations late.
NotReachedLateCancelLeak == ~(wpc = "leaked" /\ closed /\ ~stoprefused)

\* Reachability target, expected to fail: create's inline drain abandons
\* the socket (closesocket refused, nothing posted) and the lane is freed.
NotReachedAbandonFreed == ~(abandoned /\ freed)

\* Liveness on create's failure path (InlineStop): stop completes and
\* destroy frees the region, or the bounded drain gives up and leaks it,
\* with no fairness on a refused commit or closesocket.
StopCompletes == stopping ~> (freed \/ wpc = "leaked")

\* Liveness on the worker's path, which retries the stop pass without
\* bound: stop completes and destroy frees the region whenever the kernel
\* does not refuse every later commit or closesocket.  No fairness assumes
\* a refusal ends; a kernel that refuses forever falsifies the premise.
\* It also assumes no completion queue is reported corrupt: the model has
\* no RIO_CORRUPT_CQ, on which the code's stop pass fails every time and
\* the lane never stops (the region is never freed, the safe direction).
StopCompletesUnlessRefused == ([]<>~stoprefused) => (stopping ~> freed)

=============================================================================
