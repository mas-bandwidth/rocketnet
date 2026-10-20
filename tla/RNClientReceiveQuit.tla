-------------------------- MODULE RNClientReceiveQuit --------------------------
(***************************************************************************)
(* The client receive thread's quit-and-wake handshake with destroy, as   *)
(* rocketnet-client-destroy-joins-receive-thread built it.  This is the   *)
(* thread that create starts when the granted driver has no wait and wake *)
(* (sdk/rocketnet_client_transport.cpp:973); the I/O thread of the other  *)
(* branch is RNClientIo's.                                                *)
(*                                                                         *)
(* The thread (sdk/rocketnet_client_transport.cpp:2088-2100) loads quit   *)
(* with acquire at the top of every pass (2094), drains the socket        *)
(* (2096, rocketnet_client_receive_packets, which never blocks: it breaks *)
(* when the buffer is full, 2047, or the socket is empty, 2052), then     *)
(* waits up to 1 ms in rocketnet_platform_thread_wait (2098).  destroy    *)
(* (1041-1044) stores quit with release, wakes the thread, joins it and   *)
(* frees its handle, then frees the transport the thread reads (1047 on). *)
(*                                                                         *)
(* The wait and the wake are per platform (sdk/rocketnet_platform.cpp):   *)
(*   linux    wait locks wake_mutex and, while woken is false, waits on   *)
(*            wake_cond until the deadline (1124-1130); a spurious return *)
(*            re-checks woken and waits again (the while at 1125).  wake  *)
(*            sets woken under the same mutex and broadcasts (1136-1139). *)
(*   mac      wait returns at once when woken is set (acquire, 2785-2786),*)
(*            else waits on a mach semaphore until the timeout (2796),    *)
(*            whose result is ignored, so an aborted wait returns early.  *)
(*            wake stores woken with release, then signals the semaphore  *)
(*            (2802-2803); a signal between the check and the wait is     *)
(*            kept by the semaphore's count.                              *)
(*   windows  wait is rocketnet_platform_sleep (3284-3287), which always  *)
(*            returns; wake is a no-op (3140-3144).                       *)
(* On linux and mac woken is set once and never cleared (1064, 1137;      *)
(* 2742, 2802), so every wait after the wake returns at once.             *)
(*                                                                         *)
(* WaitTimeout says what the model may assume of the wait's timeout:      *)
(* "fair" (it always fires: Windows' Sleep), "unreliable" (it may never   *)
(* fire: the macOS hang the fix was for, where the 1 ms wait sometimes    *)
(* did not time out, so linux and mac must complete destroy on the wake   *)
(* alone), or "none" (the wait has no timeout at all).                    *)
(*                                                                         *)
(* Two switches restore the defects the reversed witnesses must catch:    *)
(* WakeBeforeQuit makes destroy wake before it stores quit (the lost-wake *)
(* ordering: the wake is spent on a pass that still reads quit false);    *)
(* NoWake makes the wake a no-op on a POSIX platform, which with          *)
(* WaitTimeout = "none" is a wait with neither a timeout nor a wake.       *)
(***************************************************************************)
EXTENDS Naturals

CONSTANTS Platform,        \* "linux", "mac" or "windows"
          WaitTimeout,     \* "fair", "unreliable" or "none"
          WakeBeforeQuit,  \* defect: destroy wakes before it stores quit
          NoWake           \* defect: thread_wake does nothing on POSIX

ASSUME Platform \in {"linux", "mac", "windows"}
ASSUME WaitTimeout \in {"fair", "unreliable", "none"}
ASSUME WakeBeforeQuit \in BOOLEAN /\ NoWake \in BOOLEAN

\* thread_wake reaches the thread: linux and mac, unless the defect drops it
HasWake == Platform # "windows" /\ ~NoWake

ThreadStates  == {"running", "receiving", "waiting", "woken", "exited"}
DestroyStates == {"live", "quitSet", "wokeFirst", "woke", "joining",
                  "joined", "freed"}

VARIABLES quit,   \* client_transport->quit (transport.cpp:136)
          woken,  \* the thread handle's woken flag (platform.h:761, 797)
          sem,    \* mac: the wake semaphore's count (platform.h:796)
          th,     \* the receive thread: running (at the quit check),
                  \* receiving (in the drain), waiting (in thread_wait),
                  \* woken (thread_wait returned on the wake), exited
          dst     \* destroy's progress through 1041-1044 and the free

vars == <<quit, woken, sem, th, dst>>

TypeOK ==
    /\ quit \in BOOLEAN
    /\ woken \in BOOLEAN
    /\ sem \in 0..1
    /\ th \in ThreadStates
    /\ dst \in DestroyStates

\* create: quit{0} (transport.cpp:136), woken false at thread_create
\* (platform.cpp:1064 linux, 2742 mac), the semaphore created at count 0
\* (2716), and the thread at the top of its loop (shim 1014-1016, 2707-2709)
Init ==
    /\ quit = FALSE
    /\ woken = FALSE
    /\ sem = 0
    /\ th = "running"
    /\ dst = "live"

(* ---- the receive thread, transport.cpp:2088-2100 ---- *)

\* transport.cpp:2094: quit.load(acquire); true ends the loop and the
\* thread function returns (2100), false enters the body
Check ==
    /\ th \in {"running", "woken"}
    /\ th' = IF quit THEN "exited" ELSE "receiving"
    /\ UNCHANGED <<quit, woken, sem, dst>>

\* transport.cpp:2096 then 2098: the drain returns (2047 or 2052) and
\* thread_wait(0.001) begins.  linux: under wake_mutex (1124), woken
\* already set skips the cond wait (1125) and returns.  mac: woken set
\* returns at once (2785-2786).  windows: the sleep starts (3286).
WaitBegin ==
    /\ th = "receiving"
    /\ th' = IF HasWake /\ woken THEN "woken" ELSE "waiting"
    /\ UNCHANGED <<quit, woken, sem, dst>>

\* the wait ends on the wake.  linux: wake_cond broadcast by thread_wake
\* (1138) and woken re-read true by the while (1125), unlock (1130).  mac:
\* semaphore_timedwait returns on the signal (2796, signalled at 2803),
\* consuming the count, which also covers a signal that landed between
\* the check at 2785 and the wait.
WaitWoken ==
    /\ th = "waiting"
    /\ HasWake
    /\ IF Platform = "mac" THEN sem > 0 ELSE woken
    /\ sem' = IF Platform = "mac" THEN sem - 1 ELSE sem
    /\ th' = "woken"
    /\ UNCHANGED <<quit, woken, dst>>

\* the wait's timeout fires: pthread_cond_timedwait returns ETIMEDOUT and
\* the loop breaks (1127-1128); semaphore_timedwait times out (2796); the
\* Windows sleep returns (3286).  Back to the quit check at 2094.
WaitTimesOut ==
    /\ th = "waiting"
    /\ WaitTimeout # "none"
    /\ th' = "running"
    /\ UNCHANGED <<quit, woken, sem, dst>>

\* a spurious or lost wake, the environment's.  linux: a spurious wakeup
\* of pthread_cond_timedwait finds woken false and waits again (the while
\* at 1125), a step that changes nothing.  mac: semaphore_timedwait
\* returns early (KERN_ABORTED; its result is ignored at 2796), so the
\* thread goes back to its check.  A lost wake is a timeout that never
\* fires: WaitTimesOut has no fairness unless WaitTimeout = "fair".
\* Windows' Sleep has no early return.
Spurious ==
    /\ th = "waiting"
    /\ Platform # "windows"
    /\ ~(woken /\ Platform = "linux")
    /\ th' = IF Platform = "mac" THEN "running" ELSE th
    /\ UNCHANGED <<quit, woken, sem, dst>>

(* ---- destroy, transport.cpp:1035-1068 ---- *)

\* thread_wake's effect: linux sets woken under wake_mutex and broadcasts
\* (1136-1139); mac stores woken with release then signals (2802-2803);
\* windows does nothing (3143)
WakeEffect ==
    IF HasWake
    THEN /\ woken' = TRUE
         /\ sem' = IF Platform = "mac" THEN 1 ELSE sem
    ELSE UNCHANGED <<woken, sem>>

\* transport.cpp:1041: quit.store(true, release)
DestroyQuit ==
    /\ dst = "live"
    /\ ~WakeBeforeQuit
    /\ quit' = TRUE
    /\ dst' = "quitSet"
    /\ UNCHANGED <<woken, sem, th>>

\* transport.cpp:1042: rocketnet_platform_thread_wake(thread), after the
\* quit store.  Under WakeBeforeQuit it comes first.
DestroyWake ==
    /\ \/ dst = "quitSet" /\ ~WakeBeforeQuit
       \/ dst = "live" /\ WakeBeforeQuit
    /\ WakeEffect
    /\ dst' = IF WakeBeforeQuit THEN "wokeFirst" ELSE "woke"
    /\ UNCHANGED <<quit, th>>

\* the defect's quit store, after the wake
DestroyQuitLate ==
    /\ dst = "wokeFirst"
    /\ quit' = TRUE
    /\ dst' = "woke"
    /\ UNCHANGED <<woken, sem, th>>

\* transport.cpp:1043: rocketnet_platform_thread_join(thread) is called
\* (pthread_join, platform.cpp:1087 and 2764; WaitForSingleObject, 3128)
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

\* transport.cpp:1044: rocketnet_platform_thread_destroy frees the handle,
\* its mutex and cond (platform.cpp:1093-1095), its semaphore (2770-2771)
\* or its handle (3134-3135); then destroy frees the transport the thread
\* read (1047-1068)
DestroyFree ==
    /\ dst = "joined"
    /\ dst' = "freed"
    /\ UNCHANGED <<quit, woken, sem, th>>

Done ==
    /\ dst = "freed"
    /\ UNCHANGED vars

ThreadNext == Check \/ WaitBegin \/ WaitWoken \/ WaitTimesOut \/ Spurious

DestroyNext == DestroyQuit \/ DestroyWake \/ DestroyQuitLate \/ JoinBegin
               \/ JoinReturns \/ DestroyFree

Next == ThreadNext \/ DestroyNext \/ Done

Spec == Init /\ [][Next]_vars

\* Weak fairness on the thread's own steps and on destroy once it has
\* started (destroy itself is the game's call and need never come).  The
\* timeout is fair only where the platform guarantees it (Windows' Sleep);
\* on linux and mac the model does not assume it fires, so destroy must
\* complete on the wake alone.  Spurious returns get no fairness.
SpecFair ==
    /\ Spec
    /\ WF_vars(Check)
    /\ WF_vars(WaitBegin)
    /\ WF_vars(WaitWoken)
    /\ (WaitTimeout = "fair" => WF_vars(WaitTimesOut))
    /\ WF_vars(dst # "live" /\ DestroyWake)
    /\ WF_vars(DestroyQuitLate)
    /\ WF_vars(JoinBegin)
    /\ WF_vars(JoinReturns)
    /\ WF_vars(DestroyFree)

(* ---- what the handshake must keep ---- *)

\* the join returns, and the handle and the transport are freed, only
\* after the thread function has returned
NoJoinBeforeExit ==
    dst \in {"joined", "freed"} => th = "exited"

\* nothing touches the thread or its handle once the join has returned:
\* the thread takes no step, and no wake writes woken or the semaphore of
\* a handle thread_destroy frees
Untouched == th' = th /\ woken' = woken /\ sem' = sem

NoUseAfterJoin ==
    [][(dst \in {"joined", "freed"}) => Untouched]_vars

\* a wait the wake ended is followed by a quit check that ends the loop:
\* the wake is never spent on a pass that still reads quit false
WokenSeesQuit ==
    th = "woken" => quit

\* once destroy has begun, it completes: the thread exits, the join
\* returns and the handle and transport are freed
DestroyCompletes ==
    dst # "live" ~> dst = "freed"

=============================================================================
