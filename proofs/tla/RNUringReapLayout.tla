--------------------------- MODULE RNUringReapLayout ---------------------------
(***************************************************************************)
(* The io_uring server's reap at the code's layout                         *)
(* (sdk/rocketnet_server_uring_linux.cpp rocketnet_server_uring_reap_available, *)
(* rocketnet_server_uring_receive_or_hold, rocketnet_server_uring_copy_held). *)
(*                                                                         *)
(* The completion queue is a ring of CqCap entries the kernel writes at    *)
(* its tail counter and the reap reads at its head counter, each entry at  *)
(* counter % CqCap, oldest first: the reap cannot take an entry before the *)
(* one at the head.  A receive entry names its provided buffer; a send     *)
(* entry is a send's completion.  The fill bank counts datagrams; each     *)
(* receive carries Seg of them, and a receive is copied only while         *)
(* bank + Seg <= Room (the code's MAX_SEGMENTS against BANK_SIZE).         *)
(*                                                                         *)
(* A receive without room, or reaped while an earlier one is held, is      *)
(* held: pushed at (hHead + hCount) % HeldCap of a ring of HeldCap         *)
(* buffer ids, its buffer still the server's.  The held receives are       *)
(* copied, oldest first from hHead, while the bank has room, at the start  *)
(* of every reap (CopyHeld).  A call (send_packets after its submit, or    *)
(* receive_packets, which swaps the bank at its end) reaps at most Budget  *)
(* entries.                                                                *)
(*                                                                         *)
(* Two rule switches and one constant carry the reversed witnesses.       *)
(* HoldRule = FALSE is the reap before the hold: it stops at a receive    *)
(* without room and the call ends there, so a send entry behind it waits  *)
(* for a swap (SendsNotBehindRoom).  HeldMod = FALSE pushes at            *)
(* hHead + hCount without the modulus (HeldMatches).  A HeldCap below     *)
(* Buffers is a hold ring one short of the buffers it must hold           *)
(* (HeldMatches).                                                          *)
(***************************************************************************)
EXTENDS Naturals, Sequences

CONSTANTS CqCap,      \* completion queue entries
          HeldCap,    \* hold ring slots (the code: one per provided buffer)
          Buffers,    \* provided receive buffers
          Room,       \* datagrams the fill bank holds between two swaps
          Seg,        \* datagrams one receive carries
          Budget,     \* entries one call may reap
          MaxRecv,    \* bound on receives the kernel posts
          MaxSends,   \* bound on sends submitted
          HoldRule,   \* a receive without room is held, not left at the head
          HeldMod     \* the push index wraps at HeldCap

ASSUME CqCap \in Nat \ {0} /\ HeldCap \in Nat \ {0} /\ Buffers \in Nat \ {0}
ASSUME Room \in Nat \ {0} /\ Seg \in Nat \ {0} /\ Seg <= Room
ASSUME Budget \in Nat \ {0} /\ MaxRecv \in Nat /\ MaxSends \in Nat
ASSUME HoldRule \in BOOLEAN /\ HeldMod \in BOOLEAN

BufIds == 1..Buffers
CqIdx == 0..(CqCap - 1)
HeldIdx == 0..(HeldCap - 1)

None == [k |-> "none", x |-> 0]
SendE == [k |-> "send", x |-> 0]
RecvE(b) == [k |-> "recv", x |-> b]
Entries == {None, SendE} \cup {RecvE(b) : b \in BufIds}

VARIABLES cqArr,     \* the completion ring's entries
          cqHead,    \* the reap's counter
          cqTail,    \* the kernel's counter
          heldArr,   \* the hold ring's buffer ids (0: empty)
          hHead,     \* the oldest held receive's slot
          hCount,    \* held receives
          bank,      \* datagrams in the fill bank
          ring,      \* buffers the kernel may fill
          unposted,  \* sends submitted whose completion is not yet posted
          pend,      \* sends submitted whose completion is not yet reaped
          sends,     \* sends submitted so far
          posts,     \* receives posted so far
          call,      \* none, send or recv
          work,      \* entries this call has reaped
          arrived,   \* ghost: buffers in the order the kernel posted them
          copiedSeq, \* ghost: buffers in the order they were copied
          heldSeq    \* ghost: the held receives as a FIFO

vars == <<cqArr, cqHead, cqTail, heldArr, hHead, hCount, bank, ring, unposted,
          pend, sends, posts, call, work, arrived, copiedSeq, heldSeq>>

Init ==
    /\ cqArr = [i \in CqIdx |-> None]
    /\ cqHead = 0 /\ cqTail = 0
    /\ heldArr = [i \in HeldIdx |-> 0]
    /\ hHead = 0 /\ hCount = 0
    /\ bank = 0
    /\ ring = BufIds
    /\ unposted = 0 /\ pend = 0 /\ sends = 0 /\ posts = 0
    /\ call = "none" /\ work = 0
    /\ arrived = << >> /\ copiedSeq = << >> /\ heldSeq = << >>

HasRoom == bank + Seg <= Room
CqFull == cqTail - cqHead >= CqCap
CqFront == cqArr[cqHead % CqCap]
PushIdx == IF HeldMod THEN (hHead + hCount) % HeldCap ELSE hHead + hCount
MustHold == hCount > 0 \/ ~HasRoom

\* the kernel posts a receive into a buffer from the ring
KernelRecv(b) ==
    /\ b \in ring /\ posts < MaxRecv /\ ~CqFull
    /\ cqArr' = [cqArr EXCEPT ![cqTail % CqCap] = RecvE(b)]
    /\ cqTail' = cqTail + 1
    /\ ring' = ring \ {b}
    /\ posts' = posts + 1
    /\ arrived' = Append(arrived, b)
    /\ UNCHANGED <<cqHead, heldArr, hHead, hCount, bank, unposted, pend, sends,
                   call, work, copiedSeq, heldSeq>>

\* the kernel posts a submitted send's completion
KernelSend ==
    /\ unposted > 0 /\ ~CqFull
    /\ cqArr' = [cqArr EXCEPT ![cqTail % CqCap] = SendE]
    /\ cqTail' = cqTail + 1
    /\ unposted' = unposted - 1
    /\ UNCHANGED <<cqHead, heldArr, hHead, hCount, bank, ring, pend, sends,
                   posts, call, work, arrived, copiedSeq, heldSeq>>

\* send_packets submits one send, then reaps
SendBegin ==
    /\ call = "none" /\ sends < MaxSends
    /\ call' = "send" /\ work' = 0
    /\ unposted' = unposted + 1 /\ pend' = pend + 1 /\ sends' = sends + 1
    /\ UNCHANGED <<cqArr, cqHead, cqTail, heldArr, hHead, hCount, bank, ring,
                   posts, arrived, copiedSeq, heldSeq>>

\* receive_packets reaps, then swaps
RecvBegin ==
    /\ call = "none"
    /\ call' = "recv" /\ work' = 0
    /\ UNCHANGED <<cqArr, cqHead, cqTail, heldArr, hHead, hCount, bank, ring,
                   unposted, pend, sends, posts, arrived, copiedSeq, heldSeq>>

\* the oldest held receive into the bank, its buffer back to the ring
CopyHeld ==
    /\ call # "none" /\ hCount > 0 /\ HasRoom
    /\ LET b == heldArr[hHead] IN
          /\ heldArr' = [heldArr EXCEPT ![hHead] = 0]
          /\ ring' = ring \cup {b}
          /\ copiedSeq' = Append(copiedSeq, b)
    /\ hHead' = (hHead + 1) % HeldCap
    /\ hCount' = hCount - 1
    /\ heldSeq' = Tail(heldSeq)
    /\ bank' = bank + Seg
    /\ UNCHANGED <<cqArr, cqHead, cqTail, unposted, pend, sends, posts, call,
                   work, arrived>>

\* the entry at the head, once the held receives are copied as far as the
\* room goes: a send is retired; a receive is copied, held, or (HoldRule =
\* FALSE, no room) not reaped at all
Reap ==
    /\ call # "none" /\ work < Budget /\ cqHead < cqTail
    /\ hCount = 0 \/ ~HasRoom
    /\ CqFront.k = "recv" /\ ~HoldRule => HasRoom
    /\ cqHead' = cqHead + 1
    /\ work' = work + 1
    /\ cqArr' = [cqArr EXCEPT ![cqHead % CqCap] = None]
    /\ IF CqFront.k = "send"
       THEN /\ pend' = pend - 1
            /\ UNCHANGED <<heldArr, hHead, hCount, bank, ring, copiedSeq, heldSeq>>
       ELSE IF HoldRule /\ MustHold
       THEN /\ heldArr' = [i \in HeldIdx |-> IF i = PushIdx THEN CqFront.x ELSE heldArr[i]]
            /\ hCount' = hCount + 1
            /\ heldSeq' = Append(heldSeq, CqFront.x)
            /\ UNCHANGED <<hHead, bank, ring, copiedSeq, pend>>
       ELSE /\ bank' = bank + Seg
            /\ ring' = ring \cup {CqFront.x}
            /\ copiedSeq' = Append(copiedSeq, CqFront.x)
            /\ UNCHANGED <<heldArr, hHead, hCount, heldSeq, pend>>
    /\ UNCHANGED <<cqTail, unposted, sends, posts, call, arrived>>

\* the call ends: the queue is empty or the budget spent (or, without the
\* hold, a receive without room at the head), and the held receives are
\* copied as far as the room goes; receive_packets then swaps the bank
CallEnd ==
    /\ call # "none"
    /\ \/ cqHead = cqTail
       \/ work >= Budget
       \/ ~HoldRule /\ CqFront.k = "recv" /\ ~HasRoom
    /\ hCount = 0 \/ ~HasRoom
    /\ call' = "none" /\ work' = 0
    /\ bank' = IF call = "recv" THEN 0 ELSE bank
    /\ UNCHANGED <<cqArr, cqHead, cqTail, heldArr, hHead, hCount, ring, unposted,
                   pend, sends, posts, arrived, copiedSeq, heldSeq>>

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
    /\ heldArr \in [HeldIdx -> 0..Buffers]
    /\ hHead \in HeldIdx /\ hCount \in Nat
    /\ bank \in Nat
    /\ ring \subseteq BufIds
    /\ call \in {"none", "send", "recv"}
    /\ work \in 0..Budget

\* the hold ring holds exactly the held FIFO, oldest at hHead, each slot
\* read at its modulus, and never more than its slots
HeldMatches ==
    /\ hCount = Len(heldSeq)
    /\ hCount <= HeldCap
    /\ \A i \in 1..hCount : heldArr[(hHead + i - 1) % HeldCap] = heldSeq[i]

\* receives reach the bank in the order the kernel posted them
InOrder ==
    /\ Len(copiedSeq) <= Len(arrived)
    /\ \A i \in 1..Len(copiedSeq) : copiedSeq[i] = arrived[i]

\* the fill bank never takes more than Room
BankWithinRoom ==
    bank <= Room

\* no call ends with a send's completion still in the queue while it has
\* budget left: a receive without room never holds one behind it
SendsNotBehindRoom ==
    [][(call # "none" /\ call' = "none")
         => (work >= Budget \/ \A i \in cqHead..(cqTail - 1) : cqArr[i % CqCap].k # "send")]_vars

=============================================================================
