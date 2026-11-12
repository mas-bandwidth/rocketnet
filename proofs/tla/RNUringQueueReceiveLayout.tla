---------------------- MODULE RNUringQueueReceiveLayout ----------------------
(***************************************************************************)
(* The io_uring server's per-queue receive at the code's layout            *)
(* (sdk/rocketnet_uring_receive_queue.cpp): the bank's slot arithmetic and *)
(* the provided buffer ring's, one model step per call the code makes.     *)
(* RNUringQueueReceive is the same design with the worker's write and      *)
(* publish as separate steps; here each completion is one step, as         *)
(* rocketnet_uring_receive_queue_cqe is one call, so the trace check       *)
(* (TraceRNUringQueueReceiveLayout) can match the code call by call.       *)
(*                                                                         *)
(* The bank: S slots and the 64-bit indexes tail (the worker's), head      *)
(* (main's) and exposed (main's), never wrapping (CounterWrap 0).  The    *)
(* worker banks position tail in slot tail % S while tail - headSeen < S,  *)
(* where headSeen is the head it last loaded; it loads head again only    *)
(* when headSeen says the bank is full, and a datagram that still finds    *)
(* no room is counted BANK_FULL.  A drain stores head := exposed (the end  *)
(* of the drain before), then takes positions exposed .. exposed + n - 1,  *)
(* n = min(tail - exposed, DrainMax), from slots (exposed + i) % S.         *)
(*                                                                         *)
(* The provided buffer ring: B entries, the kernel's head and the          *)
(* module's tail both 16-bit counters (TailWrap; 65536 in the kernel,      *)
(* smaller here so the instance wraps), the entry at counter c being       *)
(* c & (B - 1) (io_uring_buf_ring_mask).  Create adds buffer b at entry    *)
(* b & (B - 1) and advances the tail by B; the kernel takes the entry at   *)
(* its head while head # tail; a completion's buffer goes back at the      *)
(* tail's entry after every datagram in it is banked or counted.           *)
(*                                                                         *)
(* A completion carries one datagram, or with UDP_GRO a coalesced run of   *)
(* them, each accepted or refused by the transport's checks and the        *)
(* prefilter (refused: counted by name).                                   *)
(*                                                                         *)
(* The checked claims (each a flag the step that breaks it sets, or a      *)
(* state predicate):                                                       *)
(*   InOrder          main reads exactly the banked datagrams, in order.   *)
(*   NoTornRead       no slot main took is written before main is done.    *)
(*   FullOnlyWhenFull a BANK_FULL drop happens only when tail - head = S.  *)
(*   NoDoubleTake     the kernel takes only a buffer the ring holds.       *)
(*   Accounted        every datagram is at the socket, in a completion,    *)
(*                    banked, refused by name or counted BANK_FULL.        *)
(*                                                                         *)
(* Rule switches, each changed in one reversed witness:                    *)
(*   RoomRule     "refresh": load head again when headSeen says full (the  *)
(*                code); "stale" never loads it (FullOnlyWhenFull).        *)
(*   SlotRule     "mod": the worker writes slot tail % S (the code);       *)
(*                "plus1" writes (tail + 1) % S (InOrder).                 *)
(*   ReleaseRule  "next": head := the drain before's end (the code);       *)
(*                "take": head := this drain's end (NoTornRead).           *)
(*   CounterWrap  0: the code's uint64 indexes; k > 0 keeps them modulo k  *)
(*                (InOrder at S = 3, k = 4).                               *)
(*   B            a power of two (create refuses any other); B = 3 shows   *)
(*                why: the mask folds two entries into one (NoDoubleTake). *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS S,            \* bank slots
          B,            \* provided buffers
          TailWrap,     \* the buffer ring's counters wrap here (65536 in the kernel)
          MaxArrive,    \* receives the network sends (each one or more datagrams)
          MaxSegs,      \* the most datagrams one receive carries (UDP_GRO)
          DrainMax,     \* the most one drain takes
          RoomRule,     \* "refresh" (the code) or "stale"
          SlotRule,     \* "mod" (the code) or "plus1"
          ReleaseRule,  \* "next" (the code) or "take"
          CounterWrap   \* 0 (the code) or k > 0

ASSUME S >= 1 /\ B >= 1 /\ TailWrap > B /\ MaxArrive >= 1 /\ MaxSegs >= 1 /\ DrainMax >= 1
ASSUME RoomRule \in {"refresh", "stale"} /\ SlotRule \in {"mod", "plus1"}
ASSUME ReleaseRule \in {"next", "take"} /\ CounterWrap \in Nat

Slots == 0..(S - 1)
Bufs == 0..(B - 1)

RECURSIVE And(_, _)
And(x, y) == IF x = 0 \/ y = 0 THEN 0
             ELSE 2 * And(x \div 2, y \div 2) + (IF x % 2 = 1 /\ y % 2 = 1 THEN 1 ELSE 0)

Mask == B - 1
Entry(c) == And(c, Mask)

Wrap(x) == IF CounterWrap = 0 THEN x ELSE x % CounterWrap
Diff(a, b) == IF CounterWrap = 0 THEN a - b ELSE (a - b) % CounterWrap
SlotOf(t) == IF SlotRule = "mod" THEN t % S ELSE (t + 1) % S
Min(a, b) == IF a < b THEN a ELSE b

\* create: buffer b at entry b & mask, so with B a power of two entry e
\* holds buffer e; an entry no buffer was added at holds 0 (zeroed memory)
InitEntry(e) == LET bs == {b \in Bufs : Entry(b) = e}
                IN IF bs = {} THEN 0 ELSE CHOOSE b \in bs : \A c \in bs : c <= b

VARIABLES arrived,      \* receives the network sent
          nextId,       \* the next datagram's id
          sock,         \* the socket's receives, oldest first: each a sequence of [d, ok]
          karmed,       \* a multishot is live in the kernel
          cq,           \* completions not handled: [kind "data", bid, segs] or [kind "end"]
          armed,        \* the worker's armed
          stopping,
          brHead,       \* the kernel's buffer ring head (16-bit)
          brTail,       \* the module's buffer ring tail (16-bit)
          entries,      \* the buffer id each ring entry holds
          owner,        \* each buffer: "ring" or "cq"
          slotD,        \* each slot's datagram, 0 for none
          tail, head, headSeen, exposed,
          reading,      \* main is between a drain and the end of its update
          vidx,         \* the slots main's last drain took, in order
          view,         \* their datagrams at the drain
          banked,       \* the datagrams banked, in order
          delivered,    \* the datagrams main read, in order
          named,        \* refusals counted by name
          full,         \* BANK_FULL drops
          spurious, dup, torn   \* flags the broken steps set

vars == <<arrived, nextId, sock, karmed, cq, armed, stopping, brHead, brTail, entries, owner,
          slotD, tail, head, headSeen, exposed, reading, vidx, view, banked, delivered,
          named, full, spurious, dup, torn>>

Init ==
    /\ arrived = 0 /\ nextId = 1 /\ sock = <<>>
    /\ karmed = FALSE /\ cq = <<>> /\ armed = FALSE /\ stopping = FALSE
    /\ brHead = 0 /\ brTail = B % TailWrap
    /\ entries = [e \in Bufs |-> InitEntry(e)]
    /\ owner = [b \in Bufs |-> "ring"]
    /\ slotD = [s \in Slots |-> 0]
    /\ tail = 0 /\ head = 0 /\ headSeen = 0 /\ exposed = 0
    /\ reading = FALSE /\ vidx = <<>> /\ view = <<>>
    /\ banked = <<>> /\ delivered = <<>>
    /\ named = 0 /\ full = 0
    /\ spurious = FALSE /\ dup = FALSE /\ torn = FALSE

RingCount == (brTail - brHead) % TailWrap

BankVars == <<slotD, tail, headSeen, banked, named, full, spurious>>
MainVars == <<head, exposed, reading, vidx, view, delivered, torn>>
RingVars == <<brHead, brTail, entries, owner, dup>>

-----------------------------------------------------------------------------
\* the network and the kernel

\* a receive of n datagrams, each accepted or refused
Arrive ==
    /\ arrived < MaxArrive
    /\ \E n \in 1..MaxSegs, oks \in [1..MaxSegs -> BOOLEAN] :
         /\ sock' = Append(sock, [i \in 1..n |-> [d |-> nextId + i - 1, ok |-> oks[i]]])
         /\ nextId' = nextId + n
    /\ arrived' = arrived + 1
    /\ UNCHANGED <<karmed, cq, armed, stopping>> /\ UNCHANGED RingVars
    /\ UNCHANGED BankVars /\ UNCHANGED MainVars

KernelReceive ==
    /\ karmed /\ sock # <<>> /\ RingCount > 0
    /\ LET bid == entries[Entry(brHead)] IN
       /\ dup' = (dup \/ owner[bid] # "ring")
       /\ owner' = [owner EXCEPT ![bid] = "cq"]
       /\ cq' = Append(cq, [kind |-> "data", bid |-> bid, segs |-> Head(sock)])
    /\ brHead' = (brHead + 1) % TailWrap
    /\ sock' = Tail(sock)
    /\ UNCHANGED <<arrived, nextId, karmed, armed, stopping, brTail, entries>>
    /\ UNCHANGED BankVars /\ UNCHANGED MainVars

KernelNoBuffers ==
    /\ karmed /\ sock # <<>> /\ RingCount = 0
    /\ karmed' = FALSE
    /\ cq' = Append(cq, [kind |-> "end"])
    /\ UNCHANGED <<arrived, nextId, sock, armed, stopping>> /\ UNCHANGED RingVars
    /\ UNCHANGED BankVars /\ UNCHANGED MainVars

KernelCancel ==
    /\ karmed /\ stopping
    /\ karmed' = FALSE
    /\ cq' = Append(cq, [kind |-> "end"])
    /\ UNCHANGED <<arrived, nextId, sock, armed, stopping>> /\ UNCHANGED RingVars
    /\ UNCHANGED BankVars /\ UNCHANGED MainVars

\* rocketnet_uring_receive_queue_stop
Stop ==
    /\ ~stopping
    /\ stopping' = TRUE
    /\ UNCHANGED <<arrived, nextId, sock, karmed, cq, armed>> /\ UNCHANGED RingVars
    /\ UNCHANGED BankVars /\ UNCHANGED MainVars

-----------------------------------------------------------------------------
\* the worker

\* rocketnet_uring_receive_queue_needs_arm, then _arm with an entry
Arm ==
    /\ ~armed /\ ~stopping
    /\ armed' = TRUE
    /\ karmed' = TRUE
    /\ UNCHANGED <<arrived, nextId, sock, cq, stopping>> /\ UNCHANGED RingVars
    /\ UNCHANGED BankVars /\ UNCHANGED MainVars

\* rocketnet_uring_receive_queue_bank: one accepted datagram
BankOne(g, st) ==
    IF ~g.ok
      THEN [st EXCEPT !.named = @ + 1]
      ELSE LET seen == IF Diff(st.tail, st.headSeen) >= S /\ RoomRule = "refresh" THEN head ELSE st.headSeen
           IN IF Diff(st.tail, seen) >= S
                THEN [st EXCEPT !.headSeen = seen, !.full = @ + 1,
                                !.spurious = @ \/ Diff(st.tail, head) < S]
                ELSE [st EXCEPT !.headSeen = seen,
                                !.slotD = [@ EXCEPT ![SlotOf(st.tail)] = g.d],
                                !.tail = Wrap(st.tail + 1),
                                !.banked = Append(@, g.d)]

RECURSIVE BankAll(_, _)
BankAll(segs, st) == IF segs = <<>> THEN st ELSE BankAll(Tail(segs), BankOne(Head(segs), st))

\* rocketnet_uring_receive_queue_cqe: one completion, oldest first
Complete ==
    /\ cq # <<>>
    /\ LET c == Head(cq) IN
       /\ cq' = Tail(cq)
       /\ IF c.kind = "end"
            THEN /\ armed' = FALSE
                 /\ UNCHANGED RingVars /\ UNCHANGED BankVars
            ELSE LET st == BankAll(c.segs, [slotD |-> slotD, tail |-> tail, headSeen |-> headSeen,
                                            banked |-> banked, named |-> named, full |-> full,
                                            spurious |-> spurious])
                 IN /\ slotD' = st.slotD /\ tail' = st.tail /\ headSeen' = st.headSeen
                    /\ banked' = st.banked /\ named' = st.named /\ full' = st.full
                    /\ spurious' = st.spurious
                    \* the buffer goes back, after every datagram in it
                    /\ entries' = [entries EXCEPT ![Entry(brTail)] = c.bid]
                    /\ brTail' = (brTail + 1) % TailWrap
                    /\ owner' = [owner EXCEPT ![c.bid] = "ring"]
                    /\ UNCHANGED <<armed, brHead, dup>>
    /\ UNCHANGED <<arrived, nextId, sock, karmed, stopping>> /\ UNCHANGED MainVars

-----------------------------------------------------------------------------
\* main

\* rocketnet_uring_receive_queue_drain
Drain ==
    /\ ~reading
    /\ LET n == Min(Diff(tail, exposed), DrainMax)
           idx == [i \in 1..n |-> Wrap(exposed + i - 1) % S]
       IN /\ head' = IF ReleaseRule = "next" THEN exposed ELSE Wrap(exposed + n)
          /\ exposed' = Wrap(exposed + n)
          /\ vidx' = idx
          /\ view' = [i \in 1..n |-> slotD[idx[i]]]
    /\ reading' = TRUE
    /\ UNCHANGED <<delivered, torn>>
    /\ UNCHANGED <<arrived, nextId, sock, karmed, cq, armed, stopping>> /\ UNCHANGED RingVars
    /\ UNCHANGED BankVars

\* the update is done with the slots the drain took
Done ==
    /\ reading
    /\ LET now == [i \in 1..Len(vidx) |-> slotD[vidx[i]]] IN
       /\ torn' = (torn \/ now # view)
       /\ delivered' = delivered \o now
    /\ reading' = FALSE
    /\ UNCHANGED <<head, exposed, vidx, view>>
    /\ UNCHANGED <<arrived, nextId, sock, karmed, cq, armed, stopping>> /\ UNCHANGED RingVars
    /\ UNCHANGED BankVars

-----------------------------------------------------------------------------

Next ==
    \/ Arrive \/ KernelReceive \/ KernelNoBuffers \/ KernelCancel \/ Stop
    \/ Arm \/ Complete
    \/ Drain \/ Done

Spec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------

DataSegs(q) == LET RECURSIVE Sum(_)
                   Sum(r) == IF r = <<>> THEN 0
                             ELSE (IF Head(r).kind = "data" THEN Len(Head(r).segs) ELSE 0) + Sum(Tail(r))
               IN Sum(q)

SockSegs == LET RECURSIVE Sum(_)
                Sum(r) == IF r = <<>> THEN 0 ELSE Len(Head(r)) + Sum(Tail(r))
            IN Sum(sock)

TypeOK ==
    /\ arrived \in 0..MaxArrive
    /\ karmed \in BOOLEAN /\ armed \in BOOLEAN /\ stopping \in BOOLEAN
    /\ brHead \in 0..(TailWrap - 1) /\ brTail \in 0..(TailWrap - 1)
    /\ entries \in [Bufs -> Bufs]
    /\ owner \in [Bufs -> {"ring", "cq"}]
    /\ slotD \in [Slots -> Nat]
    /\ reading \in BOOLEAN

InOrder == delivered = SubSeq(banked, 1, Len(delivered))
NoTornRead == ~torn
FullOnlyWhenFull == ~spurious
NoDoubleTake == ~dup

Accounted == nextId - 1 = SockSegs + DataSegs(cq) + Len(banked) + named + full

\* the ring and the completions hold every buffer between them, once
BuffersKept == RingCount + Len(SelectSeq(cq, LAMBDA c : c.kind = "data")) = B

=============================================================================
