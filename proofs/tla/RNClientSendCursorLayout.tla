------------------------ MODULE RNClientSendCursorLayout ------------------------
(***************************************************************************)
(* The client send cursor at its layout.  rocketnet_client_io_send_ring    *)
(* (sdk/rocketnet_client_io.h) walks one handed-off ring from send_slot up  *)
(* to that ring's committed count.  Slot i is the array entry at            *)
(* data + i * frame_stride; a zero wire byte count is aborted and is not a  *)
(* batch entry.  The progress branch (send_batch_progress) keeps at most    *)
(* ROCKETNET_CLIENT_IO_BATCH live slots, asks the transport to consume a    *)
(* prefix of that batch, and sets                                          *)
(*                                                                         *)
(*   send_slot = consumed < num_sends ? slots[consumed] : next             *)
(*                                                                         *)
(* so a retry starts at the first live slot the prefix did not finish, or  *)
(* past the slots the walk already passed when the prefix finished the     *)
(* batch.  The terminal branch (no send_batch_progress, or a quit attempt) *)
(* assigns send_slot = committed in one step and is not this model.        *)
(*                                                                         *)
(* The transport under that call is rocketnet_client_race_send_progress    *)
(* (sdk/rocketnet_client_transport.cpp).  A ring handed off while both     *)
(* families are in flight is addressed to family 0, and each of its live   *)
(* slots is owed on both family bits.  A physical family consumes or       *)
(* refuses its one copy; the call returns the prefix of packets whose      *)
(* required bits are all consumed, and remembers the bits already consumed *)
(* so the retry does not send them again.  rocketnet_client_race_commit    *)
(* chooses the kept family.  Observed on the next attempt, a packet not    *)
(* yet started for the other family is dropped, and a packet already in    *)
(* flight keeps only the kept family's bit.  A ring taken after the commit *)
(* is addressed to the kept family alone.                                  *)
(*                                                                         *)
(* The model addresses a ring to a family; the code addresses each slot to *)
(* a server and takes the family from the destination's address family     *)
(* (rocketnet_client_race_family_of), dropping after the commit a slot not *)
(* of the kept family.  The two are equal: only the two server addresses   *)
(* are ever sent to (server_address[0] IPv4, server_address[1] IPv6), so a *)
(* destination's address family names its server, and the commit sets     *)
(* protocol.server_address to server_address[kept] and rebinds a held ring *)
(* bound to the losing server (rocketnet_client_io_rebind_held).           *)
(*                                                                         *)
(* The layout, checked at C = 4 and B = 2 (the code's ring holds 256 and   *)
(* its batch is 64; the walk is the same).  slot is the array, cursor is   *)
(* send_slot, count is the committed length.  Idx(i) = i % C is how the    *)
(* array is read.  Every i the walk reads satisfies i < count <= C, so     *)
(* Idx(i) = i, the same index as data + i * stride.                        *)
(*                                                                         *)
(*   AtMostOnce        a live slot is sent at most once on a family.       *)
(*   ExactlyOnce       once the ring is released, every family it owed at  *)
(*                     completion was sent once, and a send only went to a *)
(*                     family that was required while the slot was in      *)
(*                     flight.                                             *)
(*   NoneLost          once the ring is released, every live slot either   *)
(*                     completed owing at least one family, or was dropped *)
(*                     from a ring addressed to the losing family (the     *)
(*                     ghost dropped[g][i]: the ring's family at the drop, *)
(*                     -1 for none).                                       *)
(*   InOrder           each family's sends are strictly increasing slot    *)
(*                     indexes.                                            *)
(*   NotAfterRelease   a released generation is not held, and its send     *)
(*                     counts stay at the counts release recorded.         *)
(*                                                                         *)
(* CursorAt = "slots" is the code.  CursorAt = "plus" is the reversed      *)
(* witness cursor = send_slot + consumed, which counts batch entries and   *)
(* not the aborted slots the walk skipped, so a later attempt sends a live *)
(* slot again.  DropKept = FALSE is the code: the drop arm takes only a    *)
(* packet whose family is not the committed one (family != committed in   *)
(* rocketnet_client_race_send_progress).  DropKept = TRUE is the reversed  *)
(* witness that drops an unstarted packet of the kept family too: no copy  *)
(* is sent and none is owed, so ExactlyOnce holds and NoneLost fails.      *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS C,          \* slots in the ring array
          B,          \* live slots one batch keeps
          MaxTake,    \* rings taken, each a generation
          CursorAt,   \* "slots" the code, "plus" the reversed witness
          DropKept    \* FALSE the code, TRUE the reversed witness: the drop arm drops a kept-family packet too

ASSUME C = 4 /\ B = 2 /\ MaxTake = 2
ASSUME CursorAt \in {"slots", "plus"}
ASSUME DropKept \in BOOLEAN

Ids == 0..C - 1
Fams == {0, 1}
Gens == 0..MaxTake - 1
Plans == [0..B - 1 -> [Fams -> BOOLEAN]]

\* the array index.  i < C keeps it equal to the code's slot number.
Idx(i) == i % C

VARIABLES slot,       \* the ring array: TRUE live, FALSE aborted
          cursor,     \* send_slot
          count,      \* committed length of the ring in hand
          held,       \* a ring is in the sending hand
          gen,        \* that ring's generation
          takes,      \* rings taken so far
          dest,       \* the ring's address family
          commit,     \* -1 while both families are in flight, else the kept one
          pending,    \* a packet's family bits are only partly consumed
          req,        \* families that packet required, before the commit mask
          got,        \* families that already consumed their copy
          pend,       \* the ring index of that packet
          viol,       \* the progress assert: a retry's batch head is not that packet
          sent,       \* sent[g][i][f] copies of generation g, slot i, family f
          touched,    \* families required while the slot was in flight
          owed,       \* families required at the attempt that finished the slot
          ord,        \* ord[g][f] the slot indexes sent to that family, in order
          live,       \* live[g][i] the slot was live in that generation
          rel,        \* the generation has been released
          snap,       \* send counts recorded at release
          dropped     \* dropped[g][i] the family of the ring the drop arm took slot i from, -1 if none

vars == <<slot, cursor, count, held, gen, takes, dest, commit, pending, req, got, pend,
          viol, sent, touched, owed, ord, live, rel, snap, dropped>>

Zero == [g \in Gens |-> [i \in Ids |-> [f \in Fams |-> 0]]]
None == [g \in Gens |-> [i \in Ids |-> {}]]
NoOrd == [g \in Gens |-> [f \in Fams |-> <<>>]]
NoLive == [g \in Gens |-> [i \in Ids |-> FALSE]]
NoRel == [g \in Gens |-> FALSE]
NoDrop == [g \in Gens |-> [i \in Ids |-> -1]]

Init ==
    /\ slot = [i \in Ids |-> FALSE]
    /\ cursor = 0
    /\ count = 0
    /\ held = FALSE
    /\ gen = 0
    /\ takes = 0
    /\ dest = 0
    /\ commit = -1
    /\ pending = FALSE
    /\ req = {}
    /\ got = {}
    /\ pend = 0
    /\ viol = FALSE
    /\ sent = Zero
    /\ touched = None
    /\ owed = None
    /\ ord = NoOrd
    /\ live = NoLive
    /\ rel = NoRel
    /\ snap = Zero
    /\ dropped = NoDrop

\* send counts stop at 2: one past the at-most-once line, and no further
Bump(s, g, i, f) ==
    IF s[g][i][f] >= 2 THEN s
    ELSE [s EXCEPT ![g][i][f] = s[g][i][f] + 1]

Note(o, g, f, i, before, after) ==
    IF before = after THEN o
    ELSE [o EXCEPT ![g][f] = Append(o[g][f], i)]

\* the walk from `from`, keeping at most `need` live slots and skipping aborted
RECURSIVE TakeLives(_, _, _)
TakeLives(from, need, acc) ==
    IF need = 0 \/ from >= count THEN <<acc, from>>
    ELSE IF slot[Idx(from)] THEN TakeLives(from + 1, need - 1, Append(acc, from))
    ELSE TakeLives(from + 1, need, acc)

\* one send_batch_progress over the batch.  plan[k][f] is whether family f
\* consumes its copy of batch entry k on this attempt.  A packet whose
\* required bits are not all consumed stays pending and the call returns
\* there; a dropped losing-family packet counts as consumed and is not sent.
RECURSIVE Run(_, _, _, _)
Run(i, st, plan, batch) ==
    IF i >= Len(batch) THEN [st EXCEPT !.pending = FALSE, !.req = {}, !.got = {},
                                     !.consumed = Len(batch)]
    ELSE LET idx == batch[i + 1]
             starting == ~st.pending
             drop == starting /\ commit >= 0 /\ (dest # commit \/ DropKept)
         IN IF drop THEN Run(i + 1, [st EXCEPT !.pending = FALSE, !.req = {}, !.got = {},
                                              !.dropped = [st.dropped EXCEPT ![gen][idx] = dest]],
                             plan, batch)
            ELSE LET rq == IF starting
                            THEN IF commit < 0 /\ dest = 0 THEN Fams ELSE {dest}
                            ELSE st.req
                     need == IF commit >= 0 THEN rq \cap {commit} ELSE rq
                     ok0 == 0 \in need /\ 0 \notin st.got /\ plan[i][0]
                     sent1 == IF ok0 THEN Bump(st.sent, gen, idx, 0) ELSE st.sent
                     ord1 == IF ok0 THEN Note(st.ord, gen, 0, idx, st.sent[gen][idx][0], sent1[gen][idx][0])
                             ELSE st.ord
                     got1 == IF ok0 THEN st.got \cup {0} ELSE st.got
                     ok1 == 1 \in need /\ 1 \notin got1 /\ plan[i][1]
                     sent2 == IF ok1 THEN Bump(sent1, gen, idx, 1) ELSE sent1
                     ord2 == IF ok1 THEN Note(ord1, gen, 1, idx, sent1[gen][idx][1], sent2[gen][idx][1])
                             ELSE ord1
                     got2 == IF ok1 THEN got1 \cup {1} ELSE got1
                     touched1 == [st.touched EXCEPT ![gen][idx] = st.touched[gen][idx] \cup need]
                     mid == [pending |-> TRUE, req |-> rq, got |-> got2, consumed |-> i,
                             sent |-> sent2, touched |-> touched1, owed |-> st.owed, ord |-> ord2,
                             dropped |-> st.dropped]
                 IN IF need \subseteq got2
                    THEN LET done == [mid EXCEPT !.pending = FALSE, !.req = {}, !.got = {},
                                               !.owed = [mid.owed EXCEPT ![gen][idx] = need]]
                         IN Run(i + 1, done, plan, batch)
                    ELSE mid

\* the code's cursor, or the reversed witness send_slot + consumed
CursorOf(consumed, ns, batch, nxt) ==
    IF CursorAt = "plus" THEN cursor + consumed
    ELSE IF consumed < ns THEN batch[consumed + 1] ELSE nxt

\* a handed-off ring: its array, its committed count, the cursor at 0
Take ==
    /\ ~held
    /\ takes < MaxTake
    /\ \E n \in 1..C:
         \E liveSet \in SUBSET (0..n - 1):
            /\ slot' = [i \in Ids |-> i < n /\ i \in liveSet]
            /\ count' = n
            /\ cursor' = 0
            /\ held' = TRUE
            /\ gen' = takes
            /\ takes' = takes + 1
            /\ dest' = IF commit = -1 THEN 0 ELSE commit
            /\ pending' = FALSE
            /\ req' = {}
            /\ got' = {}
            /\ pend' = 0
            /\ live' = [live EXCEPT ![takes] = [i \in Ids |-> i < n /\ i \in liveSet]]
            /\ UNCHANGED <<commit, viol, sent, touched, owed, ord, rel, snap, dropped>>

\* one progress batch.  An empty batch (only aborted slots remain) advances
\* to the walk's end.  The plus witness adds the consumed count to the
\* cursor and stays put when that count is 0.
Send ==
    /\ held
    /\ cursor < count
    /\ ~rel[gen]
    /\ LET picked == TakeLives(cursor, B, <<>>)
           batch == picked[1]
           nxt == picked[2]
           ns == Len(batch)
           blank == [pending |-> pending, req |-> req, got |-> got, consumed |-> 0,
                     sent |-> sent, touched |-> touched, owed |-> owed, ord |-> ord, dropped |-> dropped]
       IN IF pending /\ (IF ns = 0 THEN TRUE ELSE batch[1] # pend)
          THEN /\ viol' = TRUE
               /\ UNCHANGED <<slot, cursor, count, held, gen, takes, dest, commit, pending,
                              req, got, pend, sent, touched, owed, ord, live, rel, snap, dropped>>
          ELSE IF ns = 0
          THEN /\ cursor' = CursorOf(0, 0, batch, nxt)
               /\ UNCHANGED <<slot, count, held, gen, takes, dest, commit, pending, req, got,
                              pend, viol, sent, touched, owed, ord, live, rel, snap, dropped>>
          ELSE \E plan \in Plans:
                LET r == Run(0, blank, plan, batch)
                IN /\ cursor' = CursorOf(r.consumed, ns, batch, nxt)
                   /\ pending' = r.pending
                   /\ req' = r.req
                   /\ got' = r.got
                   /\ pend' = IF r.pending THEN batch[r.consumed + 1] ELSE pend
                   /\ sent' = r.sent
                   /\ touched' = r.touched
                   /\ owed' = r.owed
                   /\ ord' = r.ord
                   /\ dropped' = r.dropped
                   /\ UNCHANGED <<slot, count, held, gen, takes, dest, commit, viol, live, rel, snap>>

\* the caller's commit, observed by the next Send
Commit ==
    /\ commit = -1
    /\ \E f \in Fams: commit' = f
    /\ UNCHANGED <<slot, cursor, count, held, gen, takes, dest, pending, req, got, pend, viol,
                   sent, touched, owed, ord, live, rel, snap, dropped>>

\* send_slot = committed: the ring goes back to the handoff, and its counts
\* are the ones a later send must not change
Release ==
    /\ held
    /\ cursor = count
    /\ ~pending
    /\ held' = FALSE
    /\ rel' = [rel EXCEPT ![gen] = TRUE]
    /\ snap' = [snap EXCEPT ![gen] = sent[gen]]
    /\ UNCHANGED <<slot, cursor, count, gen, takes, dest, commit, pending, req, got, pend, viol,
                   sent, touched, owed, ord, live, dropped>>

\* the run has taken every ring it will take
Done ==
    /\ ~held
    /\ takes = MaxTake
    /\ UNCHANGED vars

Next == Take \/ Send \/ Commit \/ Release \/ Done

Spec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------

TypeOK ==
    /\ slot \in [Ids -> BOOLEAN]
    /\ cursor \in 0..C
    /\ count \in 0..C
    /\ ~held \/ cursor <= count
    /\ \A i \in Ids: Idx(i) = i
    /\ held \in BOOLEAN
    /\ gen \in Gens
    /\ takes \in 0..MaxTake
    /\ dest \in Fams
    /\ commit \in {-1} \cup Fams
    /\ pending \in BOOLEAN
    /\ req \subseteq Fams
    /\ got \subseteq Fams
    /\ pend \in Ids
    /\ viol \in BOOLEAN
    /\ sent \in [Gens -> [Ids -> [Fams -> 0..2]]]
    /\ touched \in [Gens -> [Ids -> SUBSET Fams]]
    /\ owed \in [Gens -> [Ids -> SUBSET Fams]]
    /\ ord \in [Gens -> [Fams -> Seq(Ids)]]
    /\ live \in [Gens -> [Ids -> BOOLEAN]]
    /\ rel \in [Gens -> BOOLEAN]
    /\ snap \in [Gens -> [Ids -> [Fams -> 0..2]]]
    /\ dropped \in [Gens -> [Ids -> {-1} \cup Fams]]

AtMostOnce ==
    /\ ~viol
    /\ \A g \in Gens, i \in Ids, f \in Fams: sent[g][i][f] <= 1

ExactlyOnce ==
    \A g \in Gens:
        rel[g] =>
            \A i \in Ids:
                live[g][i] =>
                    /\ \A f \in Fams: f \in owed[g][i] => sent[g][i][f] = 1
                    /\ \A f \in Fams: sent[g][i][f] >= 1 => f \in touched[g][i]

\* a live slot never leaves the walk unaccounted: it completed owing a
\* family, or the drop arm took it from a ring of the losing family
\* (commit is set before any drop and never changes)
NoneLost ==
    \A g \in Gens, i \in Ids:
        rel[g] /\ live[g][i] =>
            \/ owed[g][i] # {}
            \/ dropped[g][i] # -1 /\ dropped[g][i] # commit

InOrder ==
    \A g \in Gens, f \in Fams:
        \A k \in 1..Len(ord[g][f]) - 1: ord[g][f][k] < ord[g][f][k + 1]

NotAfterRelease ==
    /\ ~held \/ ~rel[gen]
    /\ \A g \in Gens:
         rel[g] => \A i \in Ids, f \in Fams: sent[g][i][f] = snap[g][i][f]

=============================================================================
