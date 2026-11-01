------------------------- MODULE RNResendGenerations -------------------------
(***************************************************************************)
(* The names a transport ack follows into the resend FIFO of               *)
(* sdk/rocketnet_window_ack.cpp, at the code's slot layout: W per-number   *)
(* entries indexed n % W (send_numbers and resend_fifo), descriptors that  *)
(* are freed and taken again with the next generation, and packets that   *)
(* still carry a number after its slot holds a later one.                  *)
(*                                                                         *)
(* A link is the slot's prev/next pair and carries no name of its own:    *)
(* rn_resend_link_t is 8 bytes, prev and next.  An ack of a packet visits  *)
(* each number the packet referenced: it skips a slot that is acked,       *)
(* outside [U, next) or holds another number, then unlinks the slot,       *)
(* frees the descriptor's fragment and marks the number acked              *)
(* (rn_window_ack_on_ack).  The number comparison is the only test that    *)
(* keeps a stale packet off a later number in the same slot.              *)
(*                                                                         *)
(* Each message here is one fragment, so its descriptor is freed by the    *)
(* ack of its one number; a block's descriptor waits for its last          *)
(* fragment, which changes no name.  Generations run to GenMax, wrapping   *)
(* to 1 as the allocator does at UINT32_MAX; RNSendStoreLayout checks the  *)
(* wrap itself.                                                            *)
(*                                                                         *)
(* Parameters (the code's values first):                                   *)
(*   CheckNumber   TRUE: the ack skips a slot that holds another number    *)
(*   WindowSlack   0: a number is written only while nxt < U + W, so a     *)
(*                 slot is reused only after its number is acked; 1 is    *)
(*                 the off-by-one at capacity                              *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS
    W,                  \* window entries (a power of two in the code)
    N,                  \* numbers 0 .. N - 1 are written
    D,                  \* send descriptors 1 .. D
    GenMax,             \* the generation after GenMax is 1
    CheckNumber,
    WindowSlack

ASSUME W \in Nat \ {0} /\ N \in Nat /\ D \in Nat \ {0} /\ GenMax \in Nat \ {0}
ASSUME CheckNumber \in BOOLEAN /\ WindowSlack \in {0, 1}

Slots == 0..(W - 1)
Nums == 0..(N - 1)
Descs == 1..D
NoDesc == 0                         \* RN_DESCRIPTOR_NONE
Gens == 0..GenMax                   \* 0 is never a live name

SlotOf(n) == n % W

VARIABLES
    U,          \* send->U: the oldest unacked number
    nxt,        \* send->next: the next number to write
    sn,         \* sn[s]: send_numbers[s], [n, d, acked]
    lk,         \* lk[s]: resend_fifo[s] is linked (bit 31 of prev)
    dIn,        \* dIn[d]: send_descriptors[d].in_use
    dGen,       \* dGen[d]: send_descriptors[d].generation (0: never taken)
    fly,        \* fly[n]: packets in the sent map that reference n
    wrote,      \* ghost: [n, d, g] for each number written by d at generation g
    got,        \* ghost: numbers carried by a packet the receiver acked
    freed       \* ghost: [d, g] for each fragment an ack returned

vars == << U, nxt, sn, lk, dIn, dGen, fly, wrote, got, freed >>

\* rn_window_send_mark_acked: U moves over the acked numbers in order
AdvanceU(u, snS) ==
    CHOOSE m \in u..nxt :
        /\ \A k \in u..(m - 1) : snS[SlotOf(k)].n = k /\ snS[SlotOf(k)].acked
        /\ (m = nxt \/ ~(snS[SlotOf(m)].n = m /\ snS[SlotOf(m)].acked))

-----------------------------------------------------------------------------

Init ==
    /\ U = 0
    /\ nxt = 0
    /\ sn = [ s \in Slots |-> [ n |-> -1, d |-> NoDesc, acked |-> FALSE ] ]
    /\ lk = [ s \in Slots |-> FALSE ]
    /\ dIn = [ d \in Descs |-> FALSE ]
    /\ dGen = [ d \in Descs |-> 0 ]
    /\ fly = [ n \in Nums |-> 0 ]
    /\ wrote = {}
    /\ got = {}
    /\ freed = {}

\* rn_region_send_alloc takes a free descriptor with the next generation
\* (rocketnet_allocator.cpp), rn_window_send_first_write numbers it at
\* slot nxt % W, and rn_window_ack_park links that slot (insert_after:
\* unlink, link).  rn_window_send_admit numbers one record only while
\* next - U' <= W - 1, and U' <= U, so the guard nxt < U + W here admits
\* every write the code does.
Write(d) ==
    /\ nxt < N
    /\ nxt < U + W + WindowSlack
    /\ ~dIn[d]
    /\ LET g == IF dGen[d] = 0 \/ dGen[d] = GenMax THEN 1 ELSE dGen[d] + 1
           s == SlotOf(nxt)
       IN /\ dIn' = [ dIn EXCEPT ![d] = TRUE ]
          /\ dGen' = [ dGen EXCEPT ![d] = g ]
          /\ sn' = [ sn EXCEPT ![s] = [ n |-> nxt, d |-> d, acked |-> FALSE ] ]
          /\ lk' = [ lk EXCEPT ![s] = TRUE ]
          /\ wrote' = wrote \cup { [ n |-> nxt, d |-> d, g |-> g ] }
    /\ nxt' = nxt + 1
    /\ UNCHANGED << U, fly, got, freed >>

\* rn_window_ack_commit_packet: a packet referencing n enters the sent map
\* and n moves to its class's tail (push_tail -> insert_after: unlink,
\* link).  rn_window_ack_packet_failed's push_head relinks the same way.
Send(n) ==
    LET s == SlotOf(n) IN
    /\ U <= n /\ n < nxt
    /\ sn[s].n = n
    /\ ~sn[s].acked
    /\ fly[n] < 2
    /\ fly' = [ fly EXCEPT ![n] = fly[n] + 1 ]
    /\ lk' = [ lk EXCEPT ![s] = TRUE ]
    /\ UNCHANGED << U, nxt, sn, dIn, dGen, wrote, got, freed >>

\* the environment: a packet is lost, or its sent-map entry is evicted
Lose(n) ==
    /\ fly[n] > 0
    /\ fly' = [ fly EXCEPT ![n] = fly[n] - 1 ]
    /\ UNCHANGED << U, nxt, sn, lk, dIn, dGen, wrote, got, freed >>

\* rn_window_ack_on_ack, step 2, for a packet that referenced n: the
\* number's slot, the skips, the unlink, the fragment and the mark.
Ack(n) ==
    LET s == SlotOf(n)
        e == sn[s]
        skip == \/ e.acked
                \/ e.n < U
                \/ e.n >= nxt
                \/ (CheckNumber /\ e.n # n)
        live == e.d \in Descs /\ dIn[e.d]
        sn1 == [ sn EXCEPT ![s].acked = TRUE ]
    IN
    /\ fly[n] > 0
    /\ fly' = [ fly EXCEPT ![n] = fly[n] - 1 ]
    /\ got' = got \cup {n}
    /\ IF skip
       THEN UNCHANGED << U, sn, lk, dIn, freed >>
       ELSE /\ lk' = [ lk EXCEPT ![s] = FALSE ]
            /\ sn' = sn1
            /\ dIn' = IF live THEN [ dIn EXCEPT ![e.d] = FALSE ] ELSE dIn
            /\ freed' = IF live THEN freed \cup { [ d |-> e.d, g |-> dGen[e.d] ] } ELSE freed
            /\ U' = AdvanceU(U, sn1)
    /\ UNCHANGED << nxt, dGen, wrote >>

Next ==
    \/ \E d \in Descs : Write(d)
    \/ \E n \in Nums : Send(n) \/ Lose(n) \/ Ack(n)

Spec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------

TypeOK ==
    /\ U \in 0..N /\ nxt \in 0..N /\ U <= nxt /\ nxt <= U + W + WindowSlack
    /\ \A s \in Slots : sn[s].n \in -1..(N - 1) /\ sn[s].d \in {NoDesc} \cup Descs /\ sn[s].acked \in BOOLEAN
    /\ lk \in [ Slots -> BOOLEAN ]
    /\ dIn \in [ Descs -> BOOLEAN ]
    /\ dGen \in [ Descs -> Gens ]
    /\ fly \in [ Nums -> 0..2 ]

\* An ack returns a fragment only to the descriptor generation that wrote a
\* number the acked packet carried.
NamedGenerationOnly ==
    \A f \in freed : \E w \in wrote : w.d = f.d /\ w.g = f.g /\ w.n \in got

\* A number is marked acked only when a packet carrying it was acked.
AckedOnlyDelivered ==
    \A s \in Slots : sn[s].acked => sn[s].n \in got

\* A linked entry is an unacked number whose descriptor is live at the
\* generation that wrote it: the slot alone names its owner.
LinkNamesOwner ==
    \A s \in Slots : lk[s] =>
        /\ ~sn[s].acked
        /\ sn[s].d \in Descs
        /\ dIn[sn[s].d]
        /\ \E w \in wrote : w.n = sn[s].n /\ w.d = sn[s].d /\ w.g = dGen[sn[s].d]

\* Every number in [U, nxt) that is not acked still owns its slot: no
\* write has reused the slot of an unacked number.
WindowOwnsSlots ==
    \A n \in Nums : (U <= n /\ n < nxt) => (sn[SlotOf(n)].n = n \/ sn[SlotOf(n)].acked)

\* Reachability, violated by MCRNResendGenerationsReachStale.cfg: a packet
\* still references a number whose slot now holds a later number written by
\* the same descriptor at a later generation.
NoStaleNameInFlight ==
    ~\E w \in wrote :
        /\ fly[w.n] > 0
        /\ dIn[w.d]
        /\ dGen[w.d] # w.g
        /\ sn[SlotOf(w.n)].n # w.n
        /\ sn[SlotOf(w.n)].d = w.d
=============================================================================
