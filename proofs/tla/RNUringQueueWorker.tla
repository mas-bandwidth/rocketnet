---------------------------- MODULE RNUringQueueWorker ----------------------------
(***************************************************************************)
(* One queue's worker thread of the io_uring server (layer L3 of the       *)
(* per-queue send, sdk/rocketnet_uring_queue_worker.cpp): its own ring, on *)
(* which it submits the queue's sends and its multishot receive, and       *)
(* sleeps in one wait for any completion.  Main publishes banks to it      *)
(* (RNUringQueueSend's handoff) and wakes it through an eventfd the worker *)
(* keeps a READ armed on: a write made while no read is armed stays in the *)
(* eventfd's count and completes the next read at once.                    *)
(*                                                                         *)
(* ready is the banks published and not claimed, held whether the worker   *)
(* holds one, u the runs of the held bank waiting to be submitted, k its    *)
(* runs in the kernel, K the room the ring has for them, p the held bank's  *)
(* packets not yet done.  A claim takes a bank of up to R packets and cuts  *)
(* it into one run to one a packet; a split or a refusal cuts a run finer, *)
(* never past one a packet, so a bank is cut only so far.  The kernel      *)
(* answers each run ok (sent or failed), full (-EAGAIN, and -EINTR,        *)
(* -ENOBUFS, -ENOMEM: KernelAgain) or a segment refusal.  A full answer    *)
(* hands the run back and blocks submits until the worker's retry timer    *)
(* (a 1 ms timeout on its ring) fires or, before the stop, main wakes it:  *)
(* the gso worker's wait.  Never POLLOUT: a socket reports room while the  *)
(* device queue behind it refuses (-ENOBUFS), and a poll for it would      *)
(* spin.  A refusal hands the run back as one run a packet, sent again     *)
(* with no block; a run to a destination known to refuse is split the same *)
(* way before any send, and a run to a destination the socket cannot name *)
(* is dropped without one.  A bank retires once nothing of it waits and    *)
(* none of its runs is in the kernel or unreaped.  The multishot receive   *)
(* may end on its own (at most MaxRecvEnd times) and is armed again before *)
(* the stop.                                                               *)
(*                                                                         *)
(* The server's stop (rocketnet_server_transport_stop) first tells the     *)
(* worker shutdown has begun (BeginStop: the driver's begin_shutdown,      *)
(* ShutdownRule "begin") and asks the transport to stop (Stop: quit and    *)
(* the eventfd write) only once nothing waits: no bank ready or held       *)
(* (send_idle).  From shutdown on, each wait a full answer starts counts   *)
(* toward MaxRetry; a run done (ok) or a new bank claimed starts the count *)
(* again, and past the bound what waits is abandoned, so a socket that     *)
(* stays full cannot keep send_idle false.  Once the server is stopping    *)
(* the kernel may answer full without bound: that is the socket that       *)
(* stays full.  Before it, full answers are bounded by MaxAgain.  Once it  *)
(* sees quit the worker cancels its receive and its wake read, keeps       *)
(* sending what is published, waits on its timer between retries, and      *)
(* exits only when nothing of its is in the kernel or waiting.  Main joins *)
(* it after.  A cancel's own completion changes none of these variables,   *)
(* and the code's exit does not wait for it (the ring is torn down after   *)
(* the join): its reap is ReapCancel, a stutter of the awake worker,       *)
(* outside WorkerStep, so it moves no fairness and no Sleep.               *)
(*                                                                         *)
(* The checked claims:                                                     *)
(*   ExitDrained   (invariant) the worker exits only with no bank held or  *)
(*                 ready, no run in the kernel, no completion unreaped,    *)
(*                 its receive ended and no wake read or timer armed:      *)
(*                 nothing the kernel still writes or reads outlives the   *)
(*                 thread.                                                 *)
(*   BlockedHasTimer (invariant) a blocked worker never sleeps without its *)
(*                 retry timer in the kernel: every wait ends.             *)
(*   ShutdownDrains (liveness) once the server is stopping, nothing waits  *)
(*                 (send_idle), however full the socket stays: the driver  *)
(*                 then asks the stop.                                     *)
(*   StopFinishes  (liveness) once main stops, the worker exits.           *)
(*   Drains        (liveness) before the stop, what main publishes is      *)
(*                 submitted: a sleeping worker is always woken.           *)
(*                                                                         *)
(* Rule switches, each changed in one reversed witness:                    *)
(*   ExitRule     "drained" (the code) or "early": exit once quit is seen  *)
(*                (the witness "exit before drain", ExitDrained).          *)
(*   RetryRule    "bounded" (the code) or "unbounded": never abandon, so a *)
(*                socket that stays full holds the stop (StopFinishes).    *)
(*   WakeRule     "armed" (the code: sleep only with the wake read armed,  *)
(*                or quit seen) or "skip": sleep without it, so main's     *)
(*                write wakes nothing (StopFinishes).                      *)
(*   ShutdownRule "begin" (the code: the driver's begin_shutdown reaches   *)
(*                the worker) or "none" (the transport before the fix:     *)
(*                no begin_shutdown, so a held bank on a full socket is    *)
(*                retried forever, uncounted, send_idle never turns true   *)
(*                and the stop is never asked: ShutdownDrains).            *)
(***************************************************************************)
EXTENDS Integers

CONSTANTS U,            \* banks main may publish before the stop
          R,            \* the most runs waiting in one bank
          K,            \* runs the ring holds in the kernel at once
          MaxRetry,     \* waits after shutdown, with no run done, before the worker abandons
          MaxAgain,     \* full answers before the server stops (bounds the state space)
          MaxRefuse,    \* segment refusals (bounds the state space)
          MaxRecvEnd,   \* times the receive ends on its own (bounds the state space)
          ExitRule,     \* "drained" or "early"
          RetryRule,    \* "bounded" or "unbounded"
          WakeRule,     \* "armed" or "skip"
          ShutdownRule  \* "begin" or "none"

ASSUME U \in Nat /\ R >= 1 /\ K >= 1 /\ MaxRetry >= 1 /\ MaxAgain \in Nat /\ MaxRefuse \in Nat /\ MaxRecvEnd \in Nat
ASSUME ExitRule \in {"drained", "early"} /\ RetryRule \in {"bounded", "unbounded"} /\ WakeRule \in {"armed", "skip"}
ASSUME ShutdownRule \in {"begin", "none"}

VARIABLES stopping,   \* the server is stopping (rocketnet_server_transport_stop was called)
          shutdown,   \* main told the worker shutdown has begun (begin_shutdown, or the stop)
          quit,       \* main asked the worker to stop
          seen,       \* the worker has read quit
          st,         \* the worker: "awake", "asleep" or "exited"
          handed,     \* banks main published so far
          ready,      \* banks published, not claimed
          held,       \* the worker holds a bank
          u,          \* runs of the held bank waiting to be submitted
          k,          \* runs in the kernel
          cOk,        \* ok (sent or failed) completions posted, not reaped
          cAgain,     \* full completions posted, not reaped
          cRef,       \* segment refusals posted, not reaped
          blocked,    \* a run came back full: no submit until the timer fires (or main's wake, before the stop)
          timer,      \* the retry timer: "none", "armed" or "posted" (its completion ready)
          recv,       \* the multishot receive: "armed", "cancelling", "posted" (its last completion ready) or "ended"
          wakeRead,   \* the eventfd READ: "none", "armed", "cancelling" or "posted"
          wakeCount,  \* the eventfd's count is nonzero
          agains,     \* waits after shutdown since a run was done or a bank claimed, up to MaxRetry
          againsBefore, \* full answers before the server stopped
          refusals,   \* segment refusals so far
          recvEnds,   \* times the receive ended on its own so far
          p           \* packets of the held bank not done: waiting, in a run, or in a run answered and unreaped

vars == <<stopping, shutdown, quit, seen, st, handed, ready, held, u, k, cOk, cAgain, cRef, blocked, timer, recv, wakeRead, wakeCount,
          agains, againsBefore, refusals, recvEnds, p>>

Init ==
    /\ stopping = FALSE /\ shutdown = FALSE
    /\ quit = FALSE /\ seen = FALSE /\ st = "awake"
    /\ handed = 0 /\ ready = 0 /\ held = FALSE /\ u = 0 /\ k = 0
    /\ cOk = 0 /\ cAgain = 0 /\ cRef = 0
    /\ blocked = FALSE /\ timer = "none" /\ recv = "armed"
    /\ wakeRead = "none" /\ wakeCount = FALSE
    /\ agains = 0 /\ againsBefore = 0 /\ refusals = 0 /\ recvEnds = 0 /\ p = 0

\* main writes the eventfd: a READ in the kernel completes, else the count stays
Signal ==
    IF wakeRead \in {"armed", "cancelling"}
      THEN /\ wakeRead' = "posted" /\ UNCHANGED wakeCount
      ELSE /\ wakeCount' = TRUE /\ UNCHANGED wakeRead

\* main: send_packets publishes a bank and wakes the worker (while stopping
\* too: the server's last disconnect batches)
Publish ==
    /\ ~quit
    /\ handed < U
    /\ handed' = handed + 1
    /\ ready' = ready + 1
    /\ Signal
    /\ UNCHANGED <<stopping, shutdown, quit, seen, st, held, u, k, cOk, cAgain, cRef, blocked, timer, recv, agains, againsBefore, refusals, recvEnds, p>>

\* main: the server's stop. Under ShutdownRule "begin" the driver's
\* begin_shutdown stores shutdown and wakes the worker; under "none" the
\* transport has no begin_shutdown and the worker learns nothing
BeginStop ==
    /\ ~stopping
    /\ stopping' = TRUE
    /\ IF ShutdownRule = "begin"
         THEN /\ shutdown' = TRUE /\ Signal
         ELSE UNCHANGED <<shutdown, wakeRead, wakeCount>>
    /\ UNCHANGED <<quit, seen, st, handed, ready, held, u, k, cOk, cAgain, cRef, blocked, timer, recv, agains, againsBefore, refusals, recvEnds, p>>

\* main: the transport's stop (rocketnet_uring_queue_worker_stop): shutdown,
\* quit, then the wake. The driver asks it only once send_idle (ShutdownDrains);
\* the transport's free asks it whatever waits, so the worker's rules hold for
\* a stop at any time
Stop ==
    /\ ~quit
    /\ stopping' = TRUE /\ shutdown' = TRUE /\ quit' = TRUE
    /\ Signal
    /\ UNCHANGED <<seen, st, handed, ready, held, u, k, cOk, cAgain, cRef, blocked, timer, recv, agains, againsBefore, refusals, recvEnds, p>>

---------------------------------------------------------------------------
\* the kernel

KernelOk ==
    /\ k > 0
    /\ k' = k - 1 /\ cOk' = cOk + 1
    /\ UNCHANGED <<stopping, shutdown, quit, seen, st, handed, ready, held, u, cAgain, cRef, blocked, timer, recv, wakeRead, wakeCount, agains, againsBefore, refusals, recvEnds, p>>

\* full: -EAGAIN, -EINTR, -ENOBUFS or -ENOMEM
KernelAgain ==
    /\ k > 0
    /\ stopping \/ againsBefore < MaxAgain
    /\ k' = k - 1 /\ cAgain' = cAgain + 1
    /\ againsBefore' = IF ~stopping THEN againsBefore + 1 ELSE againsBefore
    /\ UNCHANGED <<stopping, shutdown, quit, seen, st, handed, ready, held, u, cOk, cRef, blocked, timer, recv, wakeRead, wakeCount, agains, refusals, recvEnds, p>>

KernelRefuse ==
    /\ k > 0
    /\ refusals < MaxRefuse
    /\ k' = k - 1 /\ cRef' = cRef + 1
    /\ refusals' = refusals + 1
    /\ UNCHANGED <<stopping, shutdown, quit, seen, st, handed, ready, held, u, cOk, cAgain, blocked, timer, recv, wakeRead, wakeCount, agains, againsBefore, recvEnds, p>>

\* the retry timer expires: time passes, whatever the socket does
KernelTimer ==
    /\ timer = "armed"
    /\ timer' = "posted"
    /\ UNCHANGED <<stopping, shutdown, quit, seen, st, handed, ready, held, u, k, cOk, cAgain, cRef, blocked, recv, wakeRead, wakeCount, agains, againsBefore, refusals, recvEnds, p>>

\* the cancelled multishot posts its last completion
KernelRecvEnd ==
    /\ recv = "cancelling"
    /\ recv' = "posted"
    /\ UNCHANGED <<stopping, shutdown, quit, seen, st, handed, ready, held, u, k, cOk, cAgain, cRef, blocked, timer, wakeRead, wakeCount, agains, againsBefore, refusals, recvEnds, p>>

\* the multishot ends on its own (no buffer free, an error): its last
\* completion; no fairness, it may never happen
KernelRecvEndsItself ==
    /\ recv = "armed" /\ recvEnds < MaxRecvEnd
    /\ recv' = "posted" /\ recvEnds' = recvEnds + 1
    /\ UNCHANGED <<stopping, shutdown, quit, seen, st, handed, ready, held, u, k, cOk, cAgain, cRef, blocked, timer, wakeRead, wakeCount, agains, againsBefore, refusals, p>>

\* the armed READ takes the eventfd's count
KernelWakeRead ==
    /\ wakeRead \in {"armed", "cancelling"} /\ wakeCount
    /\ wakeRead' = "posted" /\ wakeCount' = FALSE
    /\ UNCHANGED <<stopping, shutdown, quit, seen, st, handed, ready, held, u, k, cOk, cAgain, cRef, blocked, timer, recv, agains, againsBefore, refusals, recvEnds, p>>

\* the cancelled READ posts its completion
KernelWakeEnd ==
    /\ wakeRead = "cancelling"
    /\ wakeRead' = "posted"
    /\ UNCHANGED <<stopping, shutdown, quit, seen, st, handed, ready, held, u, k, cOk, cAgain, cRef, blocked, timer, recv, wakeCount, agains, againsBefore, refusals, recvEnds, p>>

\* a held bank's runs not done: waiting, in the kernel or unreaped
Runs == u + k + cOk + cAgain + cRef

\* the packets left once n runs remain, at most hi: none when no run does,
\* else at least one a run
Left(n, hi) == IF n = 0 THEN {0} ELSE n..hi

Ready == cOk > 0 \/ cAgain > 0 \/ cRef > 0 \/ timer = "posted" \/ recv = "posted" \/ wakeRead = "posted"

\* the wait returns: a completion is ready
KernelWakesWorker ==
    /\ st = "asleep" /\ Ready
    /\ st' = "awake"
    /\ UNCHANGED <<stopping, shutdown, quit, seen, handed, ready, held, u, k, cOk, cAgain, cRef, blocked, timer, recv, wakeRead, wakeCount, agains, againsBefore, refusals, recvEnds, p>>

---------------------------------------------------------------------------
\* the worker, awake

Awake == st = "awake"

Abandoning == shutdown /\ RetryRule = "bounded" /\ agains >= MaxRetry

\* from here the receive and the wake read are cancelled; a wait goes on
\* until its timer fires
Observe ==
    /\ Awake /\ quit /\ ~seen
    /\ seen' = TRUE
    /\ UNCHANGED <<stopping, shutdown, quit, st, handed, ready, held, u, k, cOk, cAgain, cRef, blocked, timer, recv, wakeRead, wakeCount, agains, againsBefore, refusals, recvEnds, p>>

\* a run done (sent or failed): its packets, at least one, are done, and the
\* count of waits starts again
ReapOk ==
    /\ Awake /\ cOk > 0
    /\ cOk' = cOk - 1
    /\ p' \in Left(Runs - 1, p - 1)
    /\ agains' = 0
    /\ UNCHANGED <<stopping, shutdown, quit, seen, st, handed, ready, held, u, k, cAgain, cRef, blocked, timer, recv, wakeRead, wakeCount, againsBefore, refusals, recvEnds>>

\* full: the run back and sending blocked until the timer fires; after
\* shutdown the answer that starts a wait counts toward the bound (one a
\* wait: the rest of the burst refused with it does not)
ReapAgain ==
    /\ Awake /\ cAgain > 0
    /\ cAgain' = cAgain - 1
    /\ u' = u + 1
    /\ blocked' = TRUE
    /\ agains' = IF shutdown /\ ~blocked /\ agains < MaxRetry THEN agains + 1 ELSE agains
    /\ UNCHANGED <<stopping, shutdown, quit, seen, st, handed, ready, held, k, cOk, cRef, timer, recv, wakeRead, wakeCount, againsBefore, refusals, recvEnds, p>>

\* a refusal: the run back as one or more runs, at most one a packet, with
\* no block
ReapRefused ==
    /\ Awake /\ cRef > 0
    /\ cRef' = cRef - 1
    /\ u' \in (u + 1)..(u + 1 + p - Runs)
    /\ UNCHANGED <<stopping, shutdown, quit, seen, st, handed, ready, held, k, cOk, cAgain, blocked, timer, recv, wakeRead, wakeCount, agains, againsBefore, refusals, recvEnds, p>>

\* the timer fired: the wait is over
ReapTimer ==
    /\ Awake /\ timer = "posted"
    /\ timer' = "none" /\ blocked' = FALSE
    /\ UNCHANGED <<stopping, shutdown, quit, seen, st, handed, ready, held, u, k, cOk, cAgain, cRef, recv, wakeRead, wakeCount, agains, againsBefore, refusals, recvEnds, p>>

ReapRecvEnd ==
    /\ Awake /\ recv = "posted"
    /\ recv' = "ended"
    /\ UNCHANGED <<stopping, shutdown, quit, seen, st, handed, ready, held, u, k, cOk, cAgain, cRef, blocked, timer, wakeRead, wakeCount, agains, againsBefore, refusals, recvEnds, p>>

\* main's wake: before the stop it also ends a wait (main published, or
\* began the shutdown); once quit is seen only the timer does
ReapWake ==
    /\ Awake /\ wakeRead = "posted"
    /\ wakeRead' = "none"
    /\ blocked' = (blocked /\ seen)
    /\ UNCHANGED <<stopping, shutdown, quit, seen, st, handed, ready, held, u, k, cOk, cAgain, cRef, timer, recv, wakeCount, agains, againsBefore, refusals, recvEnds, p>>

\* a cancel's own completion reaped: nothing of these variables changes
ReapCancel ==
    /\ Awake
    /\ UNCHANGED vars

\* a bank all of whose runs are done goes back to the free FIFO
Retire ==
    /\ Awake /\ held /\ u = 0 /\ k = 0 /\ cOk = 0 /\ cAgain = 0 /\ cRef = 0 /\ p = 0
    /\ held' = FALSE
    /\ UNCHANGED <<stopping, shutdown, quit, seen, st, handed, ready, u, k, cOk, cAgain, cRef, blocked, timer, recv, wakeRead, wakeCount, agains, againsBefore, refusals, recvEnds, p>>

\* the oldest ready bank of up to R packets, cut into runs: none when it
\* has none, else from one run to one a packet. A new bank gets the bound's
\* waits afresh (the gso worker's per-bank count)
Claim ==
    /\ Awake /\ ~held /\ ready > 0
    /\ ready' = ready - 1 /\ held' = TRUE
    /\ p' \in 0..R
    /\ u' \in IF p' = 0 THEN {0} ELSE 1..p'
    /\ agains' = 0
    /\ UNCHANGED <<stopping, shutdown, quit, seen, st, handed, k, cOk, cAgain, cRef, blocked, timer, recv, wakeRead, wakeCount, againsBefore, refusals, recvEnds>>

\* the code abandons before it sends: nothing is sent once abandoning
CanSend == Awake /\ held /\ ~blocked /\ ~Abandoning /\ u > 0 /\ k < K

Submit ==
    /\ CanSend
    /\ u' = u - 1 /\ k' = k + 1
    /\ UNCHANGED <<stopping, shutdown, quit, seen, st, handed, ready, held, cOk, cAgain, cRef, blocked, timer, recv, wakeRead, wakeCount, agains, againsBefore, refusals, recvEnds, p>>

\* a run to a destination known to refuse segmentation: one run a packet,
\* so a run of more than one packet (some run has one when Runs < p)
Split ==
    /\ CanSend /\ Runs < p
    /\ u' \in (u + 1)..(u + p - Runs)
    /\ UNCHANGED <<stopping, shutdown, quit, seen, st, handed, ready, held, k, cOk, cAgain, cRef, blocked, timer, recv, wakeRead, wakeCount, agains, againsBefore, refusals, recvEnds, p>>

\* a run to a destination the socket cannot name: dropped, no send
Drop ==
    /\ CanSend
    /\ u' = u - 1
    /\ p' \in Left(Runs - 1, p - 1)
    /\ UNCHANGED <<stopping, shutdown, quit, seen, st, handed, ready, held, k, cOk, cAgain, cRef, blocked, timer, recv, wakeRead, wakeCount, agains, againsBefore, refusals, recvEnds>>

\* past MaxRetry waits after shutdown with no run done, what waits is abandoned
Abandon ==
    /\ Awake /\ Abandoning /\ held /\ u > 0
    /\ u' = 0
    /\ p' \in Left(Runs - u, p - u)
    /\ UNCHANGED <<stopping, shutdown, quit, seen, st, handed, ready, held, k, cOk, cAgain, cRef, blocked, timer, recv, wakeRead, wakeCount, agains, againsBefore, refusals, recvEnds>>

\* the retry timer a full answer asked for, before the stop and after it
ArmTimer ==
    /\ Awake /\ blocked /\ timer = "none"
    /\ timer' = "armed"
    /\ UNCHANGED <<stopping, shutdown, quit, seen, st, handed, ready, held, u, k, cOk, cAgain, cRef, blocked, recv, wakeRead, wakeCount, agains, againsBefore, refusals, recvEnds, p>>

\* a receive that ended on its own is armed again until the stop is seen
ArmRecv ==
    /\ Awake /\ ~seen /\ recv = "ended"
    /\ recv' = "armed"
    /\ UNCHANGED <<stopping, shutdown, quit, seen, st, handed, ready, held, u, k, cOk, cAgain, cRef, blocked, timer, wakeRead, wakeCount, agains, againsBefore, refusals, recvEnds, p>>

\* the wake read is armed again until the stop is seen
ArmWake ==
    /\ Awake /\ ~seen /\ wakeRead = "none"
    /\ wakeRead' = "armed"
    /\ UNCHANGED <<stopping, shutdown, quit, seen, st, handed, ready, held, u, k, cOk, cAgain, cRef, blocked, timer, recv, wakeCount, agains, againsBefore, refusals, recvEnds, p>>

\* the stop seen: the receive and the wake read are cancelled (the timer
\* fires on its own)
CancelRecv ==
    /\ Awake /\ seen /\ recv = "armed"
    /\ recv' = "cancelling"
    /\ UNCHANGED <<stopping, shutdown, quit, seen, st, handed, ready, held, u, k, cOk, cAgain, cRef, blocked, timer, wakeRead, wakeCount, agains, againsBefore, refusals, recvEnds, p>>

CancelWake ==
    /\ Awake /\ seen /\ wakeRead = "armed"
    /\ wakeRead' = "cancelling"
    /\ UNCHANGED <<stopping, shutdown, quit, seen, st, handed, ready, held, u, k, cOk, cAgain, cRef, blocked, timer, recv, wakeCount, agains, againsBefore, refusals, recvEnds, p>>

Drained ==
    /\ ~held /\ ready = 0 /\ u = 0 /\ k = 0 /\ cOk = 0 /\ cAgain = 0 /\ cRef = 0
    /\ recv = "ended" /\ wakeRead = "none" /\ timer = "none"

Exit ==
    /\ Awake /\ seen
    /\ (ExitRule = "drained" => Drained)
    /\ st' = "exited"
    /\ UNCHANGED <<stopping, shutdown, quit, seen, handed, ready, held, u, k, cOk, cAgain, cRef, blocked, timer, recv, wakeRead, wakeCount, agains, againsBefore, refusals, recvEnds, p>>

\* everything the worker does awake but sleep and exit
Busy ==
    \/ Observe \/ ReapOk \/ ReapAgain \/ ReapRefused \/ ReapTimer \/ ReapRecvEnd \/ ReapWake
    \/ Retire \/ Claim \/ Submit \/ Split \/ Drop \/ Abandon
    \/ ArmTimer \/ ArmRecv \/ CancelRecv \/ CancelWake

\* nothing to do now: the wait. The code sleeps only with nothing it could
\* do, the wake read armed (WakeRule "armed") and not able to exit
Sleep ==
    /\ Awake
    /\ ~ENABLED Busy
    /\ ~ENABLED Exit
    /\ WakeRule = "armed" => ~ENABLED ArmWake
    /\ st' = "asleep"
    /\ UNCHANGED <<stopping, shutdown, quit, seen, handed, ready, held, u, k, cOk, cAgain, cRef, blocked, timer, recv, wakeRead, wakeCount, agains, againsBefore, refusals, recvEnds, p>>

\* the worker has exited and main has joined: stutter
Done ==
    /\ st = "exited"
    /\ UNCHANGED vars

WorkerStep == Busy \/ ArmWake \/ Exit \/ Sleep

KernelStep ==
    \/ KernelOk \/ KernelAgain \/ KernelRefuse
    \/ KernelTimer \/ KernelRecvEnd \/ KernelRecvEndsItself \/ KernelWakeRead \/ KernelWakeEnd \/ KernelWakesWorker

\* every action by name (the trace check takes the actions Next names): main,
\* the worker (WorkerStep, and ReapCancel), the kernel (KernelStep), the end
Next ==
    \/ Publish \/ BeginStop \/ Stop
    \/ Observe \/ ReapOk \/ ReapAgain \/ ReapRefused \/ ReapTimer \/ ReapRecvEnd \/ ReapWake \/ ReapCancel
    \/ Retire \/ Claim \/ Submit \/ Split \/ Drop \/ Abandon
    \/ ArmTimer \/ ArmRecv \/ CancelRecv \/ CancelWake
    \/ ArmWake \/ Exit \/ Sleep
    \/ KernelOk \/ KernelAgain \/ KernelRefuse
    \/ KernelTimer \/ KernelRecvEnd \/ KernelRecvEndsItself \/ KernelWakeRead \/ KernelWakeEnd \/ KernelWakesWorker
    \/ Done

Spec == Init /\ [][Next]_vars

\* the worker and the kernel keep going; main may stop or not, and its
\* begin and stop are its own (ShutdownDrains and StopFinishes start from them)
Fairness ==
    /\ WF_vars(WorkerStep)
    /\ WF_vars(KernelOk \/ KernelAgain \/ KernelRefuse)
    /\ WF_vars(KernelTimer) /\ WF_vars(KernelRecvEnd) /\ WF_vars(KernelWakeRead) /\ WF_vars(KernelWakeEnd)
    /\ WF_vars(KernelWakesWorker)

LiveSpec == Spec /\ Fairness

---------------------------------------------------------------------------

TypeOK ==
    /\ stopping \in BOOLEAN /\ shutdown \in BOOLEAN
    /\ quit \in BOOLEAN /\ seen \in BOOLEAN /\ st \in {"awake", "asleep", "exited"}
    /\ handed \in 0..U /\ ready \in 0..U /\ held \in BOOLEAN /\ u \in 0..R /\ k \in 0..K
    /\ cOk \in 0..K /\ cAgain \in 0..K /\ cRef \in 0..K
    /\ blocked \in BOOLEAN /\ timer \in {"none", "armed", "posted"}
    /\ recv \in {"armed", "cancelling", "posted", "ended"}
    /\ wakeRead \in {"none", "armed", "cancelling", "posted"} /\ wakeCount \in BOOLEAN
    /\ agains \in 0..MaxRetry /\ againsBefore \in 0..MaxAgain /\ refusals \in 0..MaxRefuse
    /\ recvEnds \in 0..MaxRecvEnd /\ p \in 0..R

\* every run holds at least one packet of the held bank, and a bank not held
\* has none left
PacketsCoverRuns == Runs <= p /\ (~held => p = 0)

\* a blocked worker asleep has its timer in the kernel: every wait ends
BlockedHasTimer == (st = "asleep" /\ blocked) => timer # "none"

\* a held bank's runs: waiting, in the kernel or unreaped, never past R
RunsBounded == Runs <= R

\* nothing of the worker's is in the kernel once it has exited
ExitDrained == st = "exited" => Drained

\* the worker reads quit only after main set it, and quit comes with shutdown
SeenOnlyAfterQuit == (seen => quit) /\ (quit => shutdown /\ stopping)

StopFinishes == quit ~> (st = "exited")

\* once the server is stopping nothing waits (send_idle: no bank ready or
\* held), or the worker has gone
ShutdownDrains == stopping ~> ((ready = 0 /\ ~held) \/ st = "exited")

\* before the stop, what main publishes is submitted
Drains == [](((ready > 0 \/ u > 0) /\ ~quit) => <>((ready = 0 /\ u = 0) \/ quit))

=============================================================================
