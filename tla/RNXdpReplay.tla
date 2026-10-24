---------------------------- MODULE RNXdpReplay ----------------------------
(***************************************************************************)
(* rocketnet: kernel replay protection with a read-only high-water map.    *)
(*                                                                         *)
(* The XDP program (server_xdp/server_xdp.c) holds a BPF hash map,         *)
(* server_xdp_replay_map, keyed by the source address and port.  For the   *)
(* session packet types (keep-alive, payload, disconnect, ack, fragment)   *)
(* it looks the source up and, with an entry, drops a packet whose         *)
(* sequence is KernelWindow or more behind the entry's high_water; the     *)
(* design's KernelWindow is W, the userspace replay window.  The kernel    *)
(* never writes the map.  Userspace writes it once per server update: the  *)
(* transport walks its client slots, FORGETs the address of every slot     *)
(* that was seen connected and is no longer connected, then MARKs every    *)
(* connected slot whose session replay window's highest_opened is nonzero   *)
(* and differs from what it last marked; the transport applies the forgets, *)
(* then the marks, in one commit.                                           *)
(*                                                                         *)
(* The same forget drops the reply MAC the transport learned for that      *)
(* address.  A learned MAC outranks the gateway in the reply path, so a     *)
(* stale one would send the next client's replies to the old link-layer    *)
(* address.  The transport learns a reply MAC when it accepts a client,    *)
(* before that client opens a sequence, so a slot that never held a mark   *)
(* still holds one: the walk forgets every address it saw connected,       *)
(* marked or not.                                                           *)
(*                                                                         *)
(* A client slot is identified with its address here: one live session    *)
(* per address, a reconnect from the same address being a new session     *)
(* whose sequences restart at 1.  A slot whose mapping changes address is  *)
(* the same forget as a release, for the old address.                      *)
(*                                                                         *)
(* Sequences are naturals: the 32-bit wrap of (int32_t)(high_water -       *)
(* sequence) is left out, as are the byte layout of the key and value and  *)
(* the map's capacity.  W is the userspace replay window's depth           *)
(* (ROCKETNET_REPLAY_WINDOW_BITS).  UserOpen opens every genuine packet    *)
(* of the live session: a genuine packet the userspace window refuses is   *)
(* at or below highest_opened, so opening it would change nothing either.  *)
(* FreshGenuineNeverDropped is then the rule that the kernel drops only    *)
(* what the userspace window refuses.                                      *)
(*                                                                         *)
(* Session ids are ghosts: the kernel reads only high_water.  kmap[a].sess *)
(* names the session whose highest_opened the entry was taken from, so a   *)
(* drop can be charged to the mark that caused it.                         *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS
    Addresses,    \* the client source addresses (address and port)
    MaxSeq,       \* the highest sequence a client sends or a forger names
    W,            \* the replay window depth: ROCKETNET_REPLAY_WINDOW_BITS
    Sessions,     \* the number of sessions over the whole run
    MaxInFlight   \* the bound on packets in the network at once

ASSUME /\ Addresses # {}
       /\ MaxSeq \in Nat \ {0}
       /\ W \in Nat \ {0}
       /\ Sessions \in Nat \ {0}
       /\ MaxInFlight \in Nat \ {0}

(***************************************************************************)
(* Rule switches, definitions with the design's values, each overridden    *)
(* by one reversed witness configuration.                                  *)
(*   KernelAdvances: the kernel writes kmap[a] := seq on every pass (the   *)
(*     design: FALSE, the kernel only looks up).                           *)
(*   WalkForgets: the walk forgets the old address of a slot that held a   *)
(*     mark and is no longer connected (the design: TRUE).                 *)
(*   WalkForgetsUnmarked: the walk also forgets the reply MAC of a slot    *)
(*     that was seen connected and never held a mark (the design: TRUE).   *)
(*   KernelWindow: the distance behind the mark the kernel drops at (the   *)
(*     design: W, ROCKETNET_XDP_REPLAY_WINDOW, the userspace window).      *)
(***************************************************************************)
KernelAdvances == FALSE
WalkForgets    == TRUE
WalkForgetsUnmarked == TRUE
KernelWindow   == W

NoSession == 0
SessionIds == 1..Sessions
Seqs == 1..MaxSeq

(* A kernel map entry; high_water 0 is no entry: userspace marks only a    *)
(* nonzero highest_opened, and the kernel rule treats it as absent.        *)
NoEntry == [hw |-> 0, sess |-> NoSession]
Entries == [hw : 0..MaxSeq, sess : 0..Sessions]

Packets == [addr : Addresses, seq : Seqs, session : 0..Sessions,
            genuine : BOOLEAN, stage : {"wire", "user"}, copy : {1, 2}]

VARIABLES
    session,        \* session[a]: the live session id, or NoSession
    nextSession,    \* the id the next Connect takes
    sent,           \* sent[a]: highest genuine sequence the live session sent
    opened,         \* opened[a]: highest_opened of the live session (userspace)
    reached,        \* ghost: reached[s], the highest_opened session s reached
    marked,         \* marked[a]: what the transport last marked, 0 for none
    seen,           \* seen[a]: the walk saw the slot connected at a
    seenBeforeRelease, \* ghost: the walk had seen the slot when it was released
    replyMac,       \* replyMac[a]: the reply MAC the transport holds, 0 for none
    kmap,           \* kmap[a]: the kernel entry, NoEntry for none
    net,            \* in-flight packets: on the wire, or past the kernel
    pendingForget,  \* the addresses the walk forgets, applied by Commit
    pendingMark,    \* the marks the walk takes, applied by Commit
    pendingMacForget, \* the reply MACs the walk forgets, applied by Commit
    phase,          \* "run" (packet processing) or "flush" (walk done)
    settle,         \* settle[a]: "clear", "released" or "walked" since Release(a)
    dropped         \* ghost: genuine packets the kernel dropped

vars == <<session, nextSession, sent, opened, reached, marked, seen,
          seenBeforeRelease, replyMac, kmap, net, pendingForget, pendingMark,
          pendingMacForget, phase, settle, dropped>>

Drops == [addr : Addresses, seq : Seqs, session : 0..Sessions,
          against : 0..Sessions]

TypeOK ==
    /\ session \in [Addresses -> 0..Sessions]
    /\ nextSession \in 1..Sessions + 1
    /\ sent \in [Addresses -> 0..MaxSeq]
    /\ opened \in [Addresses -> 0..MaxSeq]
    /\ reached \in [SessionIds -> 0..MaxSeq]
    /\ marked \in [Addresses -> 0..MaxSeq]
    /\ seen \in [Addresses -> BOOLEAN]
    /\ seenBeforeRelease \in [Addresses -> BOOLEAN]
    /\ replyMac \in [Addresses -> 0..Sessions]
    /\ kmap \in [Addresses -> Entries]
    /\ net \subseteq Packets
    /\ pendingForget \subseteq Addresses
    /\ pendingMark \subseteq [addr : Addresses, hw : Seqs, sess : SessionIds]
    /\ pendingMacForget \subseteq Addresses
    /\ phase \in {"run", "flush"}
    /\ settle \in [Addresses -> {"clear", "released", "walked"}]
    /\ dropped \subseteq Drops

Init ==
    /\ session = [a \in Addresses |-> NoSession]
    /\ nextSession = 1
    /\ sent = [a \in Addresses |-> 0]
    /\ opened = [a \in Addresses |-> 0]
    /\ reached = [s \in SessionIds |-> 0]
    /\ marked = [a \in Addresses |-> 0]
    /\ seen = [a \in Addresses |-> FALSE]
    /\ seenBeforeRelease = [a \in Addresses |-> FALSE]
    /\ replyMac = [a \in Addresses |-> NoSession]
    /\ kmap = [a \in Addresses |-> NoEntry]
    /\ net = {}
    /\ pendingForget = {}
    /\ pendingMark = {}
    /\ pendingMacForget = {}
    /\ phase = "run"
    /\ settle = [a \in Addresses |-> "clear"]
    /\ dropped = {}

Max(x, y) == IF x >= y THEN x ELSE y

(***************************************************************************)
(* Connect(a): a new session from a; its sequences restart at 1 and its    *)
(* highest_opened is 0.  The transport learns the reply MAC of the address *)
(* when it accepts the client, so the entry is there from the first        *)
(* session, before any sequence is opened.  A userspace step, so only while *)
(* packets are being processed.  Enabled only once a Walk and a Commit     *)
(* have both run since the last Release(a): the assumption                  *)
(* HandshakeSpansAnUpdate, true because a slot exists only after a request, *)
(* a challenge and a response round trip while the walk runs every update.  *)
(***************************************************************************)
Connect(a) ==
    /\ phase = "run"
    /\ session[a] = NoSession
    /\ settle[a] = "clear"
    /\ nextSession <= Sessions
    /\ session' = [session EXCEPT ![a] = nextSession]
    /\ nextSession' = nextSession + 1
    /\ sent' = [sent EXCEPT ![a] = 0]
    /\ opened' = [opened EXCEPT ![a] = 0]
    /\ replyMac' = [replyMac EXCEPT ![a] = nextSession]
    /\ UNCHANGED <<reached, marked, seen, seenBeforeRelease, kmap, net,
                   pendingForget, pendingMark, pendingMacForget, phase,
                   settle, dropped>>

(* ClientSend(a): the live session sends its next genuine sequence.        *)
ClientSend(a) ==
    /\ session[a] # NoSession
    /\ sent[a] < MaxSeq
    /\ Cardinality(net) < MaxInFlight
    /\ sent' = [sent EXCEPT ![a] = sent[a] + 1]
    /\ net' = net \cup {[addr |-> a, seq |-> sent[a] + 1,
                         session |-> session[a], genuine |-> TRUE,
                         stage |-> "wire", copy |-> 1]}
    /\ UNCHANGED <<session, nextSession, opened, reached, marked, seen,
                   seenBeforeRelease, replyMac, kmap, pendingForget,
                   pendingMark, pendingMacForget, phase, settle, dropped>>

(* Forge(a, s): an attacker spoofing a sends any sequence; it never opens. *)
Forge(a, s) ==
    /\ Cardinality(net) < MaxInFlight
    /\ net' = net \cup {[addr |-> a, seq |-> s, session |-> session[a],
                         genuine |-> FALSE, stage |-> "wire", copy |-> 1]}
    /\ UNCHANGED <<session, nextSession, sent, opened, reached, marked, seen,
                   seenBeforeRelease, replyMac, kmap, pendingForget,
                   pendingMark, pendingMacForget, phase, settle, dropped>>

(* Duplicate(p): the network delivers a second copy of a packet.           *)
Duplicate(p) ==
    /\ p.stage = "wire"
    /\ p.copy = 1
    /\ Cardinality(net) < MaxInFlight
    /\ net' = net \cup {[p EXCEPT !.copy = 2]}
    /\ UNCHANGED <<session, nextSession, sent, opened, reached, marked, seen,
                   seenBeforeRelease, replyMac, kmap, pendingForget,
                   pendingMark, pendingMacForget, phase, settle, dropped>>

(***************************************************************************)
(* KernelFilter(p): THE KERNEL RULE.  With an entry for the source, drop   *)
(* when (high_water - sequence) >= KernelWindow, counting                  *)
(* DROPPED_REPLAYED; a genuine drop is recorded with the session of the    *)
(* mark it was dropped against.                                            *)
(* With no entry, or a sequence inside or ahead of the window, the packet  *)
(* goes on to userspace.  The kernel runs beside the update, so it is      *)
(* enabled in every phase.  The design never writes kmap here; the broken  *)
(* shape KernelAdvances writes kmap[a] := seq on every pass.               *)
(***************************************************************************)
KernelFilter(p) ==
    /\ p.stage = "wire"
    /\ LET e == kmap[p.addr]
       IN  IF e.hw # 0 /\ e.hw - p.seq >= KernelWindow
             THEN /\ net' = net \ {p}
                  /\ dropped' = IF p.genuine
                                  THEN dropped \cup {[addr |-> p.addr,
                                                      seq |-> p.seq,
                                                      session |-> p.session,
                                                      against |-> e.sess]}
                                  ELSE dropped
                  /\ UNCHANGED kmap
             ELSE /\ net' = (net \ {p}) \cup {[p EXCEPT !.stage = "user"]}
                  /\ kmap' = IF KernelAdvances
                               THEN [kmap EXCEPT ![p.addr] =
                                       [hw |-> p.seq, sess |-> p.session]]
                               ELSE kmap
                  /\ UNCHANGED dropped
    /\ UNCHANGED <<session, nextSession, sent, opened, reached, marked,
                   seen, seenBeforeRelease, replyMac, pendingForget,
                   pendingMark, pendingMacForget, phase, settle>>

(***************************************************************************)
(* UserOpen(p): userspace receives a packet the kernel passed.  A genuine  *)
(* packet of the live session opens and raises highest_opened; a forged    *)
(* one fails the AEAD open, and a packet of an ended session has no        *)
(* mapping: neither moves anything.                                        *)
(***************************************************************************)
UserOpen(p) ==
    /\ phase = "run"
    /\ p.stage = "user"
    /\ net' = net \ {p}
    /\ IF p.genuine /\ p.session # NoSession /\ p.session = session[p.addr]
         THEN /\ opened' = [opened EXCEPT ![p.addr] = Max(@, p.seq)]
              /\ reached' = [reached EXCEPT ![p.session] = Max(@, p.seq)]
         ELSE UNCHANGED <<opened, reached>>
    /\ UNCHANGED <<session, nextSession, sent, marked, kmap, pendingForget,
                   pendingMark, pendingMacForget, phase, settle, dropped,
                   seen, seenBeforeRelease, replyMac>>

(* Release(a): the session ends (timeout, disconnect).  Userspace.  The     *)
(* reply MAC the transport learned for the address stays until the walk    *)
(* forgets it; the ghost records whether the walk had seen the slot, which *)
(* is what tells the walk that there is something to forget.               *)
Release(a) ==
    /\ phase = "run"
    /\ session[a] # NoSession
    /\ session' = [session EXCEPT ![a] = NoSession]
    /\ sent' = [sent EXCEPT ![a] = 0]
    /\ opened' = [opened EXCEPT ![a] = 0]
    /\ settle' = [settle EXCEPT ![a] = "released"]
    /\ seenBeforeRelease' = [seenBeforeRelease EXCEPT ![a] = seen[a]]
    /\ UNCHANGED <<nextSession, reached, marked, seen, replyMac, kmap, net,
                   pendingForget, pendingMark, pendingMacForget, phase,
                   dropped>>

(***************************************************************************)
(* Walk: once per update, after packet processing and before the flush.   *)
(* First the forgets: every slot that held a mark and is no longer         *)
(* connected.  Then the marks: every connected slot whose highest_opened   *)
(* is nonzero and differs from what it last marked.  The walk also records *)
(* every connected slot as seen at its address, and forgets the reply MAC  *)
(* of every address it saw connected and that is no longer connected: a    *)
(* slot that never held a mark still holds the MAC the transport learned   *)
(* when it accepted the client.  The broken shape WalkForgets = FALSE     *)
(* never forgets a released slot; the broken shape WalkForgetsUnmarked =   *)
(* FALSE forgets the reply MAC of the marked slots only.                  *)
(***************************************************************************)
Forgets == IF WalkForgets
             THEN {a \in Addresses : marked[a] # 0 /\ session[a] = NoSession}
             ELSE {}

MacForgets == IF WalkForgetsUnmarked
               THEN {a \in Addresses : seen[a] /\ session[a] = NoSession}
               ELSE {a \in Addresses : marked[a] # 0 /\ session[a] = NoSession}

Marks == {[addr |-> a, hw |-> opened[a], sess |-> session[a]] :
            a \in {b \in Addresses : /\ session[b] # NoSession
                                     /\ opened[b] # 0
                                     /\ opened[b] # marked[b]}}

Walk ==
    /\ phase = "run"
    /\ pendingForget' = Forgets
    /\ pendingMark' = Marks
    /\ pendingMacForget' = MacForgets
    /\ marked' = [a \in Addresses |->
                    IF \E m \in Marks : m.addr = a THEN opened[a]
                    ELSE IF a \in Forgets THEN 0
                    ELSE marked[a]]
    /\ seen' = [a \in Addresses |-> session[a] # NoSession]
    /\ settle' = [a \in Addresses |->
                    IF settle[a] = "released" THEN "walked" ELSE settle[a]]
    /\ phase' = "flush"
    /\ UNCHANGED <<session, nextSession, sent, opened, reached, replyMac, kmap,
                   net, seenBeforeRelease, dropped>>

(* Commit: the transport applies all forgets, then all marks, in one commit.  *)
(* The only action that writes kmap in the design.                         *)
Commit ==
    /\ phase = "flush"
    /\ LET afterForget == [a \in Addresses |->
                             IF a \in pendingForget THEN NoEntry ELSE kmap[a]]
       IN  kmap' = [a \in Addresses |->
                      IF \E m \in pendingMark : m.addr = a
                        THEN LET m == CHOOSE m \in pendingMark : m.addr = a
                             IN  [hw |-> m.hw, sess |-> m.sess]
                        ELSE afterForget[a]]
    /\ replyMac' = [a \in Addresses |->
                     IF a \in pendingMacForget THEN NoSession ELSE replyMac[a]]
    /\ pendingForget' = {}
    /\ pendingMark' = {}
    /\ pendingMacForget' = {}
    /\ settle' = [a \in Addresses |->
                    IF settle[a] = "walked" THEN "clear" ELSE settle[a]]
    /\ phase' = "run"
    /\ UNCHANGED <<session, nextSession, sent, opened, reached, marked, seen,
                   seenBeforeRelease, net, dropped>>

Next ==
    \/ \E a \in Addresses : Connect(a) \/ ClientSend(a) \/ Release(a)
    \/ \E a \in Addresses, s \in Seqs : Forge(a, s)
    \/ \E p \in net : Duplicate(p) \/ KernelFilter(p) \/ UserOpen(p)
    \/ Walk
    \/ Commit

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* Properties.                                                             *)
(***************************************************************************)

