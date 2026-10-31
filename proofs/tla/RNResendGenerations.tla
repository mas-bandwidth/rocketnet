------------------------- MODULE RNResendGenerations -------------------------
(***************************************************************************)
(* The names a transport ack follows into the resend FIFO of               *)
(* sdk/rocketnet_window_ack.cpp, at the code's slot layout: W per-number   *)
(* entries indexed n % W (send_numbers and resend_fifo), descriptors that  *)
(* are freed and taken again with the next generation, and packets that   *)
(* still carry a number after its slot holds a later one.                  *)
(*                                                                         *)
(* A link is stamped with its descriptor's generation when it is linked:   *)
(* on park and again on every requeue (rn_resend_fifo_stamp from           *)
(* rn_resend_fifo_insert_after); the unlink clears the stamp.  An ack of a *)
(* packet visits each number the packet referenced: it skips a slot that   *)
(* is acked, outside [U, next) or holds another number, then removes the   *)
(* link only when the slot's live descriptor still has the stamped         *)
(* generation (rn_resend_fifo_unlink_named, and the generation-0 branch in *)
(* rn_window_ack_on_ack), frees the descriptor's fragment and marks the     *)
(* number acked.                                                           *)
(*                                                                         *)
(* Each message here is one fragment, so its descriptor is freed by the    *)
(* ack of its one number; a block's descriptor waits for its last          *)
(* fragment, which changes no name.  Generations run to GenMax, wrapping   *)
(* to 1 as the allocator does at UINT32_MAX; RNSendStoreLayout checks the  *)
(* wrap itself.                                                            *)
(*                                                                         *)
(* Flags (TRUE is the code):                                               *)
(*   CheckNumber        the ack skips a slot that holds another number     *)
(*   Restamp            a requeue stamps the link again after its unlink   *)
(*   CompareGeneration  the ack refuses a link whose stamp is not the live *)
(*                      descriptor's generation                            *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS
    W,                  \* window entries (a power of two in the code)
    N,                  \* numbers 0 .. N - 1 are written
    D,                  \* send descriptors 1 .. D
    GenMax,             \* the generation after GenMax is 1
    CheckNumber,
    Restamp,
    CompareGeneration

ASSUME W \in Nat \ {0} /\ N \in Nat /\ D \in Nat \ {0} /\ GenMax \in Nat \ {0}
ASSUME \A f \in {CheckNumber, Restamp, CompareGeneration} : f \in BOOLEAN

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
    st,         \* st[s]: resend_fifo[s].generation, the stamp
    dIn,        \* dIn[d]: send_descriptors[d].in_use
    dGen,       \* dGen[d]: send_descriptors[d].generation (0: never taken)
    fly,        \* fly[n]: packets in the sent map that reference n
    wrote,      \* ghost: [n, d, g] for each number written by d at generation g
    got,        \* ghost: numbers carried by a packet the peer acked
    freed       \* ghost: [d, g] for each fragment an ack returned

vars == << U, nxt, sn, lk, st, dIn, dGen, fly, wrote, got, freed >>

\* rn_resend_fifo_stamp: the live descriptor's generation, or 0
Stamp(snS, dInS, dGenS) ==
    IF snS.d \in Descs /\ dInS[snS.d] /\ dGenS[snS.d] # 0 THEN dGenS[snS.d] ELSE 0

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
    /\ st = [ s \in Slots |-> 0 ]
    /\ dIn = [ d \in Descs |-> FALSE ]
    /\ dGen = [ d \in Descs |-> 0 ]
    /\ fly = [ n \in Nums |-> 0 ]
    /\ wrote = {}
    /\ got = {}
    /\ freed = {}

\* rn_region_send_alloc takes a free descriptor with the next generation
\* (rocketnet_allocator.cpp), rn_window_send_first_write numbers it at
\* slot nxt % W, and rn_window_ack_park links that slot (insert_after:
\* unlink, link, stamp).
Write(d) ==
    /\ nxt < N
    /\ nxt < U + W
    /\ ~dIn[d]
    /\ LET g == IF dGen[d] = 0 \/ dGen[d] = GenMax THEN 1 ELSE dGen[d] + 1
           s == SlotOf(nxt)
       IN /\ dIn' = [ dIn EXCEPT ![d] = TRUE ]
          /\ dGen' = [ dGen EXCEPT ![d] = g ]
          /\ sn' = [ sn EXCEPT ![s] = [ n |-> nxt, d |-> d, acked |-> FALSE ] ]
          /\ lk' = [ lk EXCEPT ![s] = TRUE ]
          /\ st' = [ st EXCEPT ![s] = g ]
          /\ wrote' = wrote \cup { [ n |-> nxt, d |-> d, g |-> g ] }
    /\ nxt' = nxt + 1
    /\ UNCHANGED << U, fly, got, freed >>

\* rn_window_ack_commit_packet: a packet referencing n enters the sent map
\* and n moves to its class's tail (push_tail -> insert_after: unlink,
\* which clears the stamp, link, stamp).  rn_window_ack_packet_failed's
\* push_head relinks the same way.
Send(n) ==
    LET s == SlotOf(n) IN
    /\ U <= n /\ n < nxt
    /\ sn[s].n = n
    /\ ~sn[s].acked
    /\ fly[n] < 2
    /\ fly' = [ fly EXCEPT ![n] = fly[n] + 1 ]
    /\ lk' = [ lk EXCEPT ![s] = TRUE ]
    /\ st' = [ st EXCEPT ![s] = IF Restamp THEN Stamp(sn[s], dIn, dGen) ELSE 0 ]
    /\ UNCHANGED << U, nxt, sn, dIn, dGen, wrote, got, freed >>

\* the environment: a packet is lost, or its sent-map entry is evicted
Lose(n) ==
    /\ fly[n] > 0
    /\ fly' = [ fly EXCEPT ![n] = fly[n] - 1 ]
    /\ UNCHANGED << U, nxt, sn, lk, st, dIn, dGen, wrote, got, freed >>

\* rn_window_ack_on_ack, step 2, for a packet that referenced n: the
\* number's slot, the skips, the named unlink, the fragment and the mark.
Ack(n) ==
    LET s == SlotOf(n)
        e == sn[s]
        skip == \/ e.acked
                \/ e.n < U
                \/ e.n >= nxt
                \/ (CheckNumber /\ e.n # n)
        live == e.d \in Descs /\ dIn[e.d]
        refused == /\ lk[s]
                   /\ CompareGeneration
                   /\ IF st[s] = 0 THEN live ELSE live /\ dGen[e.d] # st[s]
        sn1 == [ sn EXCEPT ![s].acked = TRUE ]
    IN
    /\ fly[n] > 0
    /\ fly' = [ fly EXCEPT ![n] = fly[n] - 1 ]
    /\ got' = got \cup {n}
    /\ IF skip \/ refused
       THEN UNCHANGED << U, sn, lk, st, dIn, freed >>
       ELSE /\ lk' = [ lk EXCEPT ![s] = FALSE ]
            /\ st' = [ st EXCEPT ![s] = 0 ]
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
    /\ U \in 0..N /\ nxt \in 0..N /\ U <= nxt /\ nxt <= U + W
    /\ \A s \in Slots : sn[s].n \in -1..(N - 1) /\ sn[s].d \in {NoDesc} \cup Descs /\ sn[s].acked \in BOOLEAN
    /\ lk \in [ Slots -> BOOLEAN ]
    /\ st \in [ Slots -> Gens ]
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

\* A linked entry is an unacked number whose descriptor is live, and its
\* stamp is the generation that wrote it.
StampNamesOwner ==
    \A s \in Slots : lk[s] =>
        /\ ~sn[s].acked
        /\ sn[s].d \in Descs
        /\ dIn[sn[s].d]
        /\ st[s] = dGen[sn[s].d]
        /\ \E w \in wrote : w.n = sn[s].n /\ w.d = sn[s].d /\ w.g = st[s]

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
