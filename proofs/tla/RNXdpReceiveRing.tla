------------------------- MODULE RNXdpReceiveRing -------------------------
(***************************************************************************)
(* rocketnet: the AF_XDP receive ring and the ownership of its frames.     *)
(*                                                                         *)
(* The XDP worker (rocketnet_server_socket_xdp_thread_function,            *)
(* sdk/rocketnet_server_socket_linux.cpp) peeks RX descriptors with        *)
(* xsk_ring_cons__peek, takes them while the current receive bank has      *)
(* room, releases the ones it took with xsk_ring_cons__release             *)
(* (rocketnet_xdp_receive_ring_finish), and returns their frames to the    *)
(* fill ring through its pending return (rocketnet_pending_frames_t, an    *)
(* all-or-zero flush).  The peek is bounded by the bank's remaining room,  *)
(* so every peeked descriptor is taken and the cancel branch is dead.      *)
(*                                                                         *)
(* The receive side is a three-bank handoff.  The worker appends to its    *)
(* current producer bank under the receive mutex and never publishes;      *)
(* main holds one bank as its stable view and swaps it, under the same     *)
(* mutex, for the worker's filled bank once it has consumed every packet   *)
(* of the bank it holds.  The first swap takes a spare, and a retained     *)
(* read cursor carries a partly consumed bank into the next receive call.  *)
(* A receive call also has a packet budget: when the budget ends inside a  *)
(* bank, main keeps that bank and its read cursor for the next call, so    *)
(* every packet staged before a claim is in that claim's view.  Drain is   *)
(* that claim; Consume is main reading one packet from the bank it holds;  *)
(* NewCall ends a receive call and starts the next with a fresh budget,    *)
(* carrying the cursor.                                                    *)
(*                                                                         *)
(* Only main swaps, once per receive call and before it reads, so the      *)
(* call's view (the packets it returned, which point into the bank) stays  *)
(* main's until the call ends.  Main never waits for the mutex beyond a    *)
(* bounded spin.  It tries once (Drain when the mutex is free).  When the   *)
(* worker holds the mutex it raises a flag (DrainWant, receive_wanted) and *)
(* tries again at most SpinMax times (DrainSpin for a try that misses,     *)
(* Drain for one that takes the mutex).  The worker reads the flag between *)
(* two descriptors (WorkerCheck when it is clear): when it is set it lets  *)
(* go of the mutex (WorkerYield) and does not take it again (WorkerLock)   *)
(* until main clears the flag, which main does when its swap is done or    *)
(* its tries have run out.  A claim whose every try missed is DrainMiss:   *)
(* main takes nothing from the queue in that call, keeps its bank          *)
(* (emptied, the view having expired) and its cursor, and claims again on  *)
(* its next call, whose claim views every packet staged before it.  Only   *)
(* main writes the flag, its held bank and its cursor.                     *)
(*                                                                         *)
(* What bounds a miss.  With Stall FALSE the model assumes neither thread  *)
(* stops: the worker takes a step (one descriptor, or letting go) between  *)
(* two of main's tries, and at most TryArrivals packets arrive between     *)
(* two of them.  A try of the model stands for as many of the code's tries *)
(* as one worker step lasts.  Under that assumption ClaimWithinSpin holds  *)
(* with SpinMax = 2 (the descriptor in hand, then the letting go): main    *)
(* swaps in the call that asked, no call misses, and the liveness          *)
(* properties hold with weak fairness alone (SpecWeak).  The worker is     *)
(* kept off the mutex only while main spins (WantedOnlyInSpin), and no     *)
(* arrival is dropped for a full RX ring in that time if the ring had      *)
(* room for a spin's arrivals when main raised the flag                    *)
(* (NoDropWhilePaused).  With Stall TRUE a thread may stop for any length  *)
(* of time: a worker stopped while it holds the mutex makes the claim      *)
(* miss, the safety properties still hold, and liveness then rests on      *)
(* SF_vars(Drain) and finite arrivals, as it did before the flag.          *)
(*                                                                         *)
(* The worker holds the mutex only to append, for the staging loop of a    *)
(* pass (WorkerLock, Take, WorkerCheck, WorkerYield, WorkerUnlock), and    *)
(* reads the producer bank again under every hold, because main's swap     *)
(* between two holds changes it.  It bounds its peek by its own count of   *)
(* the bank (seenCount, the bank's packets when it last let go of the      *)
(* mutex at the end of a pass), without the mutex: main's swap only ever   *)
(* hands the worker an emptied bank and counts in receive_swaps, so a new  *)
(* count (emptied) means the bank is empty and the room is never           *)
(* overstated.  The counter is 32 bits in the code; the model keeps only   *)
(* "changed since the worker looked", which differs after 2^32 swaps with  *)
(* no worker pass between.                                                 *)
(*                                                                         *)
(* The spare banks are a ring of indices (rocketnet_xdp_buffer_exchange_t: *)
(* free_index, free_head, free_count), modelled as laid out: swap_producer *)
(* takes free_index[free_head] on the first claim and never puts a bank    *)
(* back, so the third bank stays spare.                                    *)
(*                                                                         *)
(* The fill ring is the kernel's too: fq is its entries from the consumer  *)
(* index the kernel has published to the producer index.  The kernel       *)
(* reads it through a cached batch (xskq_cons_peek_addr_unchecked,         *)
(* net/xdp/xsk_queue.h): kcache is the batch it peeked, kused the entries  *)
(* of the batch it has taken, and it publishes its consumer only when the  *)
(* batch is used up.  A taken entry keeps its slot until then, so the      *)
(* user's free slots are FillSize - Len(fq) (libxdp's                      *)
(* xsk_ring_prod__reserve), and the flush fits only in those.  The server  *)
(* sizes the ring at twice its receive frames (ROCKETNET_XDP_FILL_RING_SIZE*)
(* in rocketnet_constants.h); with exactly the frames, the worker cannot   *)
(* hand a frame back until the kernel ends its batch, and the kernel then  *)
(* finds the ring empty and drops the packet (hulk 2026-10-22, "rx fill    *)
(* ring empty" on the one queue all of one host's clients hash to).        *)
(*                                                                         *)
(* The ring is the kernel's view: ring is the sequence of descriptors      *)
(* from the consumer index to the producer index.  ahead is the user's     *)
(* cached consumer minus the consumer: the descriptors peeked and neither  *)
(* released nor cancelled.  libxdp's peek starts at the cached consumer    *)
(* and advances it; cancel moves it back; release advances the consumer.   *)
(* The release hands the kernel the first n descriptors of the ring,       *)
(* whichever frames they hold, so a release while ahead > 0 at the start   *)
(* of a pass gives back descriptors that were never read.  A packet that   *)
(* arrives while the ring is full is dropped by the kernel (KernelDrop).   *)
(*                                                                         *)
(* A driver or kernel defect can post a descriptor naming a frame outside  *)
(* the receive region (a send frame, SendFrame here): KernelMisdeliver,    *)
(* at most once per behaviour.  The worker reads it and refuses it         *)
(* (rocketnet_xdp_receive_descriptor), and hands a frame back to the fill  *)
(* ring only when rocketnet_xdp_fill_return (sdk/rocketnet_xdp_frame.h)    *)
(* places it in the receive region, so a send frame is never received      *)
(* into while it is also on the send free list.                            *)
(*                                                                         *)
(* Left out: the pending return's capacity (the peek is bounded by it and  *)
(* it is ROCKETNET_XDP_RECV_QUEUE_SIZE, no fewer than the frames, so it    *)
(* never binds), packet contents and the frame codec's drops (a dropped    *)
(* frame is still taken and returned), the other queues of the same        *)
(* receive call (the budget here is this queue's share of the call), and   *)
(* main's own hold of the mutex, which is the swap alone and is one step   *)
(* with the clearing of the flag that follows its release.                 *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS
    Frames,       \* the UMEM receive frames
    RingSize,     \* the RX ring's entries
    PeekMax,      \* the most descriptors one peek asks for
    StageCap,     \* packets one receive bank holds (ROCKETNET_XDP_RECV_QUEUE_SIZE)
    FillSize,     \* the fill ring's entries (ROCKETNET_XDP_FILL_RING_SIZE)
    Budget,       \* packets one receive call may consume
    MaxPackets,   \* packets that arrive in one behaviour
    SpinMax,      \* main's further tries for the mutex after its first misses
    TryArrivals,  \* the most packets that arrive between two of main's tries
    Stall         \* TRUE: a thread may stop for any length of time

ASSUME /\ Frames # {}
       /\ RingSize \in Nat \ {0}
       /\ PeekMax \in Nat \ {0}
       /\ StageCap \in Nat \ {0}
       /\ FillSize \in Nat /\ FillSize >= Cardinality(Frames)
       /\ Budget \in Nat \ {0}
       /\ MaxPackets \in Nat \ {0}
       /\ SpinMax \in Nat
       /\ TryArrivals \in Nat \ {0}
       /\ Stall \in BOOLEAN

(***************************************************************************)
(* Rule switches, each overridden by one reversed witness and FALSE in the *)
(* design.                                                                 *)
(*   RefusedRefilled: a descriptor naming a frame outside the receive      *)
(*     region is handed back to the fill ring like any dropped frame (the  *)
(*     worker before rocketnet_xdp_fill_return).                           *)
(*   DrainStale: a claim leaves the most recently staged packet behind in  *)
(*     the worker's next bank, as the worker-driven publish deferred a     *)
(*     packet staged after it to a later claim, so a packet staged before  *)
(*     the claim was not in that claim's view.                             *)
(*   SwapInCall: main swaps again in a call that has already read from     *)
(*     the bank it holds, so the worker is handed a bank the call's view   *)
(*     still points into.                                                  *)
(*   DoubleSwap: a claim is followed by a second swap in the same call,    *)
(*     which hands the bank just claimed back to the worker unread.        *)
(*   SwapUnread: a call swaps although the bank main holds has a retained  *)
(*     tail it has not read.                                               *)
(*   KeepCursor: a swap leaves the read cursor where the last bank ended   *)
(*     (no receive_read_index = 0), so the next bank is read from there.   *)
(*   MissNoRetry: after a missed claim main skips the queue on every later *)
(*     call, as it did for a queue whose swap request stayed pending.      *)
(*   MissDropsBank: a missed claim gives up the bank main holds.           *)
(*   StaleBank: after it let go for main the worker stages into the bank   *)
(*     it read at the start of its pass instead of reading the producer    *)
(*     again.                                                              *)
(*   StaleRoom: the worker ignores the swap count and keeps its old count  *)
(*     of the bank.                                                        *)
(*   SpareKept: the first claim takes free_index[free_head] without        *)
(*     advancing free_head or lowering free_count.                         *)
(*   NoYield: the worker never reads main's flag: it holds the mutex for   *)
(*     the whole staging loop and takes it whenever it is free, as it did  *)
(*     before the flag (its release and re-acquire at a batch boundary     *)
(*     were back to back, and main's try never queued).                    *)
(*   GrabWhileWanted: the worker lets go for main but takes the mutex      *)
(*     again without waiting for main to clear the flag.                   *)
(*   StickyWant: main leaves the flag raised when its tries run out.       *)
(*   LateClear: main leaves the flag raised after its swap, until the      *)
(*     call ends.                                                          *)
(***************************************************************************)
RefusedRefilled == FALSE
DrainStale == FALSE
SwapInCall == FALSE
DoubleSwap == FALSE
SwapUnread == FALSE
KeepCursor == FALSE
MissNoRetry == FALSE
MissDropsBank == FALSE
StaleBank == FALSE
StaleRoom == FALSE
SpareKept == FALSE
NoYield == FALSE
GrabWhileWanted == FALSE
StickyWant == FALSE
LateClear == FALSE

(* The frame a defective descriptor names: one the model does not own for  *)
(* receive (a send-region frame).  An untyped model value Frames holds is  *)
(* never equal to it.                                                      *)
SendFrame == "send frame"

AllFrames == Frames \cup {SendFrame}

(* ROCKETNET_XDP_NUM_BUFFERS *)
NumBanks == 3

Banks == 0 .. NumBanks - 1

NONE == NumBanks

VARIABLES
    fq,           \* the fill ring's entries, published consumer to producer
    kcache,       \* the entries at the front of fq the kernel's batch peeked
    kused,        \* the entries of that batch the kernel has taken
    ring,         \* the RX ring's descriptors, consumer to producer (kernel view)
    ahead,        \* cached consumer - consumer (user)
    phase,        \* "idle" or "taking" in the fill pass
    start,        \* the ring offset the current peek began at
    peeked,       \* the descriptors the current peek returned
    taken,        \* the descriptors the current pass took
    pending,      \* the frames the worker holds: taken, awaiting the fill ring
    mutex,        \* the receive mutex: "free" or "worker" (main's hold is one step)
    checked,      \* under its hold, the worker may take the next descriptor
    stageBank,    \* the bank the worker's hold stages into, or NONE (receive_buffer)
    seenCount,    \* the worker's count of its bank's packets (receive_staged)
    emptied,      \* main swapped since the worker last looked (receive_swaps moved)
    workerBank,   \* the receive bank the worker stages into (the producer)
    mainBank,     \* the receive bank main holds, or NONE (receive_held_index)
    freeIndex,    \* the spare banks' ring of indices (free_index)
    freeHead,     \* the ring's oldest entry (free_head)
    freeCount,    \* the ring's entries (free_count)
    bankContent,  \* the packet ids staged in each receive bank
    readCursor,   \* packets of mainBank main has consumed (receive_read_index)
    budget,       \* packets this receive call may still consume
    mainPhase,    \* "claim": a call's first try; "spin": its further tries; "read"
    viewBank,     \* ghost: the bank this call's returned packets point into, or NONE
    heldView,     \* ghost: mainBank's packets as main last claimed or emptied it
    wanted,       \* main is spinning for the mutex (receive_wanted)
    spins,        \* main's further tries that have missed in this claim
    missRun,      \* ghost: calls in a row whose claim missed (saturates at 2)
    owed,         \* ghost: main tried since the worker's last step under its hold
    spinArrivals, \* ghost: packets arrived since main's last try of a spin
    freeAtWant,   \* ghost: the RX ring's free entries when main raised the flag
    badDrop,      \* ghost: a packet was dropped while the worker was kept off the
                  \* mutex, though the ring had room for a spin's arrivals
    dropped,      \* packets the kernel dropped for a full RX ring
    nextPacket,   \* the next packet staged
    claimMark,    \* ghost: nextPacket at main's last claim
    consumed,     \* ghost: the packet ids main consumed, in order
    took,         \* ghost: the current pass has taken a descriptor
    defect        \* the kernel has posted a descriptor naming SendFrame

fillVars == <<fq, kcache, kused>>
passVars == <<ahead, phase, start, peeked, taken>>
lockVars == <<mutex, checked, stageBank>>
roomVars == <<seenCount, emptied>>
freeVars == <<freeIndex, freeHead, freeCount>>
mainVars == <<mainBank, readCursor, budget, mainPhase, viewBank, heldView,
              claimMark, consumed>>
claimVars == <<wanted, spins, missRun>>
waitVars == <<spinArrivals, freeAtWant, badDrop>>

vars == <<fillVars, ring, passVars, pending, lockVars, roomVars, workerBank,
          freeVars, bankContent, mainVars, claimVars, owed, waitVars, dropped,
          nextPacket, took, defect>>

Range(s) == {s[i] : i \in DOMAIN s}

Min(a, b) == IF a <= b THEN a ELSE b

(* The frames of a set in some order: the order the flush writes them.     *)
SetSeq(S) == CHOOSE s \in [1..Cardinality(S) -> S] : Range(s) = S

(* The fill ring's frames the kernel has not taken.                        *)
Fill == {fq[i] : i \in kused + 1..Len(fq)}

(* The flush fits: libxdp's reservation of every pending frame or none.    *)
FlushFits == Len(fq) + Cardinality(pending) <= FillSize

(* The RX ring's descriptors the current pass has not taken: a taken       *)
(* descriptor stays in the kernel's ring until the release, and its frame  *)
(* is already the worker's.                                                *)
(* A descriptor naming SendFrame is the defect's, not a receive frame's.   *)
Untaken == {i \in 1..Len(ring) : i \notin start + 1..start + taken /\ ring[i] \in Frames}
InRing == {ring[i] : i \in Untaken}

(* The packets that have arrived: staged, posted and not yet taken, or     *)
(* dropped.  MaxPackets bounds them.                                       *)
Arrivals == nextPacket + Cardinality(Untaken) + dropped

(* Between two of main's tries at most TryArrivals packets arrive, unless  *)
(* a thread may stop (Stall).                                              *)
ArrivalInTime == Stall \/ mainPhase # "spin" \/ spinArrivals < TryArrivals
CountArrival == IF ~Stall /\ mainPhase = "spin" THEN spinArrivals + 1 ELSE spinArrivals

(* The live entries of the spare ring, and how many of them name bank b.   *)
FreeSlots == {(freeHead + i) % NumBanks : i \in 0..freeCount - 1}
FreeOwns(b) == Cardinality({s \in FreeSlots : freeIndex[s] = b})

(* The packets of the bank main holds, and whether main has read them all. *)
Held == IF mainBank = NONE THEN <<>> ELSE bankContent[mainBank]
Exhausted == readCursor >= Len(Held)

(* The worker has descriptors to stage and is kept off the mutex by main's *)
(* flag.                                                                   *)
Paused == phase = "taking" /\ taken < peeked /\ mutex = "free" /\ wanted

TypeOK ==
    /\ fq \in Seq(AllFrames) /\ Len(fq) <= FillSize
    /\ kcache \in 0..Len(fq)
    /\ kused \in 0..kcache
    /\ ring \in Seq(AllFrames) /\ Len(ring) <= RingSize
    /\ ahead \in 0..RingSize
    /\ phase \in {"idle", "taking"}
    /\ start \in 0..RingSize
    /\ peeked \in 0..PeekMax
    /\ taken \in 0..PeekMax
    /\ pending \subseteq AllFrames
    /\ mutex \in {"free", "worker"}
    /\ checked \in BOOLEAN
    /\ stageBank \in Banks \cup {NONE}
    /\ seenCount \in 0..StageCap
    /\ emptied \in BOOLEAN
    /\ workerBank \in Banks
    /\ mainBank \in Banks \cup {NONE}
    /\ freeIndex \in [0..NumBanks - 1 -> Banks]
    /\ freeHead \in 0..NumBanks - 1
    /\ freeCount \in 0..NumBanks
    /\ bankContent \in [Banks -> Seq(1..MaxPackets)]
    /\ \A b \in Banks : Len(bankContent[b]) <= StageCap
    /\ readCursor \in 0..StageCap
    /\ budget \in 0..Budget
    /\ mainPhase \in {"claim", "spin", "read", "skipped"}
    /\ viewBank \in Banks \cup {NONE}
    /\ heldView \in Seq(1..MaxPackets)
    /\ wanted \in BOOLEAN
    /\ spins \in 0..SpinMax
    /\ missRun \in 0..2
    /\ owed \in BOOLEAN
    /\ spinArrivals \in 0..TryArrivals
    /\ freeAtWant \in 0..RingSize
    /\ badDrop \in BOOLEAN
    /\ dropped \in 0..MaxPackets
    /\ nextPacket \in 0..MaxPackets
    /\ claimMark \in 0..MaxPackets
    /\ consumed \in Seq(1..MaxPackets)
    /\ took \in BOOLEAN
    /\ defect \in BOOLEAN

(* The exchange's constructor: the producer is bank 0 and the spare ring   *)
(* holds banks 1 .. NumBanks - 1 from its head (free_index[i - 1] = i).    *)
Init ==
    /\ fq = SetSeq(Frames)
    /\ kcache = 0
    /\ kused = 0
    /\ ring = <<>>
    /\ ahead = 0
    /\ phase = "idle"
    /\ start = 0
    /\ peeked = 0
    /\ taken = 0
    /\ pending = {}
    /\ mutex = "free"
    /\ checked = FALSE
    /\ stageBank = NONE
    /\ seenCount = 0
    /\ emptied = FALSE
    /\ workerBank = 0
    /\ mainBank = NONE
    /\ freeIndex = [i \in 0..NumBanks - 1 |-> IF i < NumBanks - 1 THEN i + 1 ELSE 0]
    /\ freeHead = 0
    /\ freeCount = NumBanks - 1
    /\ bankContent = [b \in Banks |-> <<>>]
    /\ readCursor = 0
    /\ budget = Budget
    /\ mainPhase = "claim"
    /\ viewBank = NONE
    /\ heldView = <<>>
    /\ wanted = FALSE
    /\ spins = 0
    /\ missRun = 0
    /\ owed = FALSE
    /\ spinArrivals = 0
    /\ freeAtWant = 0
    /\ badDrop = FALSE
    /\ dropped = 0
    /\ nextPacket = 0
    /\ claimMark = 0
    /\ consumed = <<>>
    /\ took = FALSE
    /\ defect = FALSE

(* KernelReceive: a packet lands in the next frame of the kernel's batch,  *)
(* posted to the RX ring while the ring has a free entry as the kernel     *)
(* sees it.  The fill ring slot stays the kernel's until it publishes.     *)
KernelReceive ==
    /\ kused < kcache
    /\ Len(ring) < RingSize
    /\ Arrivals < MaxPackets
    /\ ArrivalInTime
    /\ kused' = kused + 1
    /\ ring' = Append(ring, fq[kused + 1])
    /\ spinArrivals' = CountArrival
    /\ UNCHANGED <<fq, kcache, passVars, pending, lockVars, roomVars,
                    workerBank, freeVars, bankContent, mainVars, claimVars,
                    owed, freeAtWant, badDrop, dropped, nextPacket, took,
                    defect>>

(* KernelDrop: a packet arrives while the RX ring is full, and the kernel  *)
(* drops it.  badDrop records one dropped while the worker was kept off    *)
(* the mutex for main although the ring had room, when main raised the     *)
(* flag, for every packet a whole spin lets arrive.                        *)
KernelDrop ==
    /\ Len(ring) = RingSize
    /\ Arrivals < MaxPackets
    /\ ArrivalInTime
    /\ dropped' = dropped + 1
    /\ spinArrivals' = CountArrival
    /\ badDrop' = (badDrop \/ (Paused /\ freeAtWant >= SpinMax * TryArrivals))
    /\ UNCHANGED <<fillVars, ring, passVars, pending, lockVars, roomVars,
                    workerBank, freeVars, bankContent, mainVars, claimVars,
                    owed, freeAtWant, nextPacket, took, defect>>

(* KernelMisdeliver: a driver or kernel defect posts a descriptor naming a *)
(* frame outside the receive region, once.  It takes a ring entry like any *)
(* arrival, so it counts with the arrivals between two of main's tries.    *)
KernelMisdeliver ==
    /\ ~defect
    /\ Len(ring) < RingSize
    /\ ArrivalInTime
    /\ ring' = Append(ring, SendFrame)
    /\ defect' = TRUE
    /\ spinArrivals' = CountArrival
    /\ UNCHANGED <<fillVars, passVars, pending, lockVars, roomVars,
                    workerBank, freeVars, bankContent, mainVars, claimVars,
                    owed, freeAtWant, badDrop, dropped, nextPacket, took>>

(* KernelRefresh: a packet finds the batch used up.  The kernel publishes  *)
(* its consumer (the taken entries' slots go back to the user) and peeks   *)
(* what the producer shows now.  If that is nothing, the packet is         *)
(* dropped: "rx fill ring empty".                                          *)
KernelRefresh ==
    /\ kused = kcache
    /\ kused > 0 \/ Len(fq) > kcache
    /\ fq' = SubSeq(fq, kused + 1, Len(fq))
    /\ kused' = 0
    /\ kcache' = Len(fq) - kused
    /\ UNCHANGED <<ring, passVars, pending, lockVars, roomVars, workerBank,
                    freeVars, bankContent, mainVars, claimVars, owed, waitVars,
                    dropped, nextPacket, took, defect>>

(* Peek: xsk_ring_cons__peek from the cached consumer, at most PeekMax and *)
(* at most the current bank's remaining room; begin the modeled fill pass. *)
(* The room is counted without the mutex, from the worker's own count of   *)
(* the bank: zero once it sees that main swapped (a new receive_swaps),    *)
(* else the count it left the bank with.  The bound is why the pass takes  *)
(* every descriptor it peeks and the finish's cancel branch is dead.  A    *)
(* peek of zero is no step.  StaleRoom ignores the swap count.             *)
Peek ==
    LET believed == IF emptied /\ ~StaleRoom THEN 0 ELSE seenCount
        n == Min(Len(ring) - ahead, Min(PeekMax, StageCap - believed)) IN
    /\ phase = "idle"
    /\ n > 0
    /\ start' = ahead
    /\ ahead' = ahead + n
    /\ peeked' = n
    /\ taken' = 0
    /\ phase' = "taking"
    /\ seenCount' = believed
    /\ emptied' = FALSE
    /\ UNCHANGED <<fillVars, ring, pending, lockVars, workerBank, freeVars,
                    bankContent, mainVars, claimVars, owed, waitVars, dropped,
                    nextPacket, took, defect>>

(* WorkerLock: the worker acquires the receive mutex to append, at the     *)
(* start of its staging loop and again after it let go for main, and reads *)
(* the producer bank under it.  It stays off the mutex while main's flag   *)
(* is raised; otherwise it waits for the mutex, and main's hold is the     *)
(* swap alone.  The first descriptor of a hold needs no look at the flag.  *)
(* StaleBank keeps the bank read before the worker let go.                 *)
WorkerLock ==
    /\ phase = "taking"
    /\ taken < peeked
    /\ mutex = "free"
    /\ ~wanted \/ GrabWhileWanted \/ NoYield
    /\ mutex' = "worker"
    /\ checked' = TRUE
    /\ stageBank' = IF stageBank = NONE THEN workerBank ELSE stageBank
    /\ owed' = FALSE
    /\ UNCHANGED <<fillVars, ring, passVars, pending, roomVars, workerBank,
                    freeVars, bankContent, mainVars, claimVars, waitVars,
                    dropped, nextPacket, took, defect>>

(* WorkerCheck: between two descriptors, under its hold, the worker reads  *)
(* main's flag and finds it clear.  Main may raise it straight after, so   *)
(* one more descriptor can be taken under a raised flag.                   *)
WorkerCheck ==
    /\ mutex = "worker"
    /\ taken < peeked
    /\ ~checked
    /\ ~wanted \/ NoYield
    /\ checked' = TRUE
    /\ owed' = FALSE
    /\ UNCHANGED <<fillVars, ring, passVars, pending, mutex, stageBank,
                    roomVars, workerBank, freeVars, bankContent, mainVars,
                    claimVars, waitVars, dropped, nextPacket, took, defect>>

(* Take: under the mutex, read descriptor start + taken + 1 (rx_desc at    *)
(* the peek's index + i) and hold its frame.  A receive-region descriptor  *)
(* is also staged as the next packet in the bank this hold read.  A        *)
(* descriptor outside the receive region is refused: no packet is staged   *)
(* and its frame is not held for the fill ring                             *)
(* (rocketnet_xdp_fill_return false), unless RefusedRefilled.              *)
Take ==
    LET d == ring[start + taken + 1] IN
    /\ phase = "taking"
    /\ mutex = "worker"
    /\ taken < peeked
    /\ checked
    /\ d \in Frames => (Len(bankContent[stageBank]) < StageCap /\ nextPacket < MaxPackets)
    /\ pending' = IF d \in Frames \/ RefusedRefilled THEN pending \cup {d} ELSE pending
    /\ taken' = taken + 1
    /\ checked' = FALSE
    /\ bankContent' = IF d \in Frames
                         THEN [bankContent EXCEPT ![stageBank] = Append(@, nextPacket + 1)]
                         ELSE bankContent
    /\ nextPacket' = IF d \in Frames THEN nextPacket + 1 ELSE nextPacket
    /\ took' = TRUE
    /\ owed' = FALSE
    /\ UNCHANGED <<fillVars, ring, ahead, phase, start, peeked, mutex,
                    stageBank, roomVars, workerBank, freeVars, mainVars,
                    claimVars, waitVars, dropped, defect>>

(* WorkerYield: between two descriptors, under its hold, the worker reads  *)
(* main's flag and finds it raised.  It releases the mutex so that main's  *)
(* next try takes it, and takes it again (WorkerLock) only once the flag   *)
(* is clear.                                                               *)
WorkerYield ==
    /\ mutex = "worker"
    /\ taken < peeked
    /\ ~checked
    /\ wanted /\ ~NoYield
    /\ mutex' = "free"
    /\ stageBank' = IF StaleBank THEN stageBank ELSE NONE
    /\ owed' = FALSE
    /\ UNCHANGED <<fillVars, ring, passVars, pending, checked, roomVars,
                    workerBank, freeVars, bankContent, mainVars, claimVars,
                    waitVars, dropped, nextPacket, took, defect>>

(* WorkerUnlock: every peeked descriptor is taken.  Still under the mutex  *)
(* the worker notes the bank's count and the swap count it has seen        *)
(* (receive_staged, receive_swaps_seen), then releases.                    *)
WorkerUnlock ==
    /\ mutex = "worker"
    /\ taken = peeked
    /\ mutex' = "free"
    /\ checked' = FALSE
    /\ seenCount' = Len(bankContent[stageBank])
    /\ emptied' = FALSE
    /\ stageBank' = NONE
    /\ owed' = FALSE
    /\ UNCHANGED <<fillVars, ring, passVars, pending, workerBank, freeVars,
                    bankContent, mainVars, claimVars, waitVars, dropped,
                    nextPacket, took, defect>>

(* Finish: every peeked descriptor was taken (the peek was bounded by the  *)
(* bank's room) and the mutex is released.  End the fill pass and release  *)
(* the taken count: the consumer advances over the first taken             *)
(* descriptors.  Then the pass flushes its pending return into the fill    *)
(* ring, all of it if it fits, else none (a zero reservation keeps every   *)
(* frame).                                                                 *)
Finish ==
    /\ phase = "taking"
    /\ taken = peeked
    /\ mutex = "free"
    /\ ring' = SubSeq(ring, taken + 1, Len(ring))
    /\ ahead' = ahead - taken
    /\ phase' = "idle"
    /\ start' = 0
    /\ peeked' = 0
    /\ taken' = 0
    /\ took' = FALSE
    /\ IF FlushFits
          THEN /\ fq' = fq \o SetSeq(pending)
               /\ pending' = {}
          ELSE UNCHANGED <<fq, pending>>
    /\ UNCHANGED <<kcache, kused, lockVars, roomVars, workerBank, freeVars,
                    bankContent, mainVars, claimVars, owed, waitVars, dropped,
                    nextPacket, defect>>

(* Refill: the flush again at the top of the worker's next pass; a         *)
(* reservation of zero is this step not yet taken.                         *)
Refill ==
    /\ phase = "idle"
    /\ pending # {}
    /\ FlushFits
    /\ fq' = fq \o SetSeq(pending)
    /\ pending' = {}
    /\ UNCHANGED <<kcache, kused, ring, passVars, lockVars, roomVars,
                    workerBank, freeVars, bankContent, mainVars, claimVars,
                    owed, waitVars, dropped, nextPacket, took, defect>>

(* Drain: a try of main's finds the mutex free, its first (mainPhase       *)
(* "claim") or one of its further tries ("spin"), and main claims the      *)
(* worker's filled bank, exchanging the exhausted bank main holds (or a    *)
(* spare on the first claim: swap_producer takes free_index[free_head])    *)
(* under the receive mutex.  A claim happens at the start of a receive     *)
(* call, only once main has consumed every packet of the bank it holds,    *)
(* and it resets the read cursor.  Every packet staged before the claim is *)
(* in the claimed bank's view; DrainStale instead leaves the most recently *)
(* staged packet in the worker's new bank, as the pre-fix worker publish   *)
(* did.  The worker learns of the swap from the swap count (emptied).      *)
(* Main releases the mutex and then clears its flag; LateClear leaves the  *)
(* flag raised until the call ends.                                        *)
Drain ==
    LET first == mainBank = NONE
        c == bankContent[workerBank]
        stale == DrainStale /\ c # <<>>
        view == IF stale THEN SubSeq(c, 1, Len(c) - 1) ELSE c
        left == IF stale THEN <<c[Len(c)]>> ELSE <<>>
        takes == first /\ ~SpareKept
        keeps == LateClear /\ wanted IN
    /\ \/ mainPhase = "claim"
       \/ mainPhase = "spin" /\ spins < SpinMax
       \/ SwapInCall /\ mainPhase = "read" /\ viewBank # NONE /\ Exhausted
    /\ mutex = "free"
    /\ first => freeCount > 0
    /\ workerBank' = IF first THEN freeIndex[freeHead] ELSE mainBank
    /\ freeHead' = IF takes THEN (freeHead + 1) % NumBanks ELSE freeHead
    /\ freeCount' = IF takes THEN freeCount - 1 ELSE freeCount
    /\ mainBank' = workerBank
    /\ bankContent' = IF first
                         THEN [bankContent EXCEPT ![workerBank] = view]
                         ELSE [bankContent EXCEPT ![workerBank] = view, ![mainBank] = left]
    /\ readCursor' = IF KeepCursor THEN readCursor ELSE 0
    /\ heldView' = view
    /\ emptied' = TRUE
    /\ mainPhase' = IF DoubleSwap THEN "claim" ELSE "read"
    /\ claimMark' = nextPacket
    /\ wanted' = keeps
    /\ spins' = 0
    /\ missRun' = 0
    /\ owed' = FALSE
    /\ spinArrivals' = 0
    /\ freeAtWant' = IF keeps THEN freeAtWant ELSE 0
    /\ UNCHANGED <<fillVars, ring, passVars, pending, lockVars, seenCount,
                    freeIndex, budget, viewBank, consumed, badDrop, dropped,
                    nextPacket, took, defect>>

(* DrainWant: main's first try finds the worker holding the mutex.  Main   *)
(* has already emptied the bank it holds (the view of the last call has    *)
(* expired).  It raises its flag and goes on to its further tries.         *)
DrainWant ==
    /\ mainPhase = "claim"
    /\ mutex = "worker"
    /\ bankContent' = IF mainBank = NONE
                         THEN bankContent
                         ELSE [bankContent EXCEPT ![mainBank] = <<>>]
    /\ heldView' = <<>>
    /\ wanted' = TRUE
    /\ spins' = 0
    /\ mainPhase' = "spin"
    /\ owed' = ~Stall
    /\ spinArrivals' = 0
    /\ freeAtWant' = RingSize - Len(ring)
    /\ UNCHANGED <<fillVars, ring, passVars, pending, lockVars, roomVars,
                    workerBank, freeVars, mainBank, readCursor, budget,
                    viewBank, claimMark, consumed, missRun, badDrop, dropped,
                    nextPacket, took, defect>>

(* DrainSpin: a further try of main's still finds the worker holding the   *)
(* mutex.  Unless a thread may stop (Stall), the worker has taken a step   *)
(* since main's last try.                                                  *)
DrainSpin ==
    /\ mainPhase = "spin"
    /\ spins < SpinMax
    /\ mutex = "worker"
    /\ Stall \/ ~owed
    /\ spins' = spins + 1
    /\ owed' = ~Stall
    /\ spinArrivals' = 0
    /\ UNCHANGED <<fillVars, ring, passVars, pending, lockVars, roomVars,
                    workerBank, freeVars, bankContent, mainVars, wanted,
                    missRun, freeAtWant, badDrop, dropped, nextPacket, took,
                    defect>>

(* DrainMiss: main's tries have run out.  Main does not wait any longer:   *)
(* it clears its flag, keeps the bank it holds and its cursor, and takes   *)
(* nothing from the queue in this call, so the next call finds the bank    *)
(* exhausted again and claims again.  MissNoRetry instead skips the queue  *)
(* from then on; MissDropsBank gives the bank up; StickyWant leaves the    *)
(* flag raised.                                                            *)
DrainMiss ==
    /\ mainPhase = "spin"
    /\ spins = SpinMax
    /\ wanted' = StickyWant
    /\ spins' = 0
    /\ missRun' = Min(missRun + 1, 2)
    /\ mainBank' = IF MissDropsBank THEN NONE ELSE mainBank
    /\ mainPhase' = IF MissNoRetry THEN "skipped" ELSE "read"
    /\ owed' = FALSE
    /\ spinArrivals' = 0
    /\ freeAtWant' = IF StickyWant THEN freeAtWant ELSE 0
    /\ UNCHANGED <<fillVars, ring, passVars, pending, lockVars, roomVars,
                    workerBank, freeVars, bankContent, readCursor, budget,
                    viewBank, heldView, claimMark, consumed, badDrop, dropped,
                    nextPacket, took, defect>>

(* Consume: main reads one packet from the bank it holds, advancing its    *)
(* read cursor and spending one packet of the call's budget.  The packet   *)
(* it returns points into the bank until the call ends (viewBank).  A      *)
(* budget that ends here leaves the cursor for the next call: the retained *)
(* tail is not claimed again until it is consumed.                         *)
Consume ==
    /\ mainPhase = "read"
    /\ ~Exhausted
    /\ budget > 0
    /\ consumed' = Append(consumed, Held[readCursor + 1])
    /\ readCursor' = readCursor + 1
    /\ budget' = budget - 1
    /\ viewBank' = mainBank
    /\ UNCHANGED <<fillVars, ring, passVars, pending, lockVars, roomVars,
                    workerBank, freeVars, bankContent, mainBank, mainPhase,
                    heldView, claimMark, claimVars, owed, waitVars, dropped,
                    nextPacket, took, defect>>

(* NewCall: a receive call ends, at its budget or at the end of the bank   *)
(* main holds, and the next begins with a fresh budget.  The last call's   *)
(* view expires.  A partly consumed bank and its read cursor carry over    *)
(* and the new call reads on; an exhausted bank, or none, sends the new    *)
(* call to its claim.  SwapUnread sends it there with a tail unread.       *)
(* LateClear clears main's flag only here.                                 *)
NewCall ==
    /\ mainPhase = "read"
    /\ budget = 0 \/ Exhausted
    /\ budget' = Budget
    /\ viewBank' = NONE
    /\ mainPhase' = IF Exhausted \/ SwapUnread THEN "claim" ELSE "read"
    /\ wanted' = IF LateClear THEN FALSE ELSE wanted
    /\ freeAtWant' = IF LateClear THEN 0 ELSE freeAtWant
    /\ UNCHANGED <<fillVars, ring, passVars, pending, lockVars, roomVars,
                    workerBank, freeVars, bankContent, mainBank, readCursor,
                    heldView, claimMark, consumed, spins, missRun, owed,
                    spinArrivals, badDrop, dropped, nextPacket, took, defect>>

MainStep ==
    \/ Drain
    \/ DrainWant
    \/ DrainSpin
    \/ DrainMiss
    \/ Consume
    \/ NewCall

WorkerStep ==
    \/ Peek
    \/ WorkerLock
    \/ WorkerCheck
    \/ Take
    \/ WorkerYield
    \/ WorkerUnlock
    \/ Finish
    \/ Refill

Next ==
    \/ KernelReceive
    \/ KernelDrop
    \/ KernelRefresh
    \/ KernelMisdeliver
    \/ MainStep
    \/ WorkerStep

(* A thread that may stop for any length of time (Stall TRUE): the worker  *)
(* takes whenever the ring has descriptors, reads main's flag between two  *)
(* descriptors whichever way it stands, and never keeps the mutex; main    *)
(* reads, starts calls and gives up a spin; and a try succeeds in the end  *)
(* (strong fairness on Drain: it is enabled only while the mutex is free). *)
Spec == Init /\ [][Next]_vars
        /\ WF_vars(KernelReceive) /\ WF_vars(KernelRefresh)
        /\ WF_vars(Peek) /\ WF_vars(WorkerLock)
        /\ WF_vars(WorkerCheck \/ WorkerYield)
        /\ WF_vars(Take) /\ WF_vars(WorkerUnlock)
        /\ WF_vars(Finish) /\ WF_vars(Refill)
        /\ WF_vars(Consume) /\ WF_vars(NewCall) /\ WF_vars(DrainMiss)
        /\ SF_vars(Drain)

(* Threads that do not stop (Stall FALSE): each thread goes on taking      *)
(* steps, and no step is singled out.  No strong fairness, on Drain or     *)
(* anything else.                                                          *)
SpecWeak == Init /\ [][Next]_vars
            /\ WF_vars(KernelReceive) /\ WF_vars(KernelRefresh)
            /\ WF_vars(MainStep) /\ WF_vars(WorkerStep)

(***************************************************************************)
(* Invariants.                                                             *)
(***************************************************************************)

(* The fill ring only ever holds receive-region frames: a frame outside    *)
(* the region on it would be received into while it is also the send       *)
(* path's.                                                                 *)
FillHoldsOnlyReceiveFrames == \A i \in 1..Len(fq) : fq[i] \in Frames

(* Every frame is in exactly one of the fill ring (untaken), the RX ring   *)
(* (untaken) or the worker's hands (the receive banks hold copies), once   *)
(* each, and the fill ring holds nothing else.                             *)
FrameConservation ==
    /\ FillHoldsOnlyReceiveFrames
    /\ Cardinality(Untaken) = Cardinality(InRing)
    /\ Len(fq) - kused = Cardinality(Fill)
    /\ Fill \cap InRing = {}
    /\ Fill \cap pending = {}
    /\ InRing \cap pending = {}
    /\ Fill \cup InRing \cup pending = Frames

(* Between passes the cached consumer is the consumer: every peeked        *)
(* descriptor was released or cancelled.                                   *)
ConsumerNotBehind == phase = "idle" => ahead = 0

(* Between passes the worker holds no frame: every flush fits, because the *)
(* ring has a slot for each frame beyond the kernel's unpublished batch.   *)
PendingReturnedEachPass == phase = "idle" => pending = {}

(* Every receive bank has exactly one owner: the worker's producer, main's *)
(* held bank, or one live entry of the spare ring.                         *)
BankOneOwner ==
    \A b \in Banks :
        (IF workerBank = b THEN 1 ELSE 0)
      + (IF mainBank = b THEN 1 ELSE 0)
      + FreeOwns(b) = 1

(* Every packet staged before a claim is in that claim's view: each packet *)
(* staged before main's last claim was consumed earlier or is in the bank  *)
(* main now holds, none left behind in the worker's bank (the reversed     *)
(* DrainStale does leave the most recently staged one behind) and none     *)
(* handed back to the worker unread (DoubleSwap, SwapUnread).              *)
StagedBeforeClaimInView ==
    LET view == IF mainBank = NONE THEN {} ELSE Range(bankContent[mainBank]) IN
    \A n \in 1..claimMark : n \in Range(consumed) \cup view

(* Main consumes the staged packets in the order they were staged, none    *)
(* skipped and none twice, across banks and across calls.                  *)
ConsumedInOrder == \A i \in 1..Len(consumed) : consumed[i] = i

(* The bank a call has read from stays main's until the call ends: the     *)
(* packets the call returned point into it, so it is never the worker's    *)
(* to stage into while they are in use.                                    *)
ViewStable == viewBank # NONE => viewBank = mainBank

(* Nobody but main changes the bank main holds: it has the packets it had  *)
(* when main claimed it, or none once main emptied it.  The worker stages  *)
(* only into the producer bank it read under its current hold.             *)
HeldBankStable == mainBank # NONE => bankContent[mainBank] = heldView

(* The worker's bank has room for every descriptor of the pass still to be *)
(* taken: its count of the bank never understated what the bank holds      *)
(* (the assert before each append).                                        *)
PeekedFitsBank ==
    phase = "taking" => peeked - taken <= StageCap - Len(bankContent[workerBank])

(* Main's flag is raised only while main spins for the mutex: it never     *)
(* outlives the bounded tries of the claim that raised it, so the worker   *)
(* is kept off the mutex for no longer than that.                          *)
WantedOnlyInSpin == wanted => mainPhase = "spin"

(* The bound on a miss, for threads that do not stop (Stall FALSE, with    *)
(* SpinMax at least 2): main's claim takes the mutex within its bounded    *)
(* tries, in the call that asked.  No call's claim misses.                 *)
ClaimWithinSpin == missRun = 0

(* No arrival is dropped for a full RX ring while the worker is kept off   *)
(* the mutex for main, if the ring had room for a whole spin's arrivals    *)
(* when main raised its flag (Stall FALSE: at most TryArrivals packets     *)
(* arrive between two of main's tries).                                    *)
NoDropWhilePaused == ~badDrop

(* The kernel finds the fill ring empty (no entry it has not taken, so a   *)
(* refresh peeks nothing) only while no frame is free: none is waiting in  *)
(* the worker's hands between passes to go back.                           *)
FillEmpty == kused = Len(fq)

FillNeverEmptyWhileFramesFree == FillEmpty => ~(phase = "idle" /\ pending # {})

(***************************************************************************)
(* Liveness: receive never stalls while a descriptor is posted or a frame  *)
(* is free and another packet may still arrive: a pass eventually takes    *)
(* one.  And main catches up: again and again every packet staged so far   *)
(* has been consumed, so a missed claim (or a full bank) defers packets    *)
(* and never strands them.                                                 *)
(***************************************************************************)
CanStillTake == ring # <<>> \/ (Fill # {} /\ Arrivals < MaxPackets)

ReceiveNeverStalls == CanStillTake ~> took

StagedEventuallyConsumed == []<>(Len(consumed) = nextPacket)
=============================================================================
