------------------------ MODULE RNXdpReplyMacsLayout ------------------------
(***************************************************************************)
(* The reply MAC table of the XDP server transport with no gateway, as     *)
(* laid out in sdk/rocketnet_xdp_frame.h, with the code's own arithmetic.  *)
(* RNXdpReplay.tla models the table as a map from address to MAC; this     *)
(* model carries the open addressing that map is made of.                  *)
(*                                                                         *)
(* The table: N entries (a power of two, the index mask N - 1), each with  *)
(* an address, a MAC, a learned stamp and a state: EMPTY (never used),     *)
(* LIVE (a learned client's MAC) or ANSWERED (the MAC replies to the       *)
(* address last went to; a forget turns a LIVE entry into one in place).   *)
(* Every probe sequence starts at the address's home slot (the FNV hash,   *)
(* rocketnet_xdp_reply_macs_slot, chosen here at Init from every map of    *)
(* addresses to slots, so every collision pattern is checked) and runs at  *)
(* most P slots, wrapping modulo N.                                        *)
(*                                                                         *)
(*   Find(a, s)  rocketnet_xdp_reply_macs_find_state: the first slot of    *)
(*               state s holding a, stopping at the first EMPTY slot       *)
(*   Learn       rocketnet_xdp_reply_macs_learn: the address's LIVE entry, *)
(*               else its ANSWERED one, else the first slot of the window  *)
(*               not LIVE, else the LIVE entry learned longest ago         *)
(*   Answer      rocketnet_xdp_reply_mac with a usable answered MAC and no *)
(*               learned entry: rocketnet_xdp_reply_macs_answer, the       *)
(*               address's ANSWERED entry, else the first EMPTY slot of    *)
(*               the window, else its ANSWERED entry answered longest ago, *)
(*               else not kept                                             *)
(*   Forget      rocketnet_xdp_reply_macs_forget: LIVE becomes ANSWERED    *)
(*   Reset       rocketnet_xdp_reply_macs_reset with no gateway            *)
(*                                                                         *)
(* The state ROCKETNET_XDP_REPLY_MAC_TOMBSTONE is written by no function,  *)
(* so no slot holds it; answer's test of it is never true.  Nothing ever   *)
(* returns a used slot to EMPTY but a reset of the whole table, so no      *)
(* entry lies past an EMPTY slot of its probe sequence.                    *)
(*                                                                         *)
(* The stamps are learn_count, which only grows; only their order matters, *)
(* and learn_count is above them all, so the VIEW keeps each used slot's   *)
(* rank among the used slots' stamps and leaves learn_count out: the       *)
(* behaviors from two states with the same ranks are the same.             *)
(* The ghost abs is the contract: what each address's lookup must return.  *)
(*                                                                         *)
(* Reversed witnesses (each flag FALSE is a real bug class):               *)
(*   SkipOtherEntries  a find stops at a forgotten (ANSWERED) slot of      *)
(*                     another address as if it were empty: the tombstone  *)
(*                     treated as empty                                    *)
(*   EvictOldest       learn evicts the LIVE entry learned last, not first *)
(*   InsertWindowExact learn's insert probes P + 1 slots, the lookups P    *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS
    N,                  \* ROCKETNET_XDP_REPLY_MAC_ENTRIES
    P,                  \* ROCKETNET_XDP_REPLY_MAC_PROBES
    Addrs,              \* client addresses
    Macs,               \* usable MACs
    SkipOtherEntries,   \* find_state goes on past a slot that is not EMPTY and not a match
    EvictOldest,        \* learn's eviction takes the smallest learned stamp
    InsertWindowExact,  \* learn's insert loop probes P slots, as the lookups do
    NOADDR              \* the address field of a never-used slot (a model value)

ASSUME N \in Nat \ {0} /\ P \in 1..N
ASSUME \A f \in {SkipOtherEntries, EvictOldest, InsertWindowExact} : f \in BOOLEAN

Slots == 0..(N - 1)
EMPTY == "empty"
LIVE == "live"
ANSWERED == "answered"
States == {EMPTY, LIVE, ANSWERED}
None == [ kind |-> "none" ]
Entry(kind, mac) == [ kind |-> kind, mac |-> mac ]

VARIABLES
    home,           \* home[a]: rocketnet_xdp_reply_macs_slot(a), fixed
    st,             \* st[s]: entries[s].state
    addr,           \* addr[s]: entries[s].address
    mac,            \* mac[s]: entries[s].mac
    stamp,          \* stamp[s]: entries[s].learned
    count,          \* learn_count
    abs,            \* ghost: abs[a], what a lookup of a must return
    badEviction     \* ghost: an eviction took an entry that was not the oldest of its kind in the window

vars == <<home, st, addr, mac, stamp, count, abs, badEviction>>

\* the i-th slot of a's probe sequence
Probe(a, i) == (home[a] + i) % N

\* rocketnet_xdp_reply_macs_find_state: the probe offset of the match, or -1
RECURSIVE FindFrom(_, _, _)
FindFrom(a, s, i) ==
    IF i >= P THEN -1
    ELSE LET x == Probe(a, i) IN
         IF st[x] = EMPTY THEN -1
         ELSE IF st[x] = s /\ addr[x] = a THEN x
         ELSE IF ~SkipOtherEntries /\ st[x] # LIVE /\ st[x] # s THEN -1
         ELSE FindFrom(a, s, i + 1)
Find(a, s) == FindFrom(a, s, 0)

\* what the code's lookups return for a: the learned MAC, else the answered one
Lookup(a) ==
    IF Find(a, LIVE) # -1 THEN Entry(LIVE, mac[Find(a, LIVE)])
    ELSE IF Find(a, ANSWERED) # -1 THEN Entry(ANSWERED, mac[Find(a, ANSWERED)])
    ELSE None

LearnWindow == IF InsertWindowExact THEN P ELSE P + 1

\* learn's insert: the first slot of the window that is not LIVE, else the
\* oldest LIVE one (the window is then all LIVE)
LearnFree(a) == { i \in 0..(LearnWindow - 1) : st[Probe(a, i)] # LIVE }
LearnSlot(a) ==
    IF LearnFree(a) # {}
    THEN Probe(a, CHOOSE i \in LearnFree(a) : \A j \in LearnFree(a) : i <= j)
    ELSE LET W == { Probe(a, i) : i \in 0..(LearnWindow - 1) }
         IN IF EvictOldest
            THEN CHOOSE x \in W : \A y \in W : stamp[x] <= stamp[y]
            ELSE CHOOSE x \in W : \A y \in W : stamp[x] >= stamp[y]

\* answer's insert: the first EMPTY slot of the window, else the oldest
\* ANSWERED one, else none (-1)
AnswerFree(a) == { i \in 0..(P - 1) : st[Probe(a, i)] = EMPTY }
AnswerOld(a) == { Probe(a, i) : i \in 0..(P - 1) } \cap { x \in Slots : st[x] = ANSWERED }
AnswerSlot(a) ==
    IF AnswerFree(a) # {}
    THEN Probe(a, CHOOSE i \in AnswerFree(a) : \A j \in AnswerFree(a) : i <= j)
    ELSE IF AnswerOld(a) # {}
    THEN CHOOSE x \in AnswerOld(a) : \A y \in AnswerOld(a) : stamp[x] <= stamp[y]
    ELSE -1

\* the contract after slot x is taken by a: the entry it held, if another
\* address's, is gone
Displace(x, a, new) ==
    [ b \in Addrs |-> IF b = a THEN new
                      ELSE IF st[x] # EMPTY /\ addr[x] = b THEN None
                      ELSE abs[b] ]

Init ==
    /\ home \in [ Addrs -> Slots ]
    /\ st = [ s \in Slots |-> EMPTY ]
    /\ addr = [ s \in Slots |-> NOADDR ]
    /\ mac = [ s \in Slots |-> CHOOSE m \in Macs : TRUE ]
    /\ stamp = [ s \in Slots |-> 0 ]
    /\ count = 0
    /\ abs = [ a \in Addrs |-> None ]
    /\ badEviction = FALSE

\* rocketnet_xdp_reply_macs_learn of a usable MAC
Learn(a, m) ==
    /\ count' = count + 1
    /\ LET x == IF Find(a, LIVE) # -1 THEN Find(a, LIVE)
                ELSE IF Find(a, ANSWERED) # -1 THEN Find(a, ANSWERED)
                ELSE LearnSlot(a)
           found == Find(a, LIVE) # -1 \/ Find(a, ANSWERED) # -1
           W == { Probe(a, i) : i \in 0..(LearnWindow - 1) }
           evicts == ~found /\ st[x] = LIVE
       IN /\ st' = [ st EXCEPT ![x] = LIVE ]
          /\ addr' = [ addr EXCEPT ![x] = a ]
          /\ mac' = [ mac EXCEPT ![x] = m ]
          /\ stamp' = [ stamp EXCEPT ![x] = count + 1 ]
          /\ abs' = IF found THEN [ abs EXCEPT ![a] = Entry(LIVE, m) ]
                    ELSE Displace(x, a, Entry(LIVE, m))
          /\ badEviction' = (badEviction \/
                              (evicts /\ \E y \in W : st[y] = LIVE /\ stamp[y] < stamp[x]))
    /\ UNCHANGED home

\* rocketnet_xdp_reply_mac with a usable answered MAC and no gateway: a
\* learned address is answered by its learned MAC and nothing is kept;
\* otherwise rocketnet_xdp_reply_macs_answer
Answer(a, m) ==
    /\ Find(a, LIVE) = -1
    /\ count' = count + 1
    /\ LET own == Find(a, ANSWERED)
           x == IF own # -1 THEN own ELSE AnswerSlot(a)
       IN IF x = -1
          THEN UNCHANGED <<st, addr, mac, stamp, abs, badEviction>>
          ELSE /\ st' = [ st EXCEPT ![x] = ANSWERED ]
               /\ addr' = [ addr EXCEPT ![x] = a ]
               /\ mac' = [ mac EXCEPT ![x] = m ]
               /\ stamp' = [ stamp EXCEPT ![x] = count + 1 ]
               /\ abs' = IF own # -1 THEN [ abs EXCEPT ![a] = Entry(ANSWERED, m) ]
                         ELSE Displace(x, a, Entry(ANSWERED, m))
               /\ badEviction' = (badEviction \/ st[x] = LIVE)
    /\ UNCHANGED home

\* rocketnet_xdp_reply_macs_forget
Forget(a) ==
    /\ LET x == Find(a, LIVE)
       IN IF x = -1
          THEN UNCHANGED <<st, abs>>
          ELSE /\ st' = [ st EXCEPT ![x] = ANSWERED ]
               /\ abs' = [ abs EXCEPT ![a] = Entry(ANSWERED, mac[x]) ]
    /\ UNCHANGED <<home, addr, mac, stamp, count, badEviction>>

\* rocketnet_xdp_reply_macs_reset with no gateway: the memset
Reset ==
    /\ st' = [ s \in Slots |-> EMPTY ]
    /\ addr' = [ s \in Slots |-> NOADDR ]
    /\ mac' = [ s \in Slots |-> CHOOSE m \in Macs : TRUE ]
    /\ stamp' = [ s \in Slots |-> 0 ]
    /\ count' = 0
    /\ abs' = [ a \in Addrs |-> None ]
    /\ UNCHANGED <<home, badEviction>>

Next ==
    \/ \E a \in Addrs, m \in Macs : Learn(a, m) \/ Answer(a, m)
    \/ \E a \in Addrs : Forget(a)
    \/ Reset

Spec == Init /\ [][Next]_vars

\* only the order of the used slots' stamps matters, and learn_count is above them all
Rank(x) == Cardinality({ y \in Slots : st[y] # EMPTY /\ stamp[y] < stamp[x] })
View ==
    << home, st, addr,
       [ s \in Slots |-> IF st[s] = EMPTY THEN "dead" ELSE mac[s] ],
       [ s \in Slots |-> IF st[s] = EMPTY THEN 0 ELSE Rank(s) ],
       abs, badEviction >>

----------------------------------------------------------------------------

TypeOK ==
    /\ home \in [ Addrs -> Slots ]
    /\ st \in [ Slots -> States ]
    /\ addr \in [ Slots -> Addrs \cup {NOADDR} ]
    /\ mac \in [ Slots -> Macs ]
    /\ count \in Nat

\* every lookup returns what the contract says: the learned MAC while the
\* address is learned, else the MAC its replies last went to, else none
TableIsMap == \A a \in Addrs : Lookup(a) = abs[a]

\* an address holds at most one used slot
OneEntryPerAddress ==
    \A x, y \in Slots : x # y /\ st[x] # EMPTY /\ st[y] # EMPTY => addr[x] # addr[y]

\* every used slot lies within P probes of its address's home, with no
\* EMPTY slot before it on the way
NoEntryPastEmpty ==
    \A x \in Slots : st[x] # EMPTY =>
        \E i \in 0..(P - 1) : /\ Probe(addr[x], i) = x
                              /\ \A j \in 0..(i - 1) : st[Probe(addr[x], j)] # EMPTY

\* an eviction takes only the entry of its kind longest unrenewed in the
\* window, and an answer never displaces a learned entry
EvictOnlyOldest == ~badEviction

=============================================================================
