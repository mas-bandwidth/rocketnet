---------------------------- MODULE RNPortableTx ----------------------------
(***************************************************************************)
(* The portable (and macOS msgx) server transmit handoff of               *)
(* sdk/rocketnet_driver_builtin.cpp: portable_send, portable_tx_publish,  *)
(* portable_tx_worker, the publish retry in portable_receive, and the TX  *)
(* half of portable_stop and portable_stopped.  RNPortableRx covers the   *)
(* receive half; this is the layout model of the transmit half.           *)
(*                                                                         *)
(* Main owns one producer bank and appends packets to it, outside any     *)
(* lock.  It publishes the producer by try-locking the mutex, taking the   *)
(* oldest free bank in exchange and queueing the producer on the ready     *)
(* FIFO.  A failed try, or an empty free FIFO, leaves the producer with    *)
(* main: a full producer drops the rest of that call's batch and a        *)
(* partly filled one is retried by the next portable_send or, on an idle   *)
(* update, by portable_receive.  The worker takes the oldest ready bank   *)
(* under the mutex, sends its packets outside it, zeroes the count and     *)
(* returns the bank to the free FIFO under the mutex.  Main never waits.   *)
(*                                                                         *)
(* This model carries the code's own fields.  The free and ready queues   *)
(* are arrays of NumBanks entries with a head and a count, indexed by      *)
(* (head + count) mod the ring size, the code's free_index/free_head/      *)
(* free_count and ready_index/ready_head/ready_count.  The packet cells    *)
(* are one array: the code lays the banks' arrays end to end in one        *)
(* allocation (portable_tx_storage_create), so bank b's slot s is cell     *)
(* b * Cap + s and a write one past a bank's capacity lands in the next    *)
(* bank's first slot.  A bank's count is its own field.                    *)
(*                                                                         *)
(* The checked claims:                                                     *)
(*   ExactSend       the packets the worker sent, the unsent rest of its   *)
(*                   bank, the ready banks in FIFO order, the producer     *)
(*                   and the packets dropped at stop, read through the     *)
(*                   cell addresses, are exactly the packets accepted in   *)
(*                   order: nothing lost, repeated or overwritten.         *)
(*   ExclusiveOwnership  each bank is the producer, the worker's bank, or  *)
(*                   in exactly one position of one queue.                 *)
(*   InBounds        no queue position at or past NumBanks is touched.     *)
(*   ExitSentAll     when the worker exits, nothing is left ready, in      *)
(*                   flight or in the producer.                            *)
(*   EveryAcceptedSent (liveness) every accepted packet is sent unless     *)
(*                   stop drops it, with no wait timeout.                  *)
(*   StopCompletes   (liveness) once stop begins the worker exits.         *)
(*                                                                         *)
(* Rule switches, each changed in one reversed witness:                    *)
(*   FullSlack         0 is the code's full test (count = capacity);       *)
(*                     1 tests one packet late and writes past the bank.   *)
(*   HeadMod, TailMod  NumBanks is the code's modulo for a head advance    *)
(*                     and for the tail position ( head + count ); a       *)
(*                     tail that wraps at NumBanks - 1 lands in the wrong  *)
(*                     position; 0 never wraps.                            *)
(*   QuitUnderLock     the worker reads quit under the mutex with the      *)
(*                     empty test (the fix of f0ee90fe); FALSE reads it    *)
(*                     after the release, the former race.                 *)
(*   PublishBeforeQuit stop publishes the producer, then sets quit; FALSE  *)
(*                     sets quit first.                                    *)
(*   PublishUntilSent  a refused final publish keeps the producer and each *)
(*                     portable_stopped poll tries again (MStopRetry), so  *)
(*                     quit follows the publish; FALSE is the one try that *)
(*                     counted the producer dropped (StopDropsNothing).    *)
(*   RetryOnIdle       portable_receive retries a deferred publish.        *)
(*   WakeOnPublish     a successful publish signals the worker.            *)
(*   Timeout           the worker's wait times out (0.1 s in the code).    *)
(*                     The liveness controls run with Timeout = FALSE.     *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS NumBanks,           \* PORTABLE_TX_BANKS (3 in the code)
          Cap,                \* tx_capacity: packets per bank
          Batch,              \* the most packets one portable_send call carries
          MaxPackets,         \* packets the harness offers in all
          FullSlack, HeadMod, TailMod,
          QuitUnderLock, PublishBeforeQuit, PublishUntilSent, RetryOnIdle, WakeOnPublish, Timeout

ASSUME NumBanks >= 3 /\ Cap >= 1 /\ Batch >= 1 /\ MaxPackets >= 1
ASSUME FullSlack \in 0..1 /\ HeadMod \in 0..NumBanks /\ TailMod \in 0..NumBanks
ASSUME {QuitUnderLock, PublishBeforeQuit, PublishUntilSent, RetryOnIdle, WakeOnPublish, Timeout} \subseteq BOOLEAN

Banks == 0..NumBanks - 1
NoBank == -1
Pos == 0..4 * NumBanks
Cells == 0..NumBanks * Cap          \* the last cell is one past the allocation
Ids == 1..MaxPackets

VARIABLES cells,       \* the packet cells of all banks, bank b slot s at b * Cap + s
          count,       \* per bank: bank->count
          producer,    \* server->producer, main only
          freeIdx,     \* free_index
          freeHead,    \* free_head
          freeCount,   \* free_count
          readyIdx,    \* ready_index
          readyHead,   \* ready_head
          readyCount,  \* ready_count
          lock,        \* server->mutex holder: "none", "worker" or "main"
          wake,        \* server->wake: a pending signal
          quit,        \* server->quit
          stopping,    \* server->stopping
          oob,         \* a queue position at or past NumBanks was read or written
          nextId, accepted, sent, stopDropped,   \* the abstract contract
          wpc, inflight, sendpos, exitEmpty,     \* the worker
          mpc, left, ret, pubOk                  \* main

dataVars == <<cells, count>>
ringVars == <<producer, freeIdx, freeHead, freeCount, readyIdx, readyHead, readyCount>>
flagVars == <<lock, wake, quit, stopping, oob>>
ghostVars == <<nextId, accepted, sent, stopDropped>>
workerVars == <<wpc, inflight, sendpos, exitEmpty>>
mainVars == <<mpc, left, ret, pubOk>>

vars == <<cells, count, producer, freeIdx, freeHead, freeCount, readyIdx, readyHead,
          readyCount, lock, wake, quit, stopping, oob, nextId, accepted, sent,
          stopDropped, wpc, inflight, sendpos, exitEmpty, mpc, left, ret, pubOk>>

Range(s) == {s[i] : i \in 1..Len(s)}
Red(x) == IF HeadMod = 0 THEN x ELSE x % HeadMod      \* a head advances by Red( head + 1 )
Tl(x) == IF TailMod = 0 THEN x ELSE x % TailMod       \* the tail position ( head + count ) % banks
Past(p) == p >= NumBanks
Addr(b, s) == b * Cap + s
Contents(b) == [i \in 1..count[b] |-> cells[Addr(b, i - 1)]]

Init ==
    /\ cells = [c \in Cells |-> 0]
    /\ count = [b \in Banks |-> 0]
    /\ producer = 0
    /\ freeIdx = [p \in Pos |-> IF p < NumBanks - 1 THEN p + 1 ELSE 0]
    /\ freeHead = 0
    /\ freeCount = NumBanks - 1
    /\ readyIdx = [p \in Pos |-> 0]
    /\ readyHead = 0
    /\ readyCount = 0
    /\ lock = "none"
    /\ wake = FALSE
    /\ quit = FALSE
    /\ stopping = FALSE
    /\ oob = FALSE
    /\ nextId = 1
    /\ accepted = <<>>
    /\ sent = <<>>
    /\ stopDropped = <<>>
    /\ wpc = "top"
    /\ inflight = NoBank
    /\ sendpos = 0
    /\ exitEmpty = FALSE
    /\ mpc = "idle"
    /\ left = 0
    /\ ret = "idle"
    /\ pubOk = FALSE

\* ------------------------------------------------------------------------
\* Main.  portable_send, portable_receive, portable_stop.

\* portable_send begins: a batch of up to Batch packets
MCall ==
    /\ mpc = "idle"
    /\ ~stopping
    /\ \E n \in 1..Batch :
          /\ nextId + n - 1 <= MaxPackets
          /\ left' = n
    /\ mpc' = "put"
    /\ UNCHANGED <<dataVars, ringVars, flagVars, ghostVars, workerVars, ret, pubOk>>

\* the loop of portable_send: a full producer is published first, else the
\* packet is copied to the slot at count
MPut ==
    /\ mpc = "put"
    /\ left > 0
    /\ IF count[producer] = Cap + FullSlack
         THEN /\ mpc' = "pub"
              /\ ret' = "put"
              /\ UNCHANGED <<dataVars, ghostVars, left>>
         ELSE /\ cells' = [cells EXCEPT ![Addr(producer, count[producer])] = nextId]
              /\ count' = [count EXCEPT ![producer] = @ + 1]
              /\ accepted' = Append(accepted, nextId)
              /\ nextId' = nextId + 1
              /\ left' = left - 1
              /\ UNCHANGED <<mpc, ret, sent, stopDropped>>
    /\ UNCHANGED <<ringVars, flagVars, workerVars, pubOk>>

\* the call's last line: portable_tx_publish( server )
MPutEnd ==
    /\ mpc = "put"
    /\ left = 0
    /\ mpc' = "pub"
    /\ ret' = "end"
    /\ UNCHANGED <<dataVars, ringVars, flagVars, ghostVars, workerVars, left, pubOk>>

\* portable_receive: retry a deferred batch even on an idle update
MRecv ==
    /\ mpc = "idle"
    /\ IF RetryOnIdle
         THEN /\ mpc' = "pub"
              /\ ret' = "idle"
              /\ UNCHANGED <<dataVars, ringVars, flagVars, ghostVars, workerVars, left, pubOk>>
         ELSE UNCHANGED vars

\* portable_tx_publish: an empty producer needs no exchange
MPubEmpty ==
    /\ mpc = "pub"
    /\ count[producer] = 0
    /\ pubOk' = TRUE
    /\ mpc' = "return"
    /\ UNCHANGED <<dataVars, ringVars, flagVars, ghostVars, workerVars, left, ret>>

\* the try succeeds: the mutex was free
MPubTry ==
    /\ mpc = "pub"
    /\ count[producer] > 0
    /\ lock = "none"
    /\ lock' = "main"
    /\ mpc' = "swap"
    /\ UNCHANGED <<dataVars, ringVars, ghostVars, workerVars, left, ret, pubOk, wake,
                   quit, stopping, oob>>

\* the try fails: the producer stays with main and main goes on
MPubBusy ==
    /\ mpc = "pub"
    /\ count[producer] > 0
    /\ lock # "none"
    /\ pubOk' = FALSE
    /\ mpc' = "return"
    /\ UNCHANGED <<dataVars, ringVars, flagVars, ghostVars, workerVars, left, ret>>

\* under the mutex: refuse when no bank is free, else queue the producer and
\* take the oldest free bank
MPubSwap ==
    /\ mpc = "swap"
    /\ IF freeCount = 0
         THEN /\ pubOk' = FALSE
              /\ mpc' = "return"
              /\ UNCHANGED <<ringVars, oob>>
         ELSE /\ producer' = freeIdx[freeHead]
              /\ freeHead' = Red(freeHead + 1)
              /\ freeCount' = freeCount - 1
              /\ readyIdx' = [readyIdx EXCEPT ![Tl(readyHead + readyCount)] = producer]
              /\ readyCount' = readyCount + 1
              /\ oob' = (oob \/ Past(freeHead) \/ Past(Tl(readyHead + readyCount)))
              /\ mpc' = "after"
              /\ UNCHANGED <<freeIdx, readyHead, pubOk>>
    /\ lock' = "none"
    /\ UNCHANGED <<dataVars, ghostVars, workerVars, left, ret, wake, quit, stopping>>

\* after the release: the new producer's count = 0, then the worker's wake
MPubAfter ==
    /\ mpc = "after"
    /\ count' = [count EXCEPT ![producer] = 0]
    /\ wake' = (wake \/ WakeOnPublish)
    /\ pubOk' = TRUE
    /\ mpc' = "return"
    /\ UNCHANGED <<cells, ringVars, ghostVars, workerVars, left, ret, lock, quit,
                   stopping, oob>>

\* the publish returns to its caller
MReturn ==
    /\ mpc = "return"
    /\ CASE ret \in {"idle", "end"} ->
              /\ mpc' = "idle"
              /\ UNCHANGED <<count, stopDropped, left>>
         [] ret = "put" ->
              /\ IF pubOk
                   THEN /\ mpc' = "put"
                        /\ UNCHANGED left
                   ELSE /\ mpc' = "idle"     \* the rest of the batch is dropped
                        /\ left' = 0
              /\ UNCHANGED <<count, stopDropped>>
         [] ret = "stop" ->
              /\ IF pubOk
                   THEN /\ mpc' = IF PublishBeforeQuit THEN "stopq" ELSE "stopped"
                        /\ UNCHANGED <<count, stopDropped>>
                   ELSE IF PublishUntilSent
                   THEN /\ mpc' = "stopretry"
                        /\ UNCHANGED <<count, stopDropped>>
                   ELSE /\ mpc' = IF PublishBeforeQuit THEN "stopq" ELSE "stopped"
                        /\ stopDropped' = stopDropped \o Contents(producer)
                        /\ count' = [count EXCEPT ![producer] = 0]
              /\ UNCHANGED left
    /\ UNCHANGED <<cells, ringVars, flagVars, nextId, accepted, sent, workerVars, ret, pubOk>>

\* portable_stop: stopping, then the final producer, then quit, then the wake.
\* With PublishBeforeQuit = FALSE, quit and the wake come first.
MStop ==
    /\ mpc = "idle"
    /\ ~stopping
    /\ stopping' = TRUE
    /\ mpc' = "pub"
    /\ ret' = "stop"
    /\ IF PublishBeforeQuit
         THEN UNCHANGED <<quit, wake>>
         ELSE /\ quit' = TRUE
              /\ wake' = TRUE
    /\ UNCHANGED <<dataVars, ringVars, lock, oob, ghostVars, workerVars, left, pubOk>>

MStopQuit ==
    /\ mpc = "stopq"
    /\ quit' = TRUE
    /\ mpc' = "stopwake"
    /\ UNCHANGED <<dataVars, ringVars, lock, wake, stopping, oob, ghostVars, workerVars,
                   left, ret, pubOk>>

MStopWake ==
    /\ mpc = "stopwake"
    /\ wake' = TRUE
    /\ mpc' = "stopped"
    /\ UNCHANGED <<dataVars, ringVars, lock, quit, stopping, oob, ghostVars, workerVars,
                   left, ret, pubOk>>

\* portable_stopped while stop's last producer waits: one more publish try,
\* and quit after it succeeds
MStopRetry ==
    /\ mpc = "stopretry"
    /\ mpc' = "pub"
    /\ ret' = "stop"
    /\ UNCHANGED <<dataVars, ringVars, flagVars, ghostVars, workerVars, left, pubOk>>

\* after stop main polls portable_stopped between frames; nothing changes
MPollStopped ==
    /\ mpc = "stopped"
    /\ UNCHANGED vars

\* ------------------------------------------------------------------------
\* The worker, portable_tx_worker.

\* the loop's blocking acquire
WTop ==
    /\ wpc = "top"
    /\ lock = "none"
    /\ lock' = "worker"
    /\ wpc' = "take"
    /\ UNCHANGED <<dataVars, ringVars, ghostVars, mainVars, wake, quit, stopping, oob,
                   inflight, sendpos, exitEmpty>>

\* under the mutex: take the oldest ready bank, or note empty (and, in the
\* code, quit) and release
WTake ==
    /\ wpc = "take"
    /\ lock' = "none"
    /\ IF readyCount > 0
         THEN /\ inflight' = readyIdx[readyHead]
              /\ readyHead' = Red(readyHead + 1)
              /\ readyCount' = readyCount - 1
              /\ sendpos' = 0
              /\ oob' = (oob \/ Past(readyHead))
              /\ wpc' = "send"
              /\ UNCHANGED exitEmpty
         ELSE /\ exitEmpty' = (QuitUnderLock /\ quit)
              /\ wpc' = "empty"
              /\ UNCHANGED <<inflight, sendpos, readyHead, readyCount, oob>>
    /\ UNCHANGED <<dataVars, producer, freeIdx, freeHead, freeCount, readyIdx, ghostVars,
                   mainVars, wake, quit, stopping>>

\* after the release: exit on the empty-and-quit observation, else wait
WEmpty ==
    /\ wpc = "empty"
    /\ wpc' = IF (QuitUnderLock /\ exitEmpty) \/ (~QuitUnderLock /\ quit)
                THEN "exited" ELSE "wait"
    /\ UNCHANGED <<dataVars, ringVars, flagVars, ghostVars, mainVars, inflight, sendpos,
                   exitEmpty>>

\* wait on the wake, with or without its 0.1 s timeout
WWait ==
    /\ wpc = "wait"
    /\ \/ /\ wake
          /\ wake' = FALSE
       \/ /\ Timeout
          /\ UNCHANGED wake
    /\ wpc' = "top"
    /\ UNCHANGED <<dataVars, ringVars, ghostVars, mainVars, lock, quit, stopping, oob,
                   inflight, sendpos, exitEmpty>>

\* rocketnet_platform_socket_send_packets: one packet of the bank in flight
WSend ==
    /\ wpc = "send"
    /\ sendpos < count[inflight]
    /\ sent' = Append(sent, cells[Addr(inflight, sendpos)])
    /\ sendpos' = sendpos + 1
    /\ UNCHANGED <<dataVars, ringVars, flagVars, mainVars, nextId, accepted, stopDropped,
                   wpc, inflight, exitEmpty>>

\* bank->count = 0, then the return to the free queue
WSendDone ==
    /\ wpc = "send"
    /\ sendpos >= count[inflight]
    /\ count' = [count EXCEPT ![inflight] = 0]
    /\ wpc' = "retlock"
    /\ UNCHANGED <<cells, ringVars, flagVars, mainVars, ghostVars, inflight, sendpos,
                   exitEmpty>>

WRetLock ==
    /\ wpc = "retlock"
    /\ lock = "none"
    /\ lock' = "worker"
    /\ wpc' = "retswap"
    /\ UNCHANGED <<dataVars, ringVars, ghostVars, mainVars, wake, quit, stopping, oob,
                   inflight, sendpos, exitEmpty>>

\* under the mutex: free_index[( free_head + free_count ) % banks] = index
WRetSwap ==
    /\ wpc = "retswap"
    /\ freeIdx' = [freeIdx EXCEPT ![Tl(freeHead + freeCount)] = inflight]
    /\ freeCount' = freeCount + 1
    /\ oob' = (oob \/ Past(Tl(freeHead + freeCount)))
    /\ lock' = "none"
    /\ inflight' = NoBank
    /\ wpc' = "top"
    /\ UNCHANGED <<dataVars, producer, freeHead, readyIdx, readyHead, readyCount, ghostVars,
                   mainVars, wake, quit, stopping, sendpos, exitEmpty>>

Next ==
    \/ MCall \/ MPut \/ MPutEnd \/ MRecv
    \/ MPubEmpty \/ MPubTry \/ MPubBusy \/ MPubSwap \/ MPubAfter \/ MReturn
    \/ MStop \/ MStopQuit \/ MStopWake \/ MStopRetry \/ MPollStopped
    \/ WTop \/ WTake \/ WEmpty \/ WWait \/ WSend \/ WSendDone \/ WRetLock \/ WRetSwap

Spec == Init /\ [][Next]_vars

WorkerStep == WTake \/ WEmpty \/ WWait \/ WSend \/ WSendDone \/ WRetSwap
MainStep == MPut \/ MPutEnd \/ MPubEmpty \/ MPubBusy \/ MPubSwap \/ MPubAfter \/ MReturn
            \/ MStopQuit \/ MStopWake \/ MStopRetry

\* The worker runs whenever it can, and its blocking acquires are granted if
\* the mutex is free infinitely often (main holds it for an index swap only).
\* Main completes its calls, keeps calling portable_receive, and its try
\* succeeds if it is enabled infinitely often.  Stop is not forced:
\* StopCompletes is conditional on stop starting.
SpecFair ==
    /\ Spec
    /\ WF_vars(WorkerStep) /\ SF_vars(WTop) /\ SF_vars(WRetLock)
    /\ WF_vars(MainStep) /\ SF_vars(MPubTry) /\ SF_vars(MRecv)

-----------------------------------------------------------------------------

TypeOK ==
    /\ cells \in [Cells -> 0..MaxPackets]
    /\ count \in [Banks -> 0..Cap + FullSlack]
    /\ producer \in Banks
    /\ freeIdx \in [Pos -> Banks]
    /\ freeHead \in Pos
    /\ freeCount \in 0..NumBanks
    /\ readyIdx \in [Pos -> Banks]
    /\ readyHead \in Pos
    /\ readyCount \in 0..NumBanks
    /\ lock \in {"none", "worker", "main"}
    /\ {wake, quit, stopping, oob} \subseteq BOOLEAN
    /\ nextId \in 1..MaxPackets + 1
    /\ {accepted, sent, stopDropped} \subseteq Seq(Ids)
    /\ inflight \in Banks \cup {NoBank}
    /\ sendpos \in 0..Cap + FullSlack

\* No queue position at or past the array's end is read or written.
InBounds == ~oob

FreeSeq == [i \in 1..freeCount |-> freeIdx[Red(freeHead + i - 1)]]
ReadySeq == [i \in 1..readyCount |-> readyIdx[Red(readyHead + i - 1)]]
InflightSeq == IF inflight = NoBank THEN <<>> ELSE <<inflight>>
Owners == <<producer>> \o InflightSeq \o FreeSeq \o ReadySeq

RECURSIVE Concat(_)
Concat(s) == IF s = <<>> THEN <<>> ELSE Contents(Head(s)) \o Concat(Tail(s))

\* Every bank has exactly one owner: the producer, the worker's bank, or one
\* position of the free or ready queue.
ExclusiveOwnership ==
    /\ Len(Owners) = NumBanks
    /\ Range(Owners) = Banks

\* The banks the worker returned are empty when main takes them.
FreeBanksEmpty ==
    \A i \in 1..freeCount : count[FreeSeq[i]] = 0

RemainingInflight ==
    IF inflight = NoBank THEN <<>>
    ELSE SubSeq(Contents(inflight), sendpos + 1, Len(Contents(inflight)))

\* Exact delivery through the cell addresses: nothing lost, repeated or
\* overwritten, in the order accepted.
ExactSend ==
    sent \o RemainingInflight \o Concat(ReadySeq) \o Contents(producer) \o stopDropped
      = accepted

\* Main holds the mutex only inside an index swap: its lock is a try.
MainLockIsBrief ==
    lock = "main" => mpc = "swap"

\* Stop publishes or drops the last producer before quit is set.
QuitAfterFinalBank ==
    quit => count[producer] = 0

\* Stop drops nothing: the last producer is published, however long its
\* try is refused.
StopDropsNothing == stopDropped = <<>>

\* The worker exits only with nothing left to send.
ExitSentAll ==
    wpc = "exited" => /\ readyCount = 0
                      /\ inflight = NoBank
                      /\ count[producer] = 0

\* Liveness: every accepted packet is sent unless stop drops it.
EveryAcceptedSent ==
    \A i \in Ids : (i \in Range(accepted)) ~> (i \in Range(sent) \/ i \in Range(stopDropped))

\* Liveness: once stop begins the worker exits.
StopCompletes ==
    stopping ~> (wpc = "exited")

=============================================================================
