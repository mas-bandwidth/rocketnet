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
(* slot whose session replay window's highest_opened differs from what it   *)
(* last marked (none counting as zero), and every slot it first sees, zero *)
(* included; the transport applies the forgets, then                       *)
(* the marks, in one commit.  The kernel may refuse a forget; the refused  *)
(* forget stays pending and the next commit applies it first, again.       *)
(* The same accepted forget drops the reply MAC learned for that address; *)
(* slots are remembered as seen even before they acquire a replay mark.   *)
(*                                                                         *)
(* A client slot is identified with its address here: one live session    *)
(* per address, a reconnect from the same address being a new session     *)
(* whose sequences restart at 1.  A slot whose mapping changes address is  *)
(* the same forget as a release, for the old address.                      *)
(*                                                                         *)
(* A reconnect may come before the old session's forget is queued or       *)
(* while the kernel refuses it.  The new session's client sends no session *)
(* packet until the server's first keep-alive reaches it, and the server   *)
(* publishes that keep-alive after the commit of the update that connected *)
(* it (sdk/rocketnet_server_transport.cpp, the replay walk and commit come *)
(* before the update's send flush): greeted[a].  That commit carries the   *)
(* new session's first mark, zero when nothing has opened, which replaces  *)
(* whatever the kernel held for the address; a high_water of 0 drops       *)
(* nothing.  A disconnect burst a client sends before its first keep-alive *)
(* is left out: a dropped one costs the server a timeout, not the session. *)
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
    MaxInFlight,  \* the bound on packets in the network at once
    ForgetCapacity \* fixed pending-forget entries, deduplicated by address

ASSUME /\ Addresses # {}
       /\ MaxSeq \in Nat \ {0}
       /\ W \in Nat \ {0}
       /\ Sessions \in Nat \ {0}
       /\ MaxInFlight \in Nat \ {0}
       /\ ForgetCapacity \in Nat

(***************************************************************************)
(* Rule switches, definitions with the design's values, each overridden    *)
(* by one reversed witness configuration.                                  *)
(*   KernelAdvances: the kernel writes kmap[a] := seq on every pass (the   *)
(*     design: FALSE, the kernel only looks up).                           *)
(*   WalkForgets: the walk forgets the old address of a slot that held a   *)
(*     mark and is no longer connected (the design: TRUE).                 *)
(*   WalkForgetsUnmarked: the walk also forgets a released slot that was   *)
(*     seen connected before it acquired a mark (the design: TRUE).        *)
(*   KernelWindow: the distance behind the mark the kernel drops at (the   *)
(*     design: W, ROCKETNET_XDP_REPLAY_WINDOW, the userspace window).      *)
(*   KeepFailedForgets: a forget the kernel refused stays pending for the  *)
(*     next commit (the design: TRUE).                                     *)
(*   MarkNewSessions: the walk marks a slot it first sees and a slot whose *)
(*     highest_opened differs from its mark, zero included, so a new       *)
(*     session's first commit replaces an older session's entry (the       *)
(*     design: TRUE; FALSE marks only a nonzero highest_opened).           *)
(*   KeepRefusedMarks: a mark the kernel refused stays pending for the     *)
(*     next commit, and a newer mark of the same address replaces it (the  *)
(*     design: TRUE; the walk's cache has already advanced, so nothing     *)
(*     else would take it again).                                          *)
(*   ForgetDropsKeptMark: a forget the walk queues drops a kept mark of    *)
(*     the same address: the mark was taken before the slot was released,  *)
(*     and the commit applies forgets before marks, so kept it would       *)
(*     rewrite the old session's entry after the forget (the design: TRUE).*)
(*   SupersedeStaleForgets: a refused forget whose address a mark applied  *)
(*     in the same commit is dropped, not kept: the mark was taken after   *)
(*     the forget was queued, so the entry is a newer session's and the   *)
(*     stale forget no longer owns it (the design: TRUE).                  *)
(* ForgetFailureBudget bounds the refused forgets over a run, so the       *)
(* state space stays finite; one is a refused forget, kept, then landed.   *)
(* The control raises it to two (MCRNXdpReplay.cfg), so a kept forget can  *)
(* be refused again in the commit that lands a new session's mark.         *)
(* MarkFailureBudget bounds the refused marks the same way: 0 in the       *)
(* design's controls, raised by MCRNXdpReplayRefusedMark.cfg.              *)
(***************************************************************************)
KernelAdvances    == FALSE
WalkForgets       == TRUE
WalkForgetsUnmarked == TRUE
KernelWindow      == W
KeepFailedForgets == TRUE
SupersedeStaleForgets == TRUE
MarkNewSessions   == TRUE
KeepRefusedMarks  == TRUE
ForgetDropsKeptMark == TRUE

ForgetFailureBudget == 1
MarkFailureBudget == 0

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
    seenBeforeRelease, \* ghost: the walk had seen it before Release
    replyMac,       \* replyMac[a]: learned reply MAC, 0 for none
    kmap,           \* kmap[a]: the kernel entry, NoEntry for none
    net,            \* in-flight packets: on the wire, or past the kernel
    pendingForget,  \* the addresses the walk forgets, applied by Commit
    pendingMark,    \* the marks the walk takes, applied by Commit
    pendingMacForget, \* accepted reply-MAC forgets, applied by Commit
    phase,          \* "run" (packet processing) or "flush" (walk done)
    dropped,        \* ghost: genuine packets the kernel dropped
    forgetFailures, \* the forgets the kernel refused so far, at most the budget
    greeted,        \* greeted[a]: the live session's first keep-alive has left
    markFailures    \* the marks the kernel refused so far, at most the budget

vars == <<session, nextSession, sent, opened, reached, marked, seen,
          seenBeforeRelease, replyMac, kmap, net, pendingForget,
          pendingMark, pendingMacForget, phase, dropped, forgetFailures,
          greeted, markFailures>>

Drops == [addr : Addresses, seq : Seqs, session : 0..Sessions,
          against : 0..Sessions, owed : BOOLEAN]

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
    /\ Cardinality(pendingForget) <= ForgetCapacity
    /\ pendingMark \subseteq [addr : Addresses, hw : 0..MaxSeq, sess : SessionIds]
    /\ pendingMacForget \subseteq Addresses
    /\ phase \in {"run", "flush"}
    /\ dropped \subseteq Drops
    /\ forgetFailures \in 0..ForgetFailureBudget
    /\ greeted \in [Addresses -> BOOLEAN]
    /\ markFailures \in 0..MarkFailureBudget

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
    /\ dropped = {}
    /\ forgetFailures = 0
    /\ greeted = [a \in Addresses |-> FALSE]
    /\ markFailures = 0

Max(x, y) == IF x >= y THEN x ELSE y

(***************************************************************************)
(* Connect(a): a new session from a; its sequences restart at 1 and its    *)
(* highest_opened is 0.  A userspace step, so only while packets are being *)
(* processed. A reconnect may arrive before the old forget is queued or    *)
(* while a retained delete is refused: there is no settle guard here, and  *)
(* the kernel map may still hold the old session's entry for a.  The new   *)
(* session is not greeted until the next Commit.                           *)
(***************************************************************************)
Connect(a) ==
    /\ phase = "run"
    /\ session[a] = NoSession
    /\ nextSession <= Sessions
    /\ session' = [session EXCEPT ![a] = nextSession]
    /\ nextSession' = nextSession + 1
    /\ sent' = [sent EXCEPT ![a] = 0]
    /\ opened' = [opened EXCEPT ![a] = 0]
    /\ replyMac' = [replyMac EXCEPT ![a] = nextSession]
    /\ greeted' = [greeted EXCEPT ![a] = FALSE]
    /\ UNCHANGED <<reached, marked, seen, seenBeforeRelease, kmap, net,
                   pendingForget, pendingMark, pendingMacForget, phase,
                   dropped, forgetFailures, markFailures>>

(* ClientSend(a): the live session sends its next genuine sequence, once   *)
(* its first keep-alive has reached it (the client's connected state).     *)
ClientSend(a) ==
    /\ session[a] # NoSession
    /\ greeted[a]
    /\ sent[a] < MaxSeq
    /\ Cardinality(net) < MaxInFlight
    /\ sent' = [sent EXCEPT ![a] = sent[a] + 1]
    /\ net' = net \cup {[addr |-> a, seq |-> sent[a] + 1,
                         session |-> session[a], genuine |-> TRUE,
                         stage |-> "wire", copy |-> 1]}
    /\ UNCHANGED <<session, nextSession, opened, reached, marked, seen,
                   seenBeforeRelease, replyMac, kmap, pendingForget,
                   pendingMark, pendingMacForget, phase, dropped,
                   forgetFailures, greeted, markFailures>>

(* Forge(a, s): an attacker spoofing a sends any sequence; it never opens. *)
Forge(a, s) ==
    /\ Cardinality(net) < MaxInFlight
    /\ net' = net \cup {[addr |-> a, seq |-> s, session |-> session[a],
                         genuine |-> FALSE, stage |-> "wire", copy |-> 1]}
    /\ UNCHANGED <<session, nextSession, sent, opened, reached, marked, seen,
                   seenBeforeRelease, replyMac, kmap, pendingForget,
                   pendingMark, pendingMacForget, phase, dropped,
                   forgetFailures, greeted, markFailures>>

(* Duplicate(p): the network delivers a second copy of a packet.           *)
Duplicate(p) ==
    /\ p.stage = "wire"
    /\ p.copy = 1
    /\ Cardinality(net) < MaxInFlight
    /\ net' = net \cup {[p EXCEPT !.copy = 2]}
    /\ UNCHANGED <<session, nextSession, sent, opened, reached, marked, seen,
                   seenBeforeRelease, replyMac, kmap, pendingForget,
                   pendingMark, pendingMacForget, phase, dropped,
                   forgetFailures, greeted, markFailures>>

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
                                                      against |-> e.sess,
                                                      owed |-> \E m \in pendingMark :
                                                                 m.addr = p.addr /\ m.sess = p.session]}
                                  ELSE dropped
                  /\ UNCHANGED kmap
             ELSE /\ net' = (net \ {p}) \cup {[p EXCEPT !.stage = "user"]}
                  /\ kmap' = IF KernelAdvances
                               THEN [kmap EXCEPT ![p.addr] =
                                       [hw |-> p.seq, sess |-> p.session]]
                               ELSE kmap
                  /\ UNCHANGED dropped
    /\ UNCHANGED <<session, nextSession, sent, opened, reached, marked, seen,
                   seenBeforeRelease, replyMac, pendingForget, pendingMark,
                   pendingMacForget, phase, forgetFailures, greeted, markFailures>>

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
    /\ UNCHANGED <<session, nextSession, sent, marked, seen,
                   seenBeforeRelease, replyMac, kmap, pendingForget,
                   pendingMark, pendingMacForget, phase, dropped,
                   forgetFailures, greeted, markFailures>>

(* Release(a): the session ends (timeout, disconnect).  Userspace.         *)
Release(a) ==
    /\ phase = "run"
    /\ session[a] # NoSession
    /\ session' = [session EXCEPT ![a] = NoSession]
    /\ sent' = [sent EXCEPT ![a] = 0]
    /\ opened' = [opened EXCEPT ![a] = 0]
    /\ seenBeforeRelease' = [seenBeforeRelease EXCEPT ![a] = seen[a]]
    /\ greeted' = [greeted EXCEPT ![a] = FALSE]
    /\ UNCHANGED <<nextSession, reached, marked, seen, replyMac, kmap, net,
                   pendingForget, pendingMark, pendingMacForget, phase,
                   dropped, forgetFailures, markFailures>>

(***************************************************************************)
(* Walk: once per update, after packet processing and before the flush.   *)
(* First the forgets: every slot that held a mark and is no longer         *)
(* connected.  Then the marks: every connected slot whose highest_opened   *)
(* differs from what it last marked (0 for none), and every connected slot *)
(* not yet seen, zero included (MarkNewSessions).  A reconnect at a before *)
(* the walk saw the release still has the old session's marked[a], and    *)
(* its highest_opened is 0 until it is greeted, so it is marked at 0.  The *)
(* broken shape WalkForgets = FALSE never forgets a released slot; the     *)
(* broken shape MarkNewSessions = FALSE marks only a nonzero value.        *)
(***************************************************************************)
Forgets == IF WalkForgets
             THEN IF WalkForgetsUnmarked
                    THEN {a \in Addresses : seen[a] /\ session[a] = NoSession}
                    ELSE {a \in Addresses : marked[a] # 0 /\ session[a] = NoSession}
             ELSE {}

MarkTaken(b) ==
    IF MarkNewSessions
      THEN opened[b] # marked[b] \/ ~seen[b]
      ELSE opened[b] # 0 /\ opened[b] # marked[b]

Marks == {[addr |-> a, hw |-> opened[a], sess |-> session[a]] :
            a \in {b \in Addresses : /\ session[b] # NoSession
                                     /\ MarkTaken(b)}}

AdmittedNewForgets == {accepted \in SUBSET (Forgets \ pendingForget) :
    /\ Cardinality(pendingForget \cup accepted) <= ForgetCapacity
    /\ \/ accepted = (Forgets \ pendingForget)
       \/ Cardinality(pendingForget \cup accepted) = ForgetCapacity}

Walk ==
    /\ phase = "run"
    /\ \E accepted \in AdmittedNewForgets :
        /\ pendingForget' = pendingForget \cup accepted
        /\ pendingMark' = Marks \cup
               {m \in pendingMark : /\ \A k \in Marks : k.addr # m.addr
                                    /\ ForgetDropsKeptMark => m.addr \notin accepted}
        /\ pendingMacForget' = accepted
        /\ marked' = [a \in Addresses |->
                        IF \E m \in Marks : m.addr = a THEN opened[a]
                        ELSE IF a \in accepted THEN 0
                        ELSE marked[a]]
        /\ seen' = [a \in Addresses |->
                      IF a \in accepted THEN FALSE
                      ELSE IF session[a] # NoSession THEN TRUE
                      ELSE seen[a]]
        /\ phase' = "flush"
        /\ UNCHANGED <<session, nextSession, sent, opened, reached,
                       seenBeforeRelease, replyMac, kmap, net, dropped,
                       forgetFailures, greeted, markFailures>>

(* Commit: the transport applies all forgets, then all marks, in one commit.  *)
(* The only action that writes kmap in the design.  The kernel may refuse  *)
(* any set of the forgets (failed): a refused forget leaves its entry, and *)
(* the design keeps it pending for the next commit, whose forgets run      *)
(* before its marks, unless a mark of the same address was applied in this *)
(* commit: that entry is a newer session's, and the stale forget is        *)
(* superseded (rocketnet_xdp_replay_marks_reset_keeping's mark_applied).   *)
(* The kernel may refuse any set of the marks too (refused): a refused     *)
(* mark leaves the entry as it was and stays pending (KeepRefusedMarks) for *)
(* the next commit, until a newer mark of its address or a forget the walk *)
(* queues for it replaces it.  Only an applied mark supersedes a refused   *)
(* forget.  A mark of 0 is an entry that drops nothing.  The update's send *)
(* flush follows the commit whatever the kernel did, so every live session *)
(* is greeted.                                                             *)
(* The broken shape KeepFailedForgets = FALSE drops it; the broken shape   *)
(* SupersedeStaleForgets = FALSE keeps it over the newer mark.             *)
Commit ==
    /\ phase = "flush"
    /\ \E failed \in SUBSET pendingForget, refused \in SUBSET pendingMark :
        /\ forgetFailures + Cardinality(failed) <= ForgetFailureBudget
        /\ markFailures + Cardinality(refused) <= MarkFailureBudget
        /\ LET applied == pendingMark \ refused
               afterForget == [a \in Addresses |->
                                 IF a \in pendingForget \ failed
                                   THEN NoEntry ELSE kmap[a]]
           IN  /\ kmap' = [a \in Addresses |->
                             IF \E m \in applied : m.addr = a
                               THEN LET m == CHOOSE m \in applied : m.addr = a
                                    IN  [hw |-> m.hw, sess |-> m.sess]
                               ELSE afterForget[a]]
               /\ pendingForget' =
                      IF ~KeepFailedForgets THEN {}
                      ELSE IF SupersedeStaleForgets
                             THEN failed \ {m.addr : m \in applied}
                             ELSE failed
        /\ forgetFailures' = forgetFailures + Cardinality(failed)
        /\ markFailures' = markFailures + Cardinality(refused)
        /\ pendingMark' = IF KeepRefusedMarks THEN refused ELSE {}
        /\ replyMac' = [a \in Addresses |->
                          IF a \in pendingMacForget
                            THEN NoSession ELSE replyMac[a]]
    /\ pendingMacForget' = {}
    /\ greeted' = [a \in Addresses |-> session[a] # NoSession]
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

(* ForgetOwedUntilApplied: an entry left for an address with no live       *)
(* session is always owed a forget, either still to be taken by the walk   *)
(* (marked) or pending, kept across every commit the kernel refused it,    *)
(* so no stale entry outlives a released slot by more than the commits it  *)
(* took the forget to succeed.                                             *)
ForgetOwedUntilApplied ==
    \A a \in Addresses :
        (kmap[a].hw # 0 /\ session[a] = NoSession)
            => (marked[a] # 0 \/ a \in pendingForget)

(* RetryNeverDeletesNewerMark: a pending forget is never held over the    *)
(* entry of the live session at its address.  Commit applies a pending     *)
(* forget before any mark, so such a forget, once the kernel accepts it,   *)
(* would delete the newer session's mark; the walk's cache already holds   *)
(* that high-water, so nothing would mark it again until a fresh sequence. *)
RetryNeverDeletesNewerMark ==
    \A a \in pendingForget :
        ~(/\ session[a] # NoSession
          /\ kmap[a].hw # 0
          /\ kmap[a].sess = session[a])

(* MarkNeverAhead: with a live session, the entry for its address is none, *)
(* or was taken from that session and is no more than its highest_opened, *)
(* or the session is not yet greeted and its first mark is owed: still to  *)
(* be taken by the walk, or pending for the commit before its first        *)
(* keep-alive.  With no live session the forget is owed                    *)
(* (ForgetOwedUntilApplied): the map lags the truth by one update, or by   *)
(* the commits a refused forget took to land.                              *)
MarkNeverAhead ==
    /\ ForgetOwedUntilApplied
    /\ \A a \in Addresses :
        \/ kmap[a].hw = 0
        \/ session[a] = NoSession
        \/ /\ kmap[a].sess = session[a]
           /\ kmap[a].hw <= opened[a]
        \/ \E m \in pendingMark : m.addr = a /\ m.sess = session[a]
        \/ /\ ~greeted[a]
           /\ \/ opened[a] # marked[a]
              \/ ~seen[a]

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
(* dropped against a mark taken in an earlier session, whether or not the  *)
(* earlier session's forget had been queued or accepted.                   *)
NewSessionNeverDroppedByOldMark ==
    \A d \in dropped : ~(d.against # NoSession /\ d.against < d.session)

(* MarkOwedUntilApplied: a greeted live session's entry is the walk's     *)
(* last mark for it (an entry of 0 drops nothing, whichever session it     *)
(* came from), or that mark is still pending, kept across every commit the *)
(* kernel refused it, so a refused mark is retried until it is applied.    *)
MarkOwedUntilApplied ==
    \A a \in Addresses :
        (greeted[a] /\ phase = "run")
            => \/ /\ kmap[a].hw = marked[a]
                  /\ (kmap[a].hw = 0 \/ kmap[a].sess = session[a])
               \/ \E m \in pendingMark : /\ m.addr = a
                                         /\ m.sess = session[a]
                                         /\ m.hw = marked[a]

(* NewSessionDroppedOnlyWhileMarkOwed: with marks the kernel may refuse, a *)
(* genuine packet of a session is dropped against an earlier session's     *)
(* mark only while a refused mark of its own session is kept for the next  *)
(* commit: the client was greeted after a commit whose mark the kernel     *)
(* refused, and the retry ends the window (MarkOwedUntilApplied).          *)
NewSessionDroppedOnlyWhileMarkOwed ==
    \A d \in dropped : (d.against # NoSession /\ d.against < d.session) => d.owed

(* MarksTakenFromTheTruth: every entry was taken from a highest_opened its *)
(* session reached.  Checked beside MarkNeverAhead.                        *)
MarksTakenFromTheTruth ==
    \A a \in Addresses :
        kmap[a].hw = 0 \/ (kmap[a].sess \in SessionIds
                           /\ kmap[a].hw <= reached[kmap[a].sess])

ReleasedSlotForgetsItsReplyMac ==
    \A a \in Addresses :
        /\ (seenBeforeRelease[a] /\ session[a] = NoSession /\ phase = "flush"
            /\ replyMac[a] # NoSession)
            => (a \in pendingMacForget
                \/ Cardinality(pendingForget) = ForgetCapacity)
        /\ (seenBeforeRelease[a] /\ session[a] = NoSession /\ phase = "run"
            /\ ~seen[a])
            => replyMac[a] = NoSession
=============================================================================
