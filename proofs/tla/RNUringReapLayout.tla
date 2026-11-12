--------------------------- MODULE RNUringReapLayout ---------------------------
(***************************************************************************)
(* The io_uring server's reap at the code's layout                         *)
(* (sdk/rocketnet_server_uring_linux.cpp rocketnet_server_uring_reap_available, *)
(* rocketnet_server_uring_receive_or_hold, rocketnet_server_uring_copy_held). *)
(*                                                                         *)
(* The completion queue is a ring of CqCap entries the kernel writes at    *)
(* its tail counter and the reap reads at its head counter, each entry at  *)
(* counter % CqCap, oldest first: the reap cannot take an entry before the *)
(* one at the head.  A receive entry names its provided buffer, which      *)
(* belongs to one of Q queues; a send entry is a send's completion.  The   *)
(* fill bank counts datagrams; each receive carries Seg of them, and a     *)
(* receive is copied only while bank + Seg <= Room (the code's             *)
(* MAX_SEGMENTS against BANK_SIZE).                                        *)
(*                                                                         *)
(* A receive without room, or reaped while an earlier receive of its       *)
(* queue is held, is held: pushed at (hHead[q] + hCount[q]) % HeldCap of   *)
(* its queue's ring of HeldCap buffer ids, its buffer still the server's.  *)
(* The held receives are copied while the bank has room, at the start of   *)
(* every reap (CopyHeld): one receive of each queue that holds any in      *)
(* turn, from the queue turn names, each queue's oldest first.  A call     *)
(* (send_packets after its submit, or receive_packets, which swaps the     *)
(* bank at its end) reaps at most Budget entries.                          *)
(*                                                                         *)
(* Three rule switches and one constant carry the reversed witnesses.     *)
(* HoldRule = FALSE is the reap before the hold: it stops at a receive    *)
(* without room and the call ends there, so a send entry behind it waits  *)
(* for a swap (SendsNotBehindRoom).  HeldMod = FALSE pushes at            *)
(* hHead[q] + hCount[q] without the modulus (HeldMatches).  A HeldCap     *)
(* below a queue's buffers is a hold ring one short of the buffers it     *)
(* must hold (HeldMatches).  TurnRule = FALSE copies from the first queue *)
(* that holds any, every time, as one line would (TurnsTaken).            *)
(***************************************************************************)
EXTENDS Naturals, Sequences

CONSTANTS CqCap,      \* completion queue entries
          Q,          \* queues
          HeldCap,    \* hold ring slots per queue (the code: one per provided buffer of the queue)
          Buffers,    \* provided receive buffers, (b - 1) % Q the queue of buffer b
          Room,       \* datagrams the fill bank holds between two swaps
          Seg,        \* datagrams one receive carries
          Budget,     \* entries one call may reap
          MaxRecv,    \* bound on receives the kernel posts
          MaxSends,   \* bound on sends submitted
          HoldRule,   \* a receive without room is held, not left at the head
          HeldMod,    \* the push index wraps at HeldCap
          TurnRule    \* the copy of held receives takes queues in turn

ASSUME CqCap \in Nat \ {0} /\ Q \in Nat \ {0} /\ HeldCap \in Nat \ {0} /\ Buffers \in Nat \ {0}
ASSUME Room \in Nat \ {0} /\ Seg \in Nat \ {0} /\ Seg <= Room
ASSUME Budget \in Nat \ {0} /\ MaxRecv \in Nat /\ MaxSends \in Nat
ASSUME HoldRule \in BOOLEAN /\ HeldMod \in BOOLEAN /\ TurnRule \in BOOLEAN

BufIds == 1..Buffers
Queues == 0..(Q - 1)
QueueOf(b) == (b - 1) % Q
CqIdx == 0..(CqCap - 1)
HeldIdx == 0..(HeldCap - 1)

None == [k |-> "none", x |-> 0]
SendE == [k |-> "send", x |-> 0]
RecvE(b) == [k |-> "recv", x |-> b]
Entries == {None, SendE} \cup {RecvE(b) : b \in BufIds}

VARIABLES cqArr,     \* the completion ring's entries
          cqHead,    \* the reap's counter
          cqTail,    \* the kernel's counter
          heldArr,   \* each queue's hold ring of buffer ids (0: empty)
          hHead,     \* each queue's oldest held receive's slot
          hCount,    \* each queue's held receives
          turn,      \* the queue the next copy of a held receive looks at first
          bank,      \* datagrams in the fill bank
          ring,      \* buffers the kernel may fill
          unposted,  \* sends submitted whose completion is not yet posted
          pend,      \* sends submitted whose completion is not yet reaped
          sends,     \* sends submitted so far
          posts,     \* receives posted so far
          call,      \* none, send or recv
          work,      \* entries this call has reaped
          arrived,   \* ghost: each queue's buffers in the order the kernel posted them
          copiedSeq, \* ghost: each queue's buffers in the order they were copied
          heldSeq,   \* ghost: each queue's held receives as a FIFO
          waits      \* ghost: copies of other queues' held receives since each queue holding any last had one copied

vars == <<cqArr, cqHead, cqTail, heldArr, hHead, hCount, turn, bank, ring, unposted,
          pend, sends, posts, call, work, arrived, copiedSeq, heldSeq, waits>>

Init ==
    /\ cqArr = [i \in CqIdx |-> None]
    /\ cqHead = 0 /\ cqTail = 0
    /\ heldArr = [q \in Queues |-> [i \in HeldIdx |-> 0]]
    /\ hHead = [q \in Queues |-> 0] /\ hCount = [q \in Queues |-> 0]
    /\ turn = 0
    /\ bank = 0
    /\ ring = BufIds
    /\ unposted = 0 /\ pend = 0 /\ sends = 0 /\ posts = 0
    /\ call = "none" /\ work = 0
    /\ arrived = [q \in Queues |-> << >>]
    /\ copiedSeq = [q \in Queues |-> << >>]
    /\ heldSeq = [q \in Queues |-> << >>]
    /\ waits = [q \in Queues |-> 0]

HasRoom == bank + Seg <= Room
CqFull == cqTail - cqHead >= CqCap
CqFront == cqArr[cqHead % CqCap]
HeldTotal == LET Sum[s \in SUBSET Queues] ==
                    IF s = {} THEN 0
                    ELSE LET q == CHOOSE x \in s : TRUE IN hCount[q] + Sum[s \ {q}]
             IN Sum[Queues]
PushIdx(q) == IF HeldMod THEN (hHead[q] + hCount[q]) % HeldCap ELSE hHead[q] + hCount[q]
MustHold(q) == hCount[q] > 0 \/ ~HasRoom

\* the queue the copy takes next: the first from turn that holds a receive,
\* or (TurnRule = FALSE) the first from queue 0
Step(q) == (q + 1) % Q
NextHeld(from) == CHOOSE q \in Queues :
                      /\ hCount[q] > 0
                      /\ \A d \in 0..(Q - 1) :
                            (d < (q - from + Q) % Q) => hCount[(from + d) % Q] = 0
CopyFrom == NextHeld(IF TurnRule THEN turn ELSE 0)

\* the kernel posts a receive into a buffer from the ring
KernelRecv(b) ==
    /\ b \in ring /\ posts < MaxRecv /\ ~CqFull
    /\ cqArr' = [cqArr EXCEPT ![cqTail % CqCap] = RecvE(b)]
    /\ cqTail' = cqTail + 1
    /\ ring' = ring \ {b}
    /\ posts' = posts + 1
    /\ arrived' = [arrived EXCEPT ![QueueOf(b)] = Append(@, b)]
    /\ UNCHANGED <<cqHead, heldArr, hHead, hCount, turn, bank, unposted, pend, sends,
                   call, work, copiedSeq, heldSeq, waits>>

\* the kernel posts a submitted send's completion
KernelSend ==
    /\ unposted > 0 /\ ~CqFull
    /\ cqArr' = [cqArr EXCEPT ![cqTail % CqCap] = SendE]
    /\ cqTail' = cqTail + 1
    /\ unposted' = unposted - 1
    /\ UNCHANGED <<cqHead, heldArr, hHead, hCount, turn, bank, ring, pend, sends,
                   posts, call, work, arrived, copiedSeq, heldSeq, waits>>

\* send_packets submits one send, then reaps
SendBegin ==
    /\ call = "none" /\ sends < MaxSends
    /\ call' = "send" /\ work' = 0
    /\ unposted' = unposted + 1 /\ pend' = pend + 1 /\ sends' = sends + 1
    /\ UNCHANGED <<cqArr, cqHead, cqTail, heldArr, hHead, hCount, turn, bank, ring,
                   posts, arrived, copiedSeq, heldSeq, waits>>

\* receive_packets reaps, then swaps
RecvBegin ==
    /\ call = "none"
    /\ call' = "recv" /\ work' = 0
    /\ UNCHANGED <<cqArr, cqHead, cqTail, heldArr, hHead, hCount, turn, bank, ring,
                   unposted, pend, sends, posts, arrived, copiedSeq, heldSeq, waits>>

\* the oldest held receive of the queue whose turn it is into the bank, its
\* buffer back to the ring; the turn passes to the queue after it
CopyHeld ==
    /\ call # "none" /\ HeldTotal > 0 /\ HasRoom
    /\ LET q == CopyFrom
           b == heldArr[q][hHead[q]]
       IN /\ heldArr' = [heldArr EXCEPT ![q][hHead[q]] = 0]
          /\ ring' = ring \cup {b}
          /\ copiedSeq' = [copiedSeq EXCEPT ![q] = Append(@, b)]
          /\ hHead' = [hHead EXCEPT ![q] = (@ + 1) % HeldCap]
          /\ hCount' = [hCount EXCEPT ![q] = @ - 1]
          /\ heldSeq' = [heldSeq EXCEPT ![q] = Tail(@)]
          /\ turn' = IF TurnRule THEN Step(q) ELSE turn
          /\ waits' = [r \in Queues |-> IF r = q \/ hCount[r] = 0 THEN 0 ELSE waits[r] + 1]
    /\ bank' = bank + Seg
    /\ UNCHANGED <<cqArr, cqHead, cqTail, unposted, pend, sends, posts, call,
                   work, arrived>>

\* the entry at the head, once the held receives are copied as far as the
\* room goes: a send is retired; a receive is copied, held, or (HoldRule =
\* FALSE, no room) not reaped at all
Reap ==
    /\ call # "none" /\ work < Budget /\ cqHead < cqTail
    /\ HeldTotal = 0 \/ ~HasRoom
    /\ CqFront.k = "recv" /\ ~HoldRule => HasRoom
    /\ cqHead' = cqHead + 1
    /\ work' = work + 1
    /\ cqArr' = [cqArr EXCEPT ![cqHead % CqCap] = None]
    /\ IF CqFront.k = "send"
       THEN /\ pend' = pend - 1
            /\ UNCHANGED <<heldArr, hHead, hCount, bank, ring, copiedSeq, heldSeq>>
       ELSE LET b == CqFront.x
                q == QueueOf(b)
            IN IF HoldRule /\ MustHold(q)
               THEN /\ heldArr' = [heldArr EXCEPT ![q] = [i \in HeldIdx |-> IF i = PushIdx(q) THEN b ELSE heldArr[q][i]]]
                    /\ hCount' = [hCount EXCEPT ![q] = @ + 1]
                    /\ heldSeq' = [heldSeq EXCEPT ![q] = Append(@, b)]
                    /\ UNCHANGED <<hHead, bank, ring, copiedSeq, pend>>
               ELSE /\ bank' = bank + Seg
                    /\ ring' = ring \cup {b}
                    /\ copiedSeq' = [copiedSeq EXCEPT ![q] = Append(@, b)]
                    /\ UNCHANGED <<heldArr, hHead, hCount, heldSeq, pend>>
    /\ UNCHANGED <<cqTail, unposted, sends, posts, call, arrived, turn, waits>>

\* the call ends: the queue is empty or the budget spent (or, without the
\* hold, a receive without room at the head), and the held receives are
\* copied as far as the room goes; receive_packets then swaps the bank
CallEnd ==
    /\ call # "none"
    /\ \/ cqHead = cqTail
       \/ work >= Budget
       \/ ~HoldRule /\ CqFront.k = "recv" /\ ~HasRoom
    /\ HeldTotal = 0 \/ ~HasRoom
    /\ call' = "none" /\ work' = 0
    /\ bank' = IF call = "recv" THEN 0 ELSE bank
    /\ UNCHANGED <<cqArr, cqHead, cqTail, heldArr, hHead, hCount, turn, ring, unposted,
                   pend, sends, posts, arrived, copiedSeq, heldSeq, waits>>

Next ==
    \/ \E b \in BufIds : KernelRecv(b)
    \/ KernelSend
    \/ SendBegin
    \/ RecvBegin
    \/ CopyHeld
    \/ Reap
    \/ CallEnd

Spec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------

TypeOK ==
    /\ cqArr \in [CqIdx -> Entries]
    /\ cqHead \in Nat /\ cqTail \in Nat /\ cqHead <= cqTail
    /\ heldArr \in [Queues -> [HeldIdx -> 0..Buffers]]
    /\ hHead \in [Queues -> HeldIdx] /\ hCount \in [Queues -> Nat]
    /\ turn \in Queues
    /\ bank \in Nat
    /\ ring \subseteq BufIds
    /\ call \in {"none", "send", "recv"}
    /\ work \in 0..Budget

\* each queue's hold ring holds exactly its held FIFO, oldest at hHead, each
\* slot read at its modulus, and never more than its slots
HeldMatches ==
    \A q \in Queues :
        /\ hCount[q] = Len(heldSeq[q])
        /\ hCount[q] <= HeldCap
        /\ \A i \in 1..hCount[q] : heldArr[q][(hHead[q] + i - 1) % HeldCap] = heldSeq[q][i]

\* each queue's receives reach the bank in the order the kernel posted them
InOrder ==
    \A q \in Queues :
        /\ Len(copiedSeq[q]) <= Len(arrived[q])
        /\ \A i \in 1..Len(copiedSeq[q]) : copiedSeq[q][i] = arrived[q][i]

\* a queue holding receives has one copied before any other queue has two:
\* at most one copy of each other queue's held receives comes between
TurnsTaken ==
    \A q \in Queues : waits[q] <= Q - 1

\* the fill bank never takes more than Room
BankWithinRoom ==
    bank <= Room

\* no call ends with a send's completion still in the queue while it has
\* budget left: a receive without room never holds one behind it
SendsNotBehindRoom ==
    [][(call # "none" /\ call' = "none")
         => (work >= Budget \/ \A i \in cqHead..(cqTail - 1) : cqArr[i % CqCap].k # "send")]_vars

=============================================================================
