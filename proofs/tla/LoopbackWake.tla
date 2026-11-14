---------------------------- MODULE LoopbackWake ----------------------------
(***************************************************************************)
(* The loopback client's wait and wake in                                 *)
(* sdk/rocketnet_driver_loopback.cpp (loopback_queue_push,                *)
(* loopback_client_receive, loopback_client_wait, loopback_client_wake):  *)
(* one client endpoint's queue (count), its event-only platform wake      *)
(* (pending: level-triggered, consumed by the wait that returns on it)    *)
(* and the client I/O thread (io).  The thread drains the queue one       *)
(* datagram at a time (Pop) until a receive finds it empty (Drained) or   *)
(* its buffer is full (BufferFull, the core's readable = false wait on    *)
(* the wake alone).  Its idle wait checks the queue under the hub mutex   *)
(* (Check), releases it and blocks on the wake (Enter) until the wake is  *)
(* set (Wake).  A send that makes the queue non-empty sets the wake       *)
(* (Send); a hand-off or stop sets it too (Handoff).  The core's guard    *)
(* timeout is left out: it only bounds a lost wake, and the property is   *)
(* that no wake is lost.                                                   *)
(*                                                                         *)
(* Two switches are the fix; the reversed witnesses turn one off:        *)
(*   SendWakes   a send to an empty queue sets the wake.  Off, the thread *)
(*               sleeps over a queued datagram until its timeout: the     *)
(*               polling client this replaced (NoSleepOverQueued).        *)
(*   LevelWake   a set made while the thread is not blocked stays pending *)
(*               for its next wait.  Off, a send between Check and Enter  *)
(*               is lost (NoSleepOverQueued).                              *)
(***************************************************************************)
EXTENDS Naturals

CONSTANTS
    QueueMax,       \* the queue's capacity in the instance
    SendWakes,      \* the fix: a send that makes the queue non-empty sets the wake
    LevelWake       \* the platform wake is level-triggered

VARIABLES
    count,          \* datagrams queued for the client
    pending,        \* the wake is set and not yet consumed
    io              \* the I/O thread: drain, check, enter, block or full

vars == <<count, pending, io>>

Init ==
    /\ count = 0
    /\ pending = FALSE
    /\ io = "check"

\* a set: kept until a wait consumes it, or with an edge wake only seen by a blocked wait
Set ==
    IF LevelWake \/ io \in {"block", "full"} THEN TRUE ELSE pending

\* loopback_send and loopback_server_send_batch_impl through loopback_queue_push
Send ==
    /\ count < QueueMax
    /\ count' = count + 1
    /\ pending' = IF SendWakes /\ count = 0 THEN Set ELSE pending
    /\ UNCHANGED io

\* loopback_client_wake: the core's hand-off or stop
Handoff ==
    /\ pending' = Set
    /\ UNCHANGED <<count, io>>

\* loopback_client_receive dequeues one datagram
Pop ==
    /\ io = "drain"
    /\ count > 0
    /\ count' = count - 1
    /\ UNCHANGED <<pending, io>>

\* loopback_client_receive finds the queue empty: the step is idle
Drained ==
    /\ io = "drain"
    /\ count = 0
    /\ io' = "check"
    /\ UNCHANGED <<count, pending>>

\* the core's buffer is full: it waits on the wake alone (readable false)
BufferFull ==
    /\ io = "drain"
    /\ count > 0
    /\ io' = "full"
    /\ UNCHANGED <<count, pending>>

\* loopback_client_wait with readable: a queued datagram returns at once
Check ==
    /\ io = "check"
    /\ io' = IF count > 0 THEN "drain" ELSE "enter"
    /\ UNCHANGED <<count, pending>>

\* loopback_client_wait releases the hub mutex and blocks on the wake
Enter ==
    /\ io = "enter"
    /\ io' = "block"
    /\ UNCHANGED <<count, pending>>

\* loopback_client_wait returns on the wake and consumes it
Wake ==
    /\ io \in {"block", "full"}
    /\ pending
    /\ pending' = FALSE
    /\ io' = "drain"
    /\ UNCHANGED count

Next ==
    \/ Send
    \/ Handoff
    \/ Pop
    \/ Drained
    \/ BufferFull
    \/ Check
    \/ Enter
    \/ Wake

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ count \in 0..QueueMax
    /\ pending \in BOOLEAN
    /\ io \in {"drain", "check", "enter", "block", "full"}

\* a blocked thread with a datagram queued has a wake pending: it never
\* sleeps over a datagram (only the full buffer waits on the hand-off)
NoSleepOverQueued ==
    (io = "block" /\ count > 0) => pending

=============================================================================