(* KernelNeverWrites: every action but Commit leaves kmap unchanged.       *)
KernelNeverWrites == [][Commit \/ kmap' = kmap]_vars

(* MarkNeverAhead: with a live session, the entry for its address is none, *)
(* or was taken from that session and is no more than its highest_opened. *)
(* With no live session the forget may be pending: the map lags the truth *)
(* by at most one update.                                                  *)
MarkNeverAhead ==
    \A a \in Addresses :
        \/ kmap[a].hw = 0
        \/ session[a] = NoSession
        \/ /\ kmap[a].sess = session[a]
           /\ kmap[a].hw <= opened[a]

(* FreshGenuineNeverDropped: a genuine packet of the live session dropped *)
(* against a mark of its own session is never fresh: its sequence is at    *)
(* most opened[a] - W, so the userspace window, W deep, refuses it too.    *)
(* A reordered packet the userspace window accepts is never dropped.       *)
FreshGenuineNeverDropped ==
    \A d \in dropped :
        (d.session # NoSession /\ d.session = session[d.addr]
                               /\ d.against = d.session)
            => d.seq <= opened[d.addr] - W

(* NewSessionNeverDroppedByOldMark: no genuine packet of a session is      *)
(* dropped against a mark taken in an earlier session.                     *)
NewSessionNeverDroppedByOldMark ==
    \A d \in dropped : ~(d.against # NoSession /\ d.against < d.session)

(* MarksTakenFromTheTruth: every entry was taken from a highest_opened its *)
(* session reached.  Checked beside MarkNeverAhead.                        *)
MarksTakenFromTheTruth ==
    \A a \in Addresses :
        kmap[a].hw = 0 \/ (kmap[a].sess \in SessionIds
                           /\ kmap[a].hw <= reached[kmap[a].sess])

(***************************************************************************)
(* ReleasedSlotForgetsItsReplyMac: a slot the walk saw connected and that  *)
(* then left is forgotten by the next walk, marked or not, and its reply   *)
(* MAC is gone once the commit applies that forget.  A slot the walk never *)
(* saw (accepted and released inside one update) is outside the walk's    *)
(* reach: the transport's own release path is what forgets that one.       *)
(***************************************************************************)
ReleasedSlotForgetsItsReplyMac ==
    \A a \in Addresses :
        /\ (seenBeforeRelease[a] /\ settle[a] = "walked")
            => a \in pendingMacForget
        /\ (seenBeforeRelease[a] /\ settle[a] = "clear"
            /\ session[a] = NoSession)
            => replyMac[a] = NoSession
=============================================================================
