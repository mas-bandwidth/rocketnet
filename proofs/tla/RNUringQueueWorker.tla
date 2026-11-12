---------------------------- MODULE RNUringQueueWorker ----------------------------
(***************************************************************************)
(* One queue's worker thread of the io_uring server (layer L3 of the       *)
(* per-queue send): its own ring, on which it submits the queue's sends    *)
(* and its multishot receive, and sleeps in one wait for any completion.   *)
(* Main hands it work (a published bank) and wakes it through an eventfd   *)
(* the worker keeps a READ armed on: a write made while no read is armed  *)
(* stays in the eventfd's count and completes the next read at once.       *)
(*                                                                         *)
(* u is the work waiting to be submitted (runs, abstractly), k the runs in  *)
(* the kernel, K the room the ring has for them.  The kernel answers each   *)
(* run ok or -EAGAIN; -EAGAIN hands the run back (RNUringQueueSend's       *)
(* hand-back) and blocks sending until a POLLOUT poll on the socket fires: *)
(* no spinning.  Main's stop sets quit and writes the eventfd; the worker, *)
(* once it sees quit, cancels its receive, keeps sending what it holds,    *)
(* and once quit is seen and -EAGAIN has answered MaxRetry times it        *)
(* abandons what still waits (the stop's bounded retries: a socket that    *)
(* stays full must not hold the stop forever).  It exits only when nothing *)
(* of its is in the kernel: no run, no receive, no wake read, no poll.     *)
(* Main joins it after.                                                    *)
(*                                                                         *)
(* The checked claims:                                                     *)
(*   ExitDrained   (invariant) the worker exits only with no run in the    *)
(*                 kernel or waiting, no completion unreaped, its receive  *)
(*                 ended and no wake read or poll armed: nothing the       *)
(*                 kernel still writes or reads outlives the thread.       *)
(*   StopFinishes  (liveness) once main stops, the worker exits.           *)
(*   Drains        (liveness) before the stop, work main hands over is     *)
(*                 submitted: a sleeping worker is always woken.           *)
(*                                                                         *)
(* Rule switches, each changed in one reversed witness:                    *)
(*   ExitRule   "drained" (the code) or "early": exit once quit is seen    *)
(*              (the witness "exit before drain", ExitDrained).            *)
(*   RetryRule  "bounded" (the code) or "unbounded": never abandon, so a   *)
(*              socket that stays full holds the stop (StopFinishes).      *)
(*   WakeRule   "armed" (the code: sleep only with the wake read armed, or *)
(*              quit seen) or "skip": sleep without it, so main's write    *)
(*              wakes nothing (StopFinishes).                              *)
(***************************************************************************)
EXTENDS Integers

CONSTANTS U,          \* work main may hand over before the stop
          K,          \* runs the ring holds in the kernel at once
          MaxRetry,   \* -EAGAIN answers after the stop before the worker abandons
          MaxAgain,   \* -EAGAIN answers before the stop (bounds the state space)
          ExitRule,   \* "drained" or "early"
          RetryRule,  \* "bounded" or "unbounded"
          WakeRule    \* "armed" or "skip"

ASSUME U \in Nat /\ K >= 1 /\ MaxRetry >= 1 /\ MaxAgain \in Nat
ASSUME ExitRule \in {"drained", "early"} /\ RetryRule \in {"bounded", "unbounded"} /\ WakeRule \in {"armed", "skip"}

VARIABLES quit,       \* main asked the worker to stop
          seen,       \* the worker has read quit
          st,         \* the worker: "awake", "asleep" or "exited"
          handed,     \* work main handed over so far
          u,          \* work waiting to be submitted
          k,          \* runs in the kernel
          cOk,        \* ok completions posted, not reaped
          cAgain,     \* -EAGAIN completions posted, not reaped
          blocked,    \* a run came back -EAGAIN: no submit until the poll fires
          poll,       \* "none", "armed" or "posted" (its completion ready)
          recv,       \* the multishot receive: "armed", "cancelling", "posted" (its last completion ready) or "ended"
          wakeRead,   \* the eventfd READ: "none", "armed" or "posted"
          wakeCount,  \* the eventfd's count is nonzero
          agains,     \* -EAGAIN answers since the stop was seen, up to MaxRetry
          againsBefore \* -EAGAIN answers before it

vars == <<quit, seen, st, handed, u, k, cOk, cAgain, blocked, poll, recv, wakeRead, wakeCount, agains, againsBefore>>

Init ==
    /\ quit = FALSE /\ seen = FALSE /\ st = "awake"
    /\ handed = 0 /\ u = 0 /\ k = 0 /\ cOk = 0 /\ cAgain = 0
    /\ blocked = FALSE /\ poll = "none" /\ recv = "armed"
    /\ wakeRead = "none" /\ wakeCount = FALSE
    /\ agains = 0 /\ againsBefore = 0

\* main writes the eventfd: a READ armed completes, else the count stays
Signal ==
    IF wakeRead = "armed"
      THEN /\ wakeRead' = "posted" /\ UNCHANGED wakeCount
      ELSE /\ wakeCount' = TRUE /\ UNCHANGED wakeRead

\* main: send_packets publishes a bank and wakes the worker
Publish ==
    /\ ~quit
    /\ handed < U
    /\ handed' = handed + 1
    /\ u' = u + 1
    /\ Signal
    /\ UNCHANGED <<quit, seen, st, k, cOk, cAgain, blocked, poll, recv, agains, againsBefore>>

\* main: stop, then the wake
Stop ==
    /\ ~quit
    /\ quit' = TRUE
    /\ Signal
    /\ UNCHANGED <<seen, st, handed, u, k, cOk, cAgain, blocked, poll, recv, agains, againsBefore>>

---------------------------------------------------------------------------
\* the kernel

KernelComplete(res) ==
    /\ k > 0
    /\ res = "again" => (seen \/ againsBefore < MaxAgain)
    /\ k' = k - 1
    /\ IF res = "ok" THEN cOk' = cOk + 1 /\ UNCHANGED cAgain ELSE cAgain' = cAgain + 1 /\ UNCHANGED cOk
    /\ againsBefore' = IF res = "again" /\ ~seen THEN againsBefore + 1 ELSE againsBefore
    /\ UNCHANGED <<quit, seen, st, handed, u, blocked, poll, recv, wakeRead, wakeCount, agains>>

\* the socket has room again (or the poll is cancelled)
KernelPoll ==
    /\ poll = "armed"
    /\ poll' = "posted"
    /\ UNCHANGED <<quit, seen, st, handed, u, k, cOk, cAgain, blocked, recv, wakeRead, wakeCount, agains, againsBefore>>

\* the cancelled multishot posts its last completion
KernelRecvEnd ==
    /\ recv = "cancelling"
    /\ recv' = "posted"
    /\ UNCHANGED <<quit, seen, st, handed, u, k, cOk, cAgain, blocked, poll, wakeRead, wakeCount, agains, againsBefore>>

\* the armed READ takes the eventfd's count
KernelWakeRead ==
    /\ wakeRead = "armed" /\ wakeCount
    /\ wakeRead' = "posted" /\ wakeCount' = FALSE
    /\ UNCHANGED <<quit, seen, st, handed, u, k, cOk, cAgain, blocked, poll, recv, agains, againsBefore>>

Ready == cOk > 0 \/ cAgain > 0 \/ poll = "posted" \/ recv = "posted" \/ wakeRead = "posted"

\* the wait returns: a completion is ready
KernelWakesWorker ==
    /\ st = "asleep" /\ Ready
    /\ st' = "awake"
    /\ UNCHANGED <<quit, seen, handed, u, k, cOk, cAgain, blocked, poll, recv, wakeRead, wakeCount, agains, againsBefore>>

---------------------------------------------------------------------------
\* the worker, awake

Awake == st = "awake"

Observe ==
    /\ Awake /\ quit /\ ~seen
    /\ seen' = TRUE
    /\ UNCHANGED <<quit, st, handed, u, k, cOk, cAgain, blocked, poll, recv, wakeRead, wakeCount, agains, againsBefore>>

ReapOk ==
    /\ Awake /\ cOk > 0
    /\ cOk' = cOk - 1
    /\ UNCHANGED <<quit, seen, st, handed, u, k, cAgain, blocked, poll, recv, wakeRead, wakeCount, agains, againsBefore>>

\* -EAGAIN: the run back, sending blocked, a POLLOUT poll armed
ReapAgain ==
    /\ Awake /\ cAgain > 0
    /\ cAgain' = cAgain - 1
    /\ u' = u + 1
    /\ blocked' = TRUE
    /\ poll' = IF poll = "none" THEN "armed" ELSE poll
    /\ agains' = IF seen /\ agains < MaxRetry THEN agains + 1 ELSE agains
    /\ UNCHANGED <<quit, seen, st, handed, k, cOk, recv, wakeRead, wakeCount, againsBefore>>

ReapPoll ==
    /\ Awake /\ poll = "posted"
    /\ poll' = "none" /\ blocked' = FALSE
    /\ UNCHANGED <<quit, seen, st, handed, u, k, cOk, cAgain, recv, wakeRead, wakeCount, agains, againsBefore>>

ReapRecvEnd ==
    /\ Awake /\ recv = "posted"
    /\ recv' = "ended"
    /\ UNCHANGED <<quit, seen, st, handed, u, k, cOk, cAgain, blocked, poll, wakeRead, wakeCount, agains, againsBefore>>

ReapWake ==
    /\ Awake /\ wakeRead = "posted"
    /\ wakeRead' = "none"
    /\ UNCHANGED <<quit, seen, st, handed, u, k, cOk, cAgain, blocked, poll, recv, wakeCount, agains, againsBefore>>

Submit ==
    /\ Awake /\ ~blocked /\ u > 0 /\ k < K
    /\ u' = u - 1 /\ k' = k + 1
    /\ UNCHANGED <<quit, seen, st, handed, cOk, cAgain, blocked, poll, recv, wakeRead, wakeCount, agains, againsBefore>>

\* the wake read is armed again until the stop is seen
ArmWake ==
    /\ Awake /\ ~seen /\ wakeRead = "none"
    /\ wakeRead' = "armed"
    /\ UNCHANGED <<quit, seen, st, handed, u, k, cOk, cAgain, blocked, poll, recv, wakeCount, agains, againsBefore>>

\* the stop seen: the receive is cancelled, and the wake read once nothing
\* else is left
CancelRecv ==
    /\ Awake /\ seen /\ recv = "armed"
    /\ recv' = "cancelling"
    /\ UNCHANGED <<quit, seen, st, handed, u, k, cOk, cAgain, blocked, poll, wakeRead, wakeCount, agains, againsBefore>>

CancelWake ==
    /\ Awake /\ seen /\ wakeRead = "armed"
    /\ wakeRead' = "posted"
    /\ UNCHANGED <<quit, seen, st, handed, u, k, cOk, cAgain, blocked, poll, recv, wakeCount, agains, againsBefore>>

\* past MaxRetry answers of -EAGAIN since the stop, what waits is abandoned
Abandon ==
    /\ Awake /\ seen /\ RetryRule = "bounded" /\ agains >= MaxRetry /\ u > 0
    /\ u' = 0
    /\ UNCHANGED <<quit, seen, st, handed, k, cOk, cAgain, blocked, poll, recv, wakeRead, wakeCount, agains, againsBefore>>

Drained == k = 0 /\ u = 0 /\ cOk = 0 /\ cAgain = 0 /\ recv = "ended" /\ wakeRead = "none" /\ poll = "none"

Exit ==
    /\ Awake /\ seen
    /\ (ExitRule = "drained" => Drained)
    /\ st' = "exited"
    /\ UNCHANGED <<quit, seen, handed, u, k, cOk, cAgain, blocked, poll, recv, wakeRead, wakeCount, agains, againsBefore>>

\* nothing to do now: the wait. The code sleeps only with the wake read
\* armed (or the stop seen, when what it waits for is its own work in the
\* kernel), and never with a completion ready or a submit it could make
Sleep ==
    /\ Awake
    /\ ~Ready
    /\ ~(~blocked /\ u > 0 /\ k < K)
    /\ ~(quit /\ ~seen)
    /\ ~(~seen /\ wakeRead = "none" /\ WakeRule = "armed")
    /\ ~(seen /\ (recv = "armed" \/ wakeRead = "armed" \/ (RetryRule = "bounded" /\ agains >= MaxRetry /\ u > 0)))
    /\ ~(seen /\ Drained)
    /\ st' = "asleep"
    /\ UNCHANGED <<quit, seen, handed, u, k, cOk, cAgain, blocked, poll, recv, wakeRead, wakeCount, agains, againsBefore>>

\* the worker has exited and main has joined: stutter
Done ==
    /\ st = "exited"
    /\ UNCHANGED vars

WorkerStep ==
    \/ Observe \/ ReapOk \/ ReapAgain \/ ReapPoll \/ ReapRecvEnd \/ ReapWake
    \/ Submit \/ ArmWake \/ CancelRecv \/ CancelWake \/ Abandon \/ Exit \/ Sleep

KernelStep ==
    \/ \E res \in {"ok", "again"} : KernelComplete(res)
    \/ KernelPoll \/ KernelRecvEnd \/ KernelWakeRead \/ KernelWakesWorker

Next == Publish \/ Stop \/ WorkerStep \/ KernelStep \/ Done

Spec == Init /\ [][Next]_vars

\* the worker and the kernel keep going; main may stop or not
Fairness ==
    /\ WF_vars(WorkerStep)
    /\ WF_vars(\E res \in {"ok", "again"} : KernelComplete(res))
    /\ WF_vars(KernelPoll) /\ WF_vars(KernelRecvEnd) /\ WF_vars(KernelWakeRead) /\ WF_vars(KernelWakesWorker)

LiveSpec == Spec /\ Fairness

---------------------------------------------------------------------------

TypeOK ==
    /\ quit \in BOOLEAN /\ seen \in BOOLEAN /\ st \in {"awake", "asleep", "exited"}
    /\ handed \in 0..U /\ u \in 0..U /\ k \in 0..K /\ cOk \in 0..K /\ cAgain \in 0..K
    /\ blocked \in BOOLEAN /\ poll \in {"none", "armed", "posted"}
    /\ recv \in {"armed", "cancelling", "posted", "ended"}
    /\ wakeRead \in {"none", "armed", "posted"} /\ wakeCount \in BOOLEAN
    /\ agains \in 0..MaxRetry /\ againsBefore \in 0..MaxAgain

\* nothing of the worker's is in the kernel once it has exited
ExitDrained == st = "exited" => Drained

\* the worker reads quit only after main set it
SeenOnlyAfterQuit == seen => quit

StopFinishes == quit ~> (st = "exited")

\* before the stop, what main hands over is submitted
Drains == [](u > 0 /\ ~quit => <>(u = 0 \/ quit))

=============================================================================
