-------------------------- MODULE RNClientReceiveQuit --------------------------
(***************************************************************************)
(* The legacy client receive thread's quit-and-wake handshake with stop    *)
(* and destroy.  This is the thread create starts when the granted         *)
(* transport has no wait and wake (sdk/rocketnet_client_transport.cpp);    *)
(* the I/O thread of the other branch is RNClientIo's.                     *)
(*                                                                         *)
(* The thread (rocketnet_client_thread_function) loads quit with acquire   *)
(* at the top of every pass, drains the socket                             *)
(* (rocketnet_client_receive_packets, which never blocks), then waits up   *)
(* to 1 ms in rocketnet_platform_thread_wait on its own record.  Stop      *)
(* (rocketnet_client_transport_stop, the legacy branch) stores quit with   *)
(* release and then wakes the thread (rocketnet_client_thread_wake);       *)
(* destroy (rocketnet_client_thread_release) joins it and frees its        *)
(* handle, then frees the transport the thread reads.                      *)
(*                                                                         *)
(* The wait and the wake are per platform (sdk/rocketnet_platform.cpp):    *)
(*   linux    wait locks wake_mutex and, while woken is false, waits on    *)
(*            wake_cond until the deadline; a spurious return re-checks    *)
(*            woken and waits again.  wake sets woken under the same mutex *)
(*            and broadcasts.                                              *)
(*   mac      wait returns at once when woken is set (acquire), else waits *)
(*            on a mach semaphore until the timeout, whose result is       *)
(*            ignored, so an aborted wait returns early.  wake stores      *)
(*            woken with release, then signals the semaphore; a signal     *)
(*            between the check and the wait is kept by the count.         *)
(*   windows  wait is rocketnet_platform_sleep, which always returns; wake *)
(*            is a no-op.                                                  *)
(* On linux and mac woken is set once and never cleared, so every wait     *)
(* after the wake returns at once.                                         *)
(*                                                                         *)
(* WaitTimeout says what the model may assume of the wait's timeout:       *)
(* "fair" (it always fires: Windows' Sleep), "unreliable" (it may never    *)
(* fire, so linux and mac must complete destroy on the wake alone), or     *)
(* "none" (the wait has no timeout at all).                                *)
(*                                                                         *)
(* Two switches restore the defects the reversed witnesses must catch:     *)
(* WakeBeforeQuit makes stop wake before it stores quit (the lost-wake     *)
(* ordering: the wake is spent on a pass that still reads quit false);     *)
(* NoWake makes stop make no wake on a POSIX platform, which with          *)
(* WaitTimeout = "none" is a wait with neither a timeout nor a wake.       *)
(***************************************************************************)
EXTENDS Naturals

CONSTANTS Platform,        \* "linux", "mac" or "windows"
          WaitTimeout,     \* "fair", "unreliable" or "none"
          WakeBeforeQuit,  \* defect: stop wakes before it stores quit
          NoWake           \* defect: stop makes no wake on POSIX

ASSUME Platform \in {"linux", "mac", "windows"}
ASSUME WaitTimeout \in {"fair", "unreliable", "none"}
ASSUME WakeBeforeQuit \in BOOLEAN /\ NoWake \in BOOLEAN

\* stop's wake reaches the thread: linux and mac, unless the defect drops it
HasWake == Platform # "windows" /\ ~NoWake

ThreadStates  == {"running", "receiving", "waiting", "woken", "exited"}
DestroyStates == {"live", "quitSet", "wokeFirst", "woke", "joining",
                  "joined", "freed"}

VARIABLES quit,   \* client_transport->quit (rocketnet_client_transport_t in sdk/rocketnet_client_transport.cpp)
          woken,  \* the thread handle's woken flag (rocketnet_platform_thread_t in sdk/rocketnet_platform.h)
          sem,    \* mac: the wake semaphore's count (rocketnet_platform_thread_t in sdk/rocketnet_platform.h)
          th,     \* the receive thread: running (at the quit check),
                  \* receiving (in the drain), waiting (in thread_wait),
                  \* woken (thread_wait returned on the wake), exited
          dst     \* stop and destroy's progress

vars == <<quit, woken, sem, th, dst>>

TypeOK ==
    /\ quit \in BOOLEAN
    /\ woken \in BOOLEAN
    /\ sem \in 0..1
    /\ th \in ThreadStates
    /\ dst \in DestroyStates

\* create: quit{0} (rocketnet_client_transport_create), woken false at rocketnet_platform_thread_create
\* (sdk/rocketnet_platform.cpp), the semaphore created at count 0,
\* and the thread at the top of its loop in rocketnet_client_thread_function
Init ==
    /\ quit = FALSE
    /\ woken = FALSE
    /\ sem = 0
    /\ th = "running"
    /\ dst = "live"

(* ---- the receive thread, rocketnet_client_thread_function ---- *)

\* rocketnet_client_thread_function: quit.load(acquire); true ends the loop and the
\* thread function returns, false enters the body
Check ==
    /\ th \in {"running", "woken"}
    /\ th' = IF quit THEN "exited" ELSE "receiving"
    /\ UNCHANGED <<quit, woken, sem, dst>>

\* rocketnet_client_thread_function: the drain returns and
\* rocketnet_platform_thread_wait(thread, 0.001) begins.  linux: under wake_mutex, woken
\* already set skips the cond wait and returns.  mac: woken set
\* returns at once.  windows: the sleep starts.
WaitBegin ==
    /\ th = "receiving"
    /\ th' = IF HasWake /\ woken THEN "woken" ELSE "waiting"
    /\ UNCHANGED <<quit, woken, sem, dst>>

\* the wait ends on the wake.  linux: wake_cond broadcast by thread_wake
\* and woken re-read true by the while, unlock.  mac:
\* semaphore_timedwait returns on the signal ,
\* consuming the count, which also covers a signal that landed between
\* the check and the wait.
WaitWoken ==
    /\ th = "waiting"
    /\ HasWake
    /\ IF Platform = "mac" THEN sem > 0 ELSE woken
    /\ sem' = IF Platform = "mac" THEN sem - 1 ELSE sem
    /\ th' = "woken"
    /\ UNCHANGED <<quit, woken, dst>>

\* the wait's timeout fires: pthread_cond_timedwait returns ETIMEDOUT and
\* the loop breaks; semaphore_timedwait times out; the
\* Windows sleep returns.  Back to the quit check.
WaitTimesOut ==
    /\ th = "waiting"
    /\ WaitTimeout # "none"
    /\ th' = "running"
    /\ UNCHANGED <<quit, woken, sem, dst>>

\* a spurious or lost wake, the environment's.  linux: a spurious wakeup
\* of pthread_cond_timedwait finds woken false and waits again (the while), a step that changes nothing.  mac: semaphore_timedwait
\* returns early (KERN_ABORTED; its result is ignored), so the
\* thread goes back to its check.  A lost wake is a timeout that never
\* fires: WaitTimesOut has no fairness unless WaitTimeout = "fair".
\* Windows' Sleep has no early return.
Spurious ==
    /\ th = "waiting"
    /\ Platform # "windows"
    /\ ~(woken /\ Platform = "linux")
    /\ th' = IF Platform = "mac" THEN "running" ELSE th
    /\ UNCHANGED <<quit, woken, sem, dst>>

(* ---- stop, rocketnet_client_transport_stop, and destroy ---- *)

\* thread_wake's effect: linux sets woken under wake_mutex and broadcasts;
\* mac stores woken with release then signals; windows does nothing
WakeEffect ==
    IF HasWake
    THEN /\ woken' = TRUE
         /\ sem' = IF Platform = "mac" THEN 1 ELSE sem
    ELSE UNCHANGED <<woken, sem>>

\* rocketnet_client_transport_stop, the legacy branch: quit.store(true, release)
StopQuit ==
    /\ dst = "live"
    /\ ~WakeBeforeQuit
    /\ quit' = TRUE
    /\ dst' = "quitSet"
    /\ UNCHANGED <<woken, sem, th>>

\* rocketnet_client_transport_stop: rocketnet_client_thread_wake(thread), after the
\* quit store.  Under WakeBeforeQuit it comes first.
StopWake ==
    /\ \/ dst = "quitSet" /\ ~WakeBeforeQuit
       \/ dst = "live" /\ WakeBeforeQuit
    /\ WakeEffect
    /\ dst' = IF WakeBeforeQuit THEN "wokeFirst" ELSE "woke"
    /\ UNCHANGED <<quit, th>>

\* the defect's quit store, after the wake
StopQuitLate ==
    /\ WakeBeforeQuit
    /\ dst = "wokeFirst"
    /\ quit' = TRUE
    /\ dst' = "woke"
    /\ UNCHANGED <<woken, sem, th>>

\* rocketnet_client_thread_release: rocketnet_platform_thread_join(thread) is called
\* (pthread_join on linux and mac; WaitForSingleObject on windows, in sdk/rocketnet_platform.cpp)
JoinBegin ==
    /\ dst = "woke"
    /\ dst' = "joining"
    /\ UNCHANGED <<quit, woken, sem, th>>

\* the join returns once the thread function has returned
JoinReturns ==
    /\ dst = "joining"
    /\ th = "exited"
    /\ dst' = "joined"
    /\ UNCHANGED <<quit, woken, sem, th>>

\* rocketnet_client_thread_release: rocketnet_platform_thread_destroy frees the handle,
\* its mutex and cond, its semaphore
\* or its handle (in sdk/rocketnet_platform.cpp); then destroy frees the transport the
\* thread read
DestroyFree ==
    /\ dst = "joined"
    /\ dst' = "freed"
    /\ UNCHANGED <<quit, woken, sem, th>>

Done ==
    /\ dst = "freed"
    /\ UNCHANGED vars

ThreadNext == Check \/ WaitBegin \/ WaitWoken \/ WaitTimesOut \/ Spurious

DestroyNext == StopQuit \/ StopWake \/ StopQuitLate \/ JoinBegin
               \/ JoinReturns \/ DestroyFree

Next == ThreadNext \/ DestroyNext \/ Done

Spec == Init /\ [][Next]_vars

\* Weak fairness on the thread's own steps and on stop and destroy once stop
\* has started (stop itself is the game's call and need never come).  The
\* timeout is fair only where the platform guarantees it (Windows' Sleep);
\* on linux and mac the model does not assume it fires, so destroy must
\* complete on the wake alone.  Spurious returns get no fairness.
SpecFair ==
    /\ Spec
    /\ WF_vars(Check)
    /\ WF_vars(WaitBegin)
    /\ WF_vars(WaitWoken)
    /\ (WaitTimeout = "fair" => WF_vars(WaitTimesOut))
    /\ WF_vars(dst # "live" /\ StopWake)
    /\ WF_vars(StopQuitLate)
    /\ WF_vars(JoinBegin)
    /\ WF_vars(JoinReturns)
    /\ WF_vars(DestroyFree)

(* ---- what the handshake must keep ---- *)

\* a wait the wake ended is followed by a quit check that ends the loop:
\* the wake is never spent on a pass that still reads quit false
WokenSeesQuit ==
    th = "woken" => quit

\* once stop has begun, destroy completes: the thread exits, the join
\* returns and the handle and transport are freed
DestroyCompletes ==
    dst # "live" ~> dst = "freed"

=============================================================================
