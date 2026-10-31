-------------------------- MODULE RNClientReceiveQuit --------------------------
(***************************************************************************)
(* The client receive thread's quit handshake with stop and destroy.  This *)
(* is the thread create starts when the granted transport has no wait and  *)
(* wake (the legacy path of sdk/rocketnet_client_transport.cpp); the I/O   *)
(* thread of the other branch is RNClientIo's.                             *)
(*                                                                         *)
(* The thread (rocketnet_client_thread_function) loads quit with acquire   *)
(* at the top of every pass, drains the socket                             *)
(* (rocketnet_client_receive_packets, which never blocks), then waits up   *)
(* to 1 ms in rocketnet_platform_thread_wait on its own record.  Stop      *)
(* (rocketnet_client_transport_stop) stores quit with release and makes no *)
(* wake; destroy (rocketnet_client_thread_release) joins the thread and    *)
(* frees its handle, then frees the transport the thread reads.  Nothing   *)
(* calls rocketnet_platform_thread_wake on this path, so the thread leaves *)
(* its wait only when the wait returns by itself.                          *)
(*                                                                         *)
(* The wait per platform (sdk/rocketnet_platform.cpp): linux waits on a    *)
(* condition variable until a CLOCK_MONOTONIC deadline, re-waiting after a *)
(* spurious wakeup; mac waits on a mach semaphore until the timeout and    *)
(* ignores the result, so an aborted wait returns early; windows sleeps,   *)
(* which always returns.                                                   *)
(*                                                                         *)
(* WaitTimeout says what the model may assume of the wait's timeout:       *)
(* "fair" (it always fires) or "unreliable" (it may never fire).  The code *)
(* relies on "fair": with no wake, a timeout that never fires leaves the   *)
(* join waiting forever, and MCRNClientReceiveQuitBrokenTimeoutNeverFires  *)
(* .cfg is that case, failing DestroyCompletes.                            *)
(***************************************************************************)
EXTENDS Naturals

CONSTANTS Platform,        \* "linux", "mac" or "windows"
          WaitTimeout      \* "fair" or "unreliable"

ASSUME Platform \in {"linux", "mac", "windows"}
ASSUME WaitTimeout \in {"fair", "unreliable"}

ThreadStates  == {"running", "receiving", "waiting", "exited"}
DestroyStates == {"live", "quitSet", "joining", "joined", "freed"}

VARIABLES quit,   \* client_transport->quit
          th,     \* the receive thread: running (at the quit check),
                  \* receiving (in the drain), waiting (in thread_wait), exited
          dst     \* stop and destroy's progress

vars == <<quit, th, dst>>

TypeOK ==
    /\ quit \in BOOLEAN
    /\ th \in ThreadStates
    /\ dst \in DestroyStates

\* create: quit false, the thread at the top of its loop
Init ==
    /\ quit = FALSE
    /\ th = "running"
    /\ dst = "live"

(* ---- the receive thread, rocketnet_client_thread_function ---- *)

\* quit.load(acquire): true ends the loop and the thread function returns,
\* false enters the body
Check ==
    /\ th = "running"
    /\ th' = IF quit THEN "exited" ELSE "receiving"
    /\ UNCHANGED <<quit, dst>>

\* the drain returns and rocketnet_platform_thread_wait(thread, 0.001) begins
WaitBegin ==
    /\ th = "receiving"
    /\ th' = "waiting"
    /\ UNCHANGED <<quit, dst>>

\* the wait's timeout fires: pthread_cond_timedwait returns ETIMEDOUT,
\* semaphore_timedwait times out, or the Windows sleep returns.  Back to
\* the quit check.
WaitTimesOut ==
    /\ th = "waiting"
    /\ th' = "running"
    /\ UNCHANGED <<quit, dst>>

\* a spurious return, the environment's.  linux: a spurious wakeup finds
\* nothing to do and waits again, a step that changes nothing.  mac:
\* semaphore_timedwait returns early and the thread goes back to its
\* check.  Windows' Sleep has no early return.
Spurious ==
    /\ th = "waiting"
    /\ Platform # "windows"
    /\ th' = IF Platform = "mac" THEN "running" ELSE th
    /\ UNCHANGED <<quit, dst>>

(* ---- stop and destroy ---- *)

\* rocketnet_client_transport_stop, the legacy branch: quit.store(true,
\* release), and no wake
StopQuit ==
    /\ dst = "live"
    /\ quit' = TRUE
    /\ dst' = "quitSet"
    /\ UNCHANGED th

\* rocketnet_client_thread_release: rocketnet_platform_thread_join(thread)
\* is called (pthread_join on linux and mac; WaitForSingleObject on windows)
JoinBegin ==
    /\ dst = "quitSet"
    /\ dst' = "joining"
    /\ UNCHANGED <<quit, th>>

\* the join returns once the thread function has returned
JoinReturns ==
    /\ dst = "joining"
    /\ th = "exited"
    /\ dst' = "joined"
    /\ UNCHANGED <<quit, th>>

\* rocketnet_platform_thread_destroy frees the handle; then destroy frees
\* the transport the thread read
DestroyFree ==
    /\ dst = "joined"
    /\ dst' = "freed"
    /\ UNCHANGED <<quit, th>>

Done ==
    /\ dst = "freed"
    /\ UNCHANGED vars

ThreadNext == Check \/ WaitBegin \/ WaitTimesOut \/ Spurious

DestroyNext == StopQuit \/ JoinBegin \/ JoinReturns \/ DestroyFree

Next == ThreadNext \/ DestroyNext \/ Done

Spec == Init /\ [][Next]_vars

\* Weak fairness on the thread's own steps and on destroy once stop has
\* run (stop itself is the game's call and need never come).  The timeout
\* is fair only under WaitTimeout = "fair"; spurious returns get no
\* fairness.
SpecFair ==
    /\ Spec
    /\ WF_vars(Check)
    /\ WF_vars(WaitBegin)
    /\ (WaitTimeout = "fair" => WF_vars(WaitTimesOut))
    /\ WF_vars(JoinBegin)
    /\ WF_vars(JoinReturns)
    /\ WF_vars(DestroyFree)

(* ---- what the handshake must keep ---- *)

\* once stop has run, destroy completes: the thread exits, the join
\* returns and the handle and transport are freed
DestroyCompletes ==
    dst # "live" ~> dst = "freed"

=============================================================================
