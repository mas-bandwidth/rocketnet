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
(* answers each run ok                                                     *)
(* (sent or failed), -EAGAIN or a segment refusal.  -EAGAIN hands the run   *)
(* back; before the stop it blocks submits until a POLLOUT poll on the     *)
(* socket fires: no spinning.  A refusal hands the run back as one run a   *)
(* packet, sent again with no block; a run to a destination known to       *)
(* refuse is split the same way before any send, and a run to a            *)
(* destination the socket cannot name is dropped without one.  A bank      *)
(* retires once nothing of it waits and none of its runs is in the kernel  *)
(* or unreaped.  The multishot receive may end on its own (at most         *)
(* MaxRecvEnd times) and is armed again before the stop.                   *)
(*                                                                         *)
(* Main's stop sets quit and writes the eventfd; the worker, once it sees  *)
(* quit, cancels its receive, its wake read and any poll, keeps sending    *)
(* what is published, and retries a run -EAGAIN hands back at once, with   *)
(* no poll: a socket that stays full fires no poll, so no poll may hold    *)
(* the stop.  Once -EAGAIN has answered MaxRetry times since the stop was  *)
(* seen, what waits is abandoned (the stop's bounded retries).  After the  *)
(* stop the kernel may answer -EAGAIN without bound: that is the socket    *)
(* that stays full.  Before it, WF on KernelPoll is the socket draining.   *)
(* The worker exits only when nothing of its is in the kernel or waiting.  *)
(* Main joins it after.  A cancel's own completion is not modelled: it     *)
(* changes none of these variables, and the code's exit does not wait for  *)
(* it (the ring is torn down after the join).                              *)
(*                                                                         *)
(* The checked claims:                                                     *)
(*   ExitDrained   (invariant) the worker exits only with no bank held or  *)
(*                 ready, no run in the kernel, no completion unreaped,    *)
(*                 its receive ended and no wake read or poll armed:       *)
(*                 nothing the kernel still writes or reads outlives the   *)
(*                 thread.                                                 *)
(*   StopFinishes  (liveness) once main stops, the worker exits.           *)
(*   Drains        (liveness) before the stop, what main publishes is      *)
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

CONSTANTS U,          \* banks main may publish before the stop
          R,          \* the most runs waiting in one bank
          K,          \* runs the ring holds in the kernel at once
          MaxRetry,   \* -EAGAIN answers after the stop before the worker abandons
          MaxAgain,   \* -EAGAIN answers before the stop (bounds the state space)
          MaxRefuse,  \* segment refusals (bounds the state space)
          MaxRecvEnd, \* times the receive ends on its own (bounds the state space)
          ExitRule,   \* "drained" or "early"
          RetryRule,  \* "bounded" or "unbounded"
          WakeRule    \* "armed" or "skip"

ASSUME U \in Nat /\ R >= 1 /\ K >= 1 /\ MaxRetry >= 1 /\ MaxAgain \in Nat /\ MaxRefuse \in Nat /\ MaxRecvEnd \in Nat
ASSUME ExitRule \in {"drained", "early"} /\ RetryRule \in {"bounded", "unbounded"} /\ WakeRule \in {"armed", "skip"}

VARIABLES quit,       \* main asked the worker to stop
          seen,       \* the worker has read quit
          st,         \* the worker: "awake", "asleep" or "exited"
          handed,     \* banks main published so far
          ready,      \* banks published, not claimed
          held,       \* the worker holds a bank
          u,          \* runs of the held bank waiting to be submitted
          k,          \* runs in the kernel
          cOk,        \* ok (sent or failed) completions posted, not reaped
          cAgain,     \* -EAGAIN completions posted, not reaped
          cRef,       \* segment refusals posted, not reaped
          blocked,    \* a run came back -EAGAIN: no submit until the poll fires
          poll,       \* "none", "armed", "cancelling" or "posted" (its completion ready)
          recv,       \* the multishot receive: "armed", "cancelling", "posted" (its last completion ready) or "ended"
          wakeRead,   \* the eventfd READ: "none", "armed", "cancelling" or "posted"
          wakeCount,  \* the eventfd's count is nonzero
          agains,     \* -EAGAIN answers since the stop was seen, up to MaxRetry
          againsBefore, \* -EAGAIN answers before it
          refusals,   \* segment refusals so far
          recvEnds,   \* times the receive ended on its own so far
          p           \* packets of the held bank not done: waiting, in a run, or in a run answered and unreaped

vars == <<quit, seen, st, handed, ready, held, u, k, cOk, cAgain, cRef, blocked, poll, recv, wakeRead, wakeCount, agains, againsBefore, refusals, recvEnds, p>>

Init ==
    /\ quit = FALSE /\ seen = FALSE /\ st = "awake"
    /\ handed = 0 /\ ready = 0 /\ held = FALSE /\ u = 0 /\ k = 0
    /\ cOk = 0 /\ cAgain = 0 /\ cRef = 0
    /\ blocked = FALSE /\ poll = "none" /\ recv = "armed"
    /\ wakeRead = "none" /\ wakeCount = FALSE
    /\ agains = 0 /\ againsBefore = 0 /\ refusals = 0 /\ recvEnds = 0 /\ p = 0

\* main writes the eventfd: a READ in the kernel completes, else the count stays
Signal ==
    IF wakeRead \in {"armed", "cancelling"}
      THEN /\ wakeRead' = "posted" /\ UNCHANGED wakeCount
      ELSE /\ wakeCount' = TRUE /\ UNCHANGED wakeRead

\* main: send_packets publishes a bank and wakes the worker
Publish ==
    /\ ~quit
    /\ handed < U
    /\ handed' = handed + 1
    /\ ready' = ready + 1
    /\ Signal
    /\ UNCHANGED <<quit, seen, st, held, u, k, cOk, cAgain, cRef, blocked, poll, recv, agains, againsBefore, refusals, recvEnds, p>>

\* main: stop, then the wake
Stop ==
    /\ ~quit
    /\ quit' = TRUE
    /\ Signal
    /\ UNCHANGED <<seen, st, handed, ready, held, u, k, cOk, cAgain, cRef, blocked, poll, recv, agains, againsBefore, refusals, recvEnds, p>>

---------------------------------------------------------------------------
\* the kernel

KernelOk ==
    /\ k > 0
    /\ k' = k - 1 /\ cOk' = cOk + 1
    /\ UNCHANGED <<quit, seen, st, handed, ready, held, u, cAgain, cRef, blocked, poll, recv, wakeRead, wakeCount, agains, againsBefore, refusals, recvEnds, p>>

KernelAgain ==
    /\ k > 0
    /\ seen \/ againsBefore < MaxAgain
    /\ k' = k - 1 /\ cAgain' = cAgain + 1
    /\ againsBefore' = IF ~seen THEN againsBefore + 1 ELSE againsBefore
    /\ UNCHANGED <<quit, seen, st, handed, ready, held, u, cOk, cRef, blocked, poll, recv, wakeRead, wakeCount, agains, refusals, recvEnds, p>>

KernelRefuse ==
    /\ k > 0
    /\ refusals < MaxRefuse
    /\ k' = k - 1 /\ cRef' = cRef + 1
    /\ refusals' = refusals + 1
    /\ UNCHANGED <<quit, seen, st, handed, ready, held, u, cOk, cAgain, blocked, poll, recv, wakeRead, wakeCount, agains, againsBefore, recvEnds, p>>

\* the socket has room again, or the poll is cancelled
KernelPoll ==
    /\ poll \in {"armed", "cancelling"}
    /\ poll' = "posted"
    /\ UNCHANGED <<quit, seen, st, handed, ready, held, u, k, cOk, cAgain, cRef, blocked, recv, wakeRead, wakeCount, agains, againsBefore, refusals, recvEnds, p>>

\* the cancelled multishot posts its last completion
KernelRecvEnd ==
    /\ recv = "cancelling"
    /\ recv' = "posted"
    /\ UNCHANGED <<quit, seen, st, handed, ready, held, u, k, cOk, cAgain, cRef, blocked, poll, wakeRead, wakeCount, agains, againsBefore, refusals, recvEnds, p>>

\* the multishot ends on its own (no buffer free, an error): its last
\* completion; no fairness, it may never happen
KernelRecvEndsItself ==
    /\ recv = "armed" /\ recvEnds < MaxRecvEnd
    /\ recv' = "posted" /\ recvEnds' = recvEnds + 1
    /\ UNCHANGED <<quit, seen, st, handed, ready, held, u, k, cOk, cAgain, cRef, blocked, poll, wakeRead, wakeCount, agains, againsBefore, refusals, p>>

\* the armed READ takes the eventfd's count
KernelWakeRead ==
    /\ wakeRead \in {"armed", "cancelling"} /\ wakeCount
    /\ wakeRead' = "posted" /\ wakeCount' = FALSE
    /\ UNCHANGED <<quit, seen, st, handed, ready, held, u, k, cOk, cAgain, cRef, blocked, poll, recv, agains, againsBefore, refusals, recvEnds, p>>

\* the cancelled READ posts its completion
KernelWakeEnd ==
    /\ wakeRead = "cancelling"
    /\ wakeRead' = "posted"
    /\ UNCHANGED <<quit, seen, st, handed, ready, held, u, k, cOk, cAgain, cRef, blocked, poll, recv, wakeCount, agains, againsBefore, refusals, recvEnds, p>>

\* a held bank's runs not done: waiting, in the kernel or unreaped
Runs == u + k + cOk + cAgain + cRef

\* the packets left once n runs remain, at most hi: none when no run does,
\* else at least one a run
Left(n, hi) == IF n = 0 THEN {0} ELSE n..hi

Ready == cOk > 0 \/ cAgain > 0 \/ cRef > 0 \/ poll = "posted" \/ recv = "posted" \/ wakeRead = "posted"

\* the wait returns: a completion is ready
KernelWakesWorker ==
    /\ st = "asleep" /\ Ready
    /\ st' = "awake"
    /\ UNCHANGED <<quit, seen, handed, ready, held, u, k, cOk, cAgain, cRef, blocked, poll, recv, wakeRead, wakeCount, agains, againsBefore, refusals, recvEnds, p>>

---------------------------------------------------------------------------
\* the worker, awake

Awake == st = "awake"

Abandoning == seen /\ RetryRule = "bounded" /\ agains >= MaxRetry

\* from here nothing blocks: -EAGAIN retries at once and the poll is cancelled
Observe ==
    /\ Awake /\ quit /\ ~seen
    /\ seen' = TRUE /\ blocked' = FALSE
    /\ UNCHANGED <<quit, st, handed, ready, held, u, k, cOk, cAgain, cRef, poll, recv, wakeRead, wakeCount, agains, againsBefore, refusals, recvEnds, p>>

\* a run done (sent or failed): its packets, at least one, are done
ReapOk ==
    /\ Awake /\ cOk > 0
    /\ cOk' = cOk - 1
    /\ p' \in Left(Runs - 1, p - 1)
    /\ UNCHANGED <<quit, seen, st, handed, ready, held, u, k, cAgain, cRef, blocked, poll, recv, wakeRead, wakeCount, agains, againsBefore, refusals, recvEnds>>

\* -EAGAIN: the run back; before the stop sending is blocked until a poll
\* fires, after it the answer counts toward the bound
ReapAgain ==
    /\ Awake /\ cAgain > 0
    /\ cAgain' = cAgain - 1
    /\ u' = u + 1
    /\ blocked' = ~seen
    /\ agains' = IF seen /\ agains < MaxRetry THEN agains + 1 ELSE agains
    /\ UNCHANGED <<quit, seen, st, handed, ready, held, k, cOk, cRef, poll, recv, wakeRead, wakeCount, againsBefore, refusals, recvEnds, p>>

\* a refusal: the run back as one or more runs, at most one a packet, with
\* no block
ReapRefused ==
    /\ Awake /\ cRef > 0
    /\ cRef' = cRef - 1
    /\ u' \in (u + 1)..(u + 1 + p - Runs)
    /\ UNCHANGED <<quit, seen, st, handed, ready, held, k, cOk, cAgain, blocked, poll, recv, wakeRead, wakeCount, agains, againsBefore, refusals, recvEnds, p>>

ReapPoll ==
    /\ Awake /\ poll = "posted"
    /\ poll' = "none" /\ blocked' = FALSE
    /\ UNCHANGED <<quit, seen, st, handed, ready, held, u, k, cOk, cAgain, cRef, recv, wakeRead, wakeCount, agains, againsBefore, refusals, recvEnds, p>>

ReapRecvEnd ==
    /\ Awake /\ recv = "posted"
    /\ recv' = "ended"
    /\ UNCHANGED <<quit, seen, st, handed, ready, held, u, k, cOk, cAgain, cRef, blocked, poll, wakeRead, wakeCount, agains, againsBefore, refusals, recvEnds, p>>

ReapWake ==
    /\ Awake /\ wakeRead = "posted"
    /\ wakeRead' = "none"
    /\ UNCHANGED <<quit, seen, st, handed, ready, held, u, k, cOk, cAgain, cRef, blocked, poll, recv, wakeCount, agains, againsBefore, refusals, recvEnds, p>>

\* a bank all of whose runs are done goes back to the free FIFO
Retire ==
    /\ Awake /\ held /\ u = 0 /\ k = 0 /\ cOk = 0 /\ cAgain = 0 /\ cRef = 0 /\ p = 0
    /\ held' = FALSE
    /\ UNCHANGED <<quit, seen, st, handed, ready, u, k, cOk, cAgain, cRef, blocked, poll, recv, wakeRead, wakeCount, agains, againsBefore, refusals, recvEnds, p>>

\* the oldest ready bank of up to R packets, cut into runs: none when it
\* has none, else from one run to one a packet
Claim ==
    /\ Awake /\ ~held /\ ready > 0
    /\ ready' = ready - 1 /\ held' = TRUE
    /\ p' \in 0..R
    /\ u' \in IF p' = 0 THEN {0} ELSE 1..p'
    /\ UNCHANGED <<quit, seen, st, handed, k, cOk, cAgain, cRef, blocked, poll, recv, wakeRead, wakeCount, agains, againsBefore, refusals, recvEnds>>

\* the code abandons before it sends: nothing is sent once abandoning
CanSend == Awake /\ held /\ ~blocked /\ ~Abandoning /\ u > 0 /\ k < K

Submit ==
    /\ CanSend
    /\ u' = u - 1 /\ k' = k + 1
    /\ UNCHANGED <<quit, seen, st, handed, ready, held, cOk, cAgain, cRef, blocked, poll, recv, wakeRead, wakeCount, agains, againsBefore, refusals, recvEnds, p>>

\* a run to a destination known to refuse segmentation: one run a packet,
\* so a run of more than one packet (some run has one when Runs < p)
Split ==
    /\ CanSend /\ Runs < p
    /\ u' \in (u + 1)..(u + p - Runs)
    /\ UNCHANGED <<quit, seen, st, handed, ready, held, k, cOk, cAgain, cRef, blocked, poll, recv, wakeRead, wakeCount, agains, againsBefore, refusals, recvEnds, p>>

\* a run to a destination the socket cannot name: dropped, no send
Drop ==
    /\ CanSend
    /\ u' = u - 1
    /\ p' \in Left(Runs - 1, p - 1)
    /\ UNCHANGED <<quit, seen, st, handed, ready, held, k, cOk, cAgain, cRef, blocked, poll, recv, wakeRead, wakeCount, agains, againsBefore, refusals, recvEnds>>

\* past MaxRetry answers of -EAGAIN since the stop, what waits is abandoned
Abandon ==
    /\ Awake /\ Abandoning /\ held /\ u > 0
    /\ u' = 0
    /\ p' \in Left(Runs - u, p - u)
    /\ UNCHANGED <<quit, seen, st, handed, ready, held, k, cOk, cAgain, cRef, blocked, poll, recv, wakeRead, wakeCount, agains, againsBefore, refusals, recvEnds>>

\* the POLLOUT poll a -EAGAIN asked for before the stop
ArmPoll ==
    /\ Awake /\ blocked /\ poll = "none" /\ ~seen
    /\ poll' = "armed"
    /\ UNCHANGED <<quit, seen, st, handed, ready, held, u, k, cOk, cAgain, cRef, blocked, recv, wakeRead, wakeCount, agains, againsBefore, refusals, recvEnds, p>>

\* a receive that ended on its own is armed again until the stop is seen
ArmRecv ==
    /\ Awake /\ ~seen /\ recv = "ended"
    /\ recv' = "armed"
    /\ UNCHANGED <<quit, seen, st, handed, ready, held, u, k, cOk, cAgain, cRef, blocked, poll, wakeRead, wakeCount, agains, againsBefore, refusals, recvEnds, p>>

\* the wake read is armed again until the stop is seen
ArmWake ==
    /\ Awake /\ ~seen /\ wakeRead = "none"
    /\ wakeRead' = "armed"
    /\ UNCHANGED <<quit, seen, st, handed, ready, held, u, k, cOk, cAgain, cRef, blocked, poll, recv, wakeCount, agains, againsBefore, refusals, recvEnds, p>>

\* the stop seen: the receive, the wake read and the poll are cancelled
CancelRecv ==
    /\ Awake /\ seen /\ recv = "armed"
    /\ recv' = "cancelling"
    /\ UNCHANGED <<quit, seen, st, handed, ready, held, u, k, cOk, cAgain, cRef, blocked, poll, wakeRead, wakeCount, agains, againsBefore, refusals, recvEnds, p>>

CancelWake ==
    /\ Awake /\ seen /\ wakeRead = "armed"
    /\ wakeRead' = "cancelling"
    /\ UNCHANGED <<quit, seen, st, handed, ready, held, u, k, cOk, cAgain, cRef, blocked, poll, recv, wakeCount, agains, againsBefore, refusals, recvEnds, p>>

CancelPoll ==
    /\ Awake /\ seen /\ poll = "armed"
    /\ poll' = "cancelling"
    /\ UNCHANGED <<quit, seen, st, handed, ready, held, u, k, cOk, cAgain, cRef, blocked, recv, wakeRead, wakeCount, agains, againsBefore, refusals, recvEnds, p>>

Drained ==
    /\ ~held /\ ready = 0 /\ u = 0 /\ k = 0 /\ cOk = 0 /\ cAgain = 0 /\ cRef = 0
    /\ recv = "ended" /\ wakeRead = "none" /\ poll = "none"

Exit ==
    /\ Awake /\ seen
    /\ (ExitRule = "drained" => Drained)
    /\ st' = "exited"
    /\ UNCHANGED <<quit, seen, handed, ready, held, u, k, cOk, cAgain, cRef, blocked, poll, recv, wakeRead, wakeCount, agains, againsBefore, refusals, recvEnds, p>>

\* everything the worker does awake but sleep and exit
Busy ==
    \/ Observe \/ ReapOk \/ ReapAgain \/ ReapRefused \/ ReapPoll \/ ReapRecvEnd \/ ReapWake
    \/ Retire \/ Claim \/ Submit \/ Split \/ Drop \/ Abandon
    \/ ArmPoll \/ ArmRecv \/ CancelRecv \/ CancelWake \/ CancelPoll

\* nothing to do now: the wait. The code sleeps only with nothing it could
\* do, the wake read armed (WakeRule "armed") and not able to exit
Sleep ==
    /\ Awake
    /\ ~ENABLED Busy
    /\ ~ENABLED Exit
    /\ WakeRule = "armed" => ~ENABLED ArmWake
    /\ st' = "asleep"
    /\ UNCHANGED <<quit, seen, handed, ready, held, u, k, cOk, cAgain, cRef, blocked, poll, recv, wakeRead, wakeCount, agains, againsBefore, refusals, recvEnds, p>>

\* the worker has exited and main has joined: stutter
Done ==
    /\ st = "exited"
    /\ UNCHANGED vars

WorkerStep == Busy \/ ArmWake \/ Exit \/ Sleep

KernelStep ==
    \/ KernelOk \/ KernelAgain \/ KernelRefuse
    \/ KernelPoll \/ KernelRecvEnd \/ KernelRecvEndsItself \/ KernelWakeRead \/ KernelWakeEnd \/ KernelWakesWorker

Next == Publish \/ Stop \/ WorkerStep \/ KernelStep \/ Done

Spec == Init /\ [][Next]_vars

\* the worker and the kernel keep going; main may stop or not
Fairness ==
    /\ WF_vars(WorkerStep)
    /\ WF_vars(KernelOk \/ KernelAgain \/ KernelRefuse)
    /\ WF_vars(KernelPoll) /\ WF_vars(KernelRecvEnd) /\ WF_vars(KernelWakeRead) /\ WF_vars(KernelWakeEnd)
    /\ WF_vars(KernelWakesWorker)

LiveSpec == Spec /\ Fairness

---------------------------------------------------------------------------

TypeOK ==
    /\ quit \in BOOLEAN /\ seen \in BOOLEAN /\ st \in {"awake", "asleep", "exited"}
    /\ handed \in 0..U /\ ready \in 0..U /\ held \in BOOLEAN /\ u \in 0..R /\ k \in 0..K
    /\ cOk \in 0..K /\ cAgain \in 0..K /\ cRef \in 0..K
    /\ blocked \in BOOLEAN /\ poll \in {"none", "armed", "cancelling", "posted"}
    /\ recv \in {"armed", "cancelling", "posted", "ended"}
    /\ wakeRead \in {"none", "armed", "cancelling", "posted"} /\ wakeCount \in BOOLEAN
    /\ agains \in 0..MaxRetry /\ againsBefore \in 0..MaxAgain /\ refusals \in 0..MaxRefuse
    /\ recvEnds \in 0..MaxRecvEnd /\ p \in 0..R

\* every run holds at least one packet of the held bank, and a bank not held
\* has none left
PacketsCoverRuns == Runs <= p /\ (~held => p = 0)

\* nothing blocks once the stop is seen: no poll may hold the stop
BlockedOnlyBeforeStop == seen => ~blocked

\* a held bank's runs: waiting, in the kernel or unreaped, never past R
RunsBounded == Runs <= R

\* nothing of the worker's is in the kernel once it has exited
ExitDrained == st = "exited" => Drained

\* the worker reads quit only after main set it
SeenOnlyAfterQuit == seen => quit

StopFinishes == quit ~> (st = "exited")

\* before the stop, what main publishes is submitted
Drains == [](((ready > 0 \/ u > 0) /\ ~quit) => <>((ready = 0 /\ u = 0) \/ quit))

=============================================================================
