------------------------- MODULE RNUringQueueReceive -------------------------
(***************************************************************************)
(* The io_uring server's per-queue receive (sdk/rocketnet_uring_receive_  *)
(* queue.cpp): the handoff of one queue's receive bank between its worker  *)
(* thread and main, and the life of the queue's multishot RECVMSG.         *)
(*                                                                         *)
(* The kernel side: datagrams arrive at the socket, which holds SockCap    *)
(* and drops the rest (sk_drops, which each later datagram carries as its  *)
(* SO_RXQ_OVFL stamp).  While the multishot is armed the kernel takes the  *)
(* socket's oldest datagram into a free provided buffer and posts a        *)
(* completion that names it; with no free buffer the multishot ends        *)
(* (ENOBUFS), and a stop's cancel ends it too.                             *)
(*                                                                         *)
(* The worker handles the completions in order.  An ended multishot        *)
(* leaves the queue needing an arm, which the worker gives it unless the   *)
(* queue is stopping.  A datagram the transport or the prefilter refuses   *)
(* is counted by name; an accepted one is banked when the bank has room    *)
(* and counted BANK_FULL when it has none; either way its buffer goes back *)
(* to the ring.  Banking is two steps, as on the hardware: the worker     *)
(* writes the slot, then publishes it (tail + 1, a release store).         *)
(*                                                                         *)
(* The bank is a ring of S slots, position p in slot p % S.  tail is the   *)
(* worker's (every position below it is whole), head is main's (every     *)
(* position below it main has finished reading), exposed is the end of    *)
(* what main's last drain took.  A drain first releases the positions the  *)
(* drain before took (head := exposed), then takes up to DrainMax more;    *)
(* main reads them until its update is done.                               *)
(*                                                                         *)
(* The checked claims:                                                     *)
(*   NoOverwrite      the worker writes no slot whose datagram main has    *)
(*                    not finished reading.                                *)
(*   NoTornRead       a slot main took is not written before main is done  *)
(*                    with it.                                             *)
(*   InOrder          main reads exactly the banked datagrams, in the      *)
(*                    order they were banked, each once.                   *)
(*   Accounted        every datagram that reached the socket is at the     *)
(*                    socket, in a completion, with the worker, banked,    *)
(*                    refused by name, counted BANK_FULL or counted as a  *)
(*                    socket drop.                                         *)
(*   BuffersKept      every provided buffer is in the ring, in a           *)
(*                    completion or with the worker: none leaks.          *)
(*   SocketCounted    the socket drops the worker counted are exactly the *)
(*                    stamp of the last datagram it handled.               *)
(*   Rearms           (liveness) a multishot that ended while the queue    *)
(*                    runs is armed again.                                 *)
(*                                                                         *)
(* Rule switches, each changed in one reversed witness:                    *)
(*   FullRule     "lt": room while tail - head < S (the code); "le" lets   *)
(*                the worker write position head + S (NoOverwrite).        *)
(*   PublishRule  "after": the slot is written, then tail published (the   *)
(*                code); "before" publishes first, so a drain can take a   *)
(*                position whose slot still holds an older datagram        *)
(*                (InOrder).                                               *)
(*   ReleaseRule  "next": a drain releases what the drain before took (the *)
(*                code); "take" releases what it takes as it takes it, so  *)
(*                the worker writes slots main is reading (NoTornRead).    *)
(*   FullCount    "count": a full bank's drop is counted (the code);       *)
(*                "silent" drops it uncounted (Accounted).                 *)
(*   RearmRule    "clear": an ended multishot clears the worker's armed    *)
(*                (the code); "keep" leaves it set, so nothing arms again  *)
(*                (Rearms).                                                *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS S,            \* bank slots
          B,            \* provided buffers
          SockCap,      \* datagrams the socket holds
          MaxArrive,    \* datagrams the network sends
          DrainMax,     \* the most one drain takes
          FullRule,     \* "lt" (the code) or "le"
          PublishRule,  \* "after" (the code) or "before"
          ReleaseRule,  \* "next" (the code) or "take"
          FullCount,    \* "count" (the code) or "silent"
          RearmRule     \* "clear" (the code) or "keep"

ASSUME S >= 1 /\ B >= 1 /\ SockCap >= 1 /\ MaxArrive >= 1 /\ DrainMax >= 1
ASSUME FullRule \in {"lt", "le"} /\ PublishRule \in {"after", "before"}
ASSUME ReleaseRule \in {"next", "take"} /\ FullCount \in {"count", "silent"}
ASSUME RearmRule \in {"clear", "keep"}

Slots == 0..(S - 1)

VARIABLES arrived,      \* datagrams the network sent the socket (ids 1..arrived)
          sock,         \* the socket's queue: [d, ok, stamp], oldest first
          kdrops,       \* datagrams the socket dropped (sk_drops)
          free,         \* buffers in the provided ring
          karmed,       \* a multishot is live in the kernel
          cq,           \* completions posted and not handled, oldest first
          marmed,       \* the worker's armed (needs an arm when FALSE)
          stopping,     \* the transport stopped the queue
          wstate,       \* the worker: "idle", "write" or "publish"
          wd,           \* the datagram the worker is banking, 0 when none
          wpos,         \* the position it is banking at
          slot,         \* each slot's datagram, 0 for none
          tail, head, exposed,
          reading,      \* main is between a drain and the end of its update
          view,         \* the datagrams main's last drain took, by slot read at the drain
          banked,       \* the datagrams in the order they took their positions
          delivered,    \* the datagrams main read, in order
          named,        \* refusals counted by name
          full,         \* BANK_FULL drops counted
          lost,         \* BANK_FULL drops not counted
          sockCounted,  \* socket drops the worker counted (SO_RXQ_OVFL)
          lastStamp,    \* the stamp of the last datagram the worker handled
          overwrite, torn  \* flags the broken steps set

vars == <<arrived, sock, kdrops, free, karmed, cq, marmed, stopping, wstate, wd, wpos,
          slot, tail, head, exposed, reading, view, banked, delivered, named, full, lost,
          sockCounted, lastStamp, overwrite, torn>>

Init ==
    /\ arrived = 0 /\ sock = <<>> /\ kdrops = 0 /\ free = B
    /\ karmed = FALSE /\ cq = <<>> /\ marmed = FALSE /\ stopping = FALSE
    /\ wstate = "idle" /\ wd = 0 /\ wpos = 0
    /\ slot = [s \in Slots |-> 0]
    /\ tail = 0 /\ head = 0 /\ exposed = 0
    /\ reading = FALSE /\ view = <<>>
    /\ banked = <<>> /\ delivered = <<>>
    /\ named = 0 /\ full = 0 /\ lost = 0
    /\ sockCounted = 0 /\ lastStamp = 0
    /\ overwrite = FALSE /\ torn = FALSE

Room == IF FullRule = "lt" THEN tail - head < S ELSE tail - head <= S

WorkerVars == <<marmed, wstate, wd, wpos, slot, tail, banked, named, full, lost, sockCounted, lastStamp, overwrite>>
MainVars == <<head, exposed, reading, view, delivered, torn>>
KernelVars == <<arrived, sock, kdrops, karmed, stopping>>

-----------------------------------------------------------------------------
\* the network and the kernel

Arrive ==
    /\ arrived < MaxArrive
    /\ arrived' = arrived + 1
    /\ IF Len(sock) < SockCap
         THEN \E ok \in BOOLEAN :
                 /\ sock' = Append(sock, [d |-> arrived + 1, ok |-> ok, stamp |-> kdrops])
                 /\ UNCHANGED kdrops
         ELSE /\ kdrops' = kdrops + 1
              /\ UNCHANGED sock
    /\ UNCHANGED <<free, karmed, cq, stopping>> /\ UNCHANGED WorkerVars /\ UNCHANGED MainVars

KernelReceive ==
    /\ karmed /\ sock # <<>> /\ free > 0
    /\ free' = free - 1
    /\ cq' = Append(cq, [kind |-> "data"] @@ Head(sock))
    /\ sock' = Tail(sock)
    /\ UNCHANGED <<arrived, kdrops, karmed, stopping>> /\ UNCHANGED WorkerVars /\ UNCHANGED MainVars

\* ENOBUFS: the ring is empty, so the multishot ends
KernelNoBuffers ==
    /\ karmed /\ sock # <<>> /\ free = 0
    /\ karmed' = FALSE
    /\ cq' = Append(cq, [kind |-> "end"])
    /\ UNCHANGED <<arrived, sock, kdrops, free, stopping>> /\ UNCHANGED WorkerVars /\ UNCHANGED MainVars

\* the stop's cancel ends the multishot (ECANCELED)
KernelCancel ==
    /\ karmed /\ stopping
    /\ karmed' = FALSE
    /\ cq' = Append(cq, [kind |-> "end"])
    /\ UNCHANGED <<arrived, sock, kdrops, free, stopping>> /\ UNCHANGED WorkerVars /\ UNCHANGED MainVars

Stop ==
    /\ ~stopping
    /\ stopping' = TRUE
    /\ UNCHANGED <<arrived, sock, kdrops, free, karmed, cq>> /\ UNCHANGED WorkerVars /\ UNCHANGED MainVars

-----------------------------------------------------------------------------
\* the worker

\* rocketnet_uring_receive_queue_needs_arm and _arm
Arm ==
    /\ ~marmed /\ ~stopping /\ wstate = "idle"
    /\ marmed' = TRUE
    /\ karmed' = TRUE
    /\ UNCHANGED <<arrived, sock, kdrops, free, cq, stopping, wstate, wd, wpos, slot, tail, banked,
                   named, full, lost, sockCounted, lastStamp, overwrite>>
    /\ UNCHANGED MainVars

\* rocketnet_uring_receive_queue_cqe: one completion, oldest first
Complete ==
    /\ wstate = "idle" /\ cq # <<>>
    /\ LET c == Head(cq) IN
       /\ cq' = Tail(cq)
       /\ IF c.kind = "end"
            THEN /\ marmed' = IF RearmRule = "clear" THEN FALSE ELSE marmed
                 /\ UNCHANGED <<free, wstate, wd, wpos, slot, tail, banked, named, full, lost, sockCounted, lastStamp>>
            ELSE /\ sockCounted' = sockCounted + (c.stamp - lastStamp)
                 /\ lastStamp' = c.stamp
                 /\ UNCHANGED marmed
                 /\ IF ~c.ok
                      THEN /\ named' = named + 1
                           /\ free' = free + 1
                           /\ UNCHANGED <<wstate, wd, wpos, slot, tail, banked, full, lost>>
                      ELSE IF Room
                             THEN /\ wd' = c.d
                                  /\ wpos' = tail
                                  /\ banked' = Append(banked, c.d)
                                  /\ wstate' = IF PublishRule = "after" THEN "write" ELSE "publish"
                                  /\ UNCHANGED <<free, slot, tail, named, full, lost>>
                             ELSE /\ full' = full + (IF FullCount = "count" THEN 1 ELSE 0)
                                  /\ lost' = lost + (IF FullCount = "count" THEN 0 ELSE 1)
                                  /\ free' = free + 1
                                  /\ UNCHANGED <<wstate, wd, wpos, slot, tail, banked, named>>
    /\ UNCHANGED overwrite
    /\ UNCHANGED KernelVars /\ UNCHANGED MainVars

\* the slot write; under "after" the publish follows, under "before" it
\* preceded, and then the buffer goes back
Write ==
    /\ wstate = "write"
    /\ slot' = [slot EXCEPT ![wpos % S] = wd]
    /\ overwrite' = (overwrite \/ wpos - head >= S)
    /\ IF PublishRule = "after"
         THEN /\ wstate' = "publish"
              /\ UNCHANGED <<free, wd>>
         ELSE /\ wstate' = "idle"
              /\ wd' = 0
              /\ free' = free + 1
    /\ UNCHANGED <<marmed, wpos, tail, banked, named, full, lost, sockCounted, lastStamp>>
    /\ UNCHANGED <<cq>> /\ UNCHANGED KernelVars /\ UNCHANGED MainVars

Publish ==
    /\ wstate = "publish"
    /\ tail' = tail + 1
    /\ IF PublishRule = "after"
         THEN /\ wstate' = "idle"
              /\ wd' = 0
              /\ free' = free + 1
         ELSE /\ wstate' = "write"
              /\ UNCHANGED <<free, wd>>
    /\ UNCHANGED <<marmed, wpos, slot, banked, named, full, lost, sockCounted, lastStamp, overwrite>>
    /\ UNCHANGED <<cq>> /\ UNCHANGED KernelVars /\ UNCHANGED MainVars

-----------------------------------------------------------------------------
\* main

Min(a, b) == IF a < b THEN a ELSE b

\* rocketnet_uring_receive_queue_drain: release, then take up to DrainMax
Drain ==
    /\ ~reading
    /\ LET n == Min(tail - exposed, DrainMax) IN
       /\ exposed' = exposed + n
       /\ head' = IF ReleaseRule = "next" THEN exposed ELSE exposed + n
       /\ view' = [i \in 1..n |-> slot[(exposed + i - 1) % S]]
    /\ reading' = TRUE
    /\ UNCHANGED <<delivered, torn>>
    /\ UNCHANGED <<free, cq>> /\ UNCHANGED KernelVars /\ UNCHANGED WorkerVars

\* the update is done with what the drain took: main read each slot
Done ==
    /\ reading
    /\ LET n == Len(view)
           now == [i \in 1..n |-> slot[(exposed - n + i - 1) % S]]
       IN /\ torn' = (torn \/ now # view)
          /\ delivered' = delivered \o now
    /\ reading' = FALSE
    /\ UNCHANGED <<head, exposed, view>>
    /\ UNCHANGED <<free, cq>> /\ UNCHANGED KernelVars /\ UNCHANGED WorkerVars

-----------------------------------------------------------------------------

Next ==
    \/ Arrive \/ KernelReceive \/ KernelNoBuffers \/ KernelCancel \/ Stop
    \/ Arm \/ Complete \/ Write \/ Publish
    \/ Drain \/ Done

Spec == Init /\ [][Next]_vars

\* the worker, main and the kernel are fair; the network and the stop are not
Fairness ==
    /\ WF_vars(KernelReceive) /\ WF_vars(KernelNoBuffers) /\ WF_vars(KernelCancel)
    /\ WF_vars(Arm) /\ WF_vars(Complete) /\ WF_vars(Write) /\ WF_vars(Publish)
    /\ WF_vars(Drain) /\ WF_vars(Done)

LiveSpec == Spec /\ Fairness

-----------------------------------------------------------------------------

DataIn(q) == Len(SelectSeq(q, LAMBDA c : c.kind = "data"))

TypeOK ==
    /\ arrived \in 0..MaxArrive /\ kdrops \in Nat /\ free \in 0..B
    /\ karmed \in BOOLEAN /\ marmed \in BOOLEAN /\ stopping \in BOOLEAN
    /\ wstate \in {"idle", "write", "publish"}
    /\ slot \in [Slots -> 0..MaxArrive]
    /\ head <= exposed /\ exposed <= tail
    /\ reading \in BOOLEAN

NoOverwrite == ~overwrite
NoTornRead == ~torn

\* main read exactly the first datagrams banked, in that order
InOrder == delivered = SubSeq(banked, 1, Len(delivered))

\* a datagram the worker is banking is already in banked
Accounted ==
    /\ arrived = Len(sock) + kdrops + DataIn(cq) + Len(banked) + named + full + lost
    /\ lost = 0

\* the worker needs an arm only when the kernel has no multishot
ArmedAgrees == ~marmed => ~karmed

BuffersKept == free + DataIn(cq) + (IF wstate = "idle" THEN 0 ELSE 1) = B

SocketCounted == sockCounted = lastStamp /\ sockCounted <= kdrops

\* an ended multishot is armed again while the queue runs
Rearms == [](~karmed /\ ~stopping => <>(karmed \/ stopping))

=============================================================================
