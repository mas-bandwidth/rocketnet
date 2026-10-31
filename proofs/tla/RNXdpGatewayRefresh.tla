------------------------ MODULE RNXdpGatewayRefresh ------------------------
(***************************************************************************)
(* The XDP server's gateway refresh (sdk/rocketnet_xdp_frame.h GATEWAY     *)
(* REFRESH, rocketnet_xdp_gateway_refresh; sdk/rocketnet_server_socket_    *)
(* linux.cpp rocketnet_server_socket_refresh_gateway) and the handoff of   *)
(* the gateway MAC it writes to the fill threads that stage replies.       *)
(*                                                                         *)
(* A MAC is six bytes and no store or load of it is atomic, so the model   *)
(* splits it into two halves, written and read one at a time. The main     *)
(* thread alternates between the receive call, where the refresh runs,     *)
(* and the fill phase, where game threads stage packets through start and  *)
(* finish, which copy the table's gateway MAC into each packet; the fills  *)
(* are joined before the next update starts, and the clock advances once   *)
(* per update. The kernel's neighbour entry for the gateway (what the      *)
(* resolve reads) changes at any moment, at most MaxChanges times. The     *)
(* xdp threads send the staged copies and never read the table, so the     *)
(* staged set is what goes on the wire.                                    *)
(*                                                                         *)
(* Actions:                                                                *)
(*   Join          fills joined; the next update starts, the clock ticks   *)
(*   Refresh       the receive call's refresh: schedules on its first      *)
(*                 call, then at or past the deadline resolves, sets the   *)
(*                 next deadline and writes the first half of a changed    *)
(*                 MAC                                                     *)
(*   WriteLow      the refresh writes the second half                      *)
(*   ReadHigh      a fill copies the first half of the table's MAC         *)
(*   ReadLow       the fill copies the second half and stages the packet   *)
(*   ChangeGateway the router is replaced (environment)                    *)
(*                                                                         *)
(* Invariants: NoTornMac (every staged MAC is one some resolve gave, never *)
(* a mix of two) and FreshAfterPeriod (a packet staged Period or more     *)
(* ticks after the last change carries the new MAC). The first receive     *)
(* only schedules, at the tick create resolved in.                         *)
(*                                                                         *)
(* Rule switches for the reversed witnesses:                               *)
(*   RefreshInFill  the refresh runs during the fill phase, as on a worker *)
(*                  or a timer thread, and its writes interleave with the  *)
(*                  fills' reads: NoTornMac fails                          *)
(*   RefreshNever   the gateway is resolved at create only, the defect     *)
(*                  before the refresh: FreshAfterPeriod fails             *)
(***************************************************************************)
EXTENDS Naturals

CONSTANTS Period, MaxTime, MaxChanges, RefreshInFill, RefreshNever

Halves == {1, 2}
Macs == {<<1, 1>>, <<2, 2>>}
Nil == 0

VARIABLES
    phase,          \* "receive", "writelow" (the refresh's second store) or "fill"
    now,            \* the transport clock, one tick per update
    scheduled,      \* the refresh has set its first deadline
    nextTime,       \* the refresh deadline
    table,          \* the table's gateway MAC, two halves
    pendingLow,     \* the second half the refresh has yet to store, Nil for none
    resolver,       \* the kernel's neighbour entry for the gateway
    changedAt,      \* the clock at the last change
    changes,        \* changes so far
    readHigh,       \* the half a fill has copied, Nil when no fill is mid-copy
    staged          \* [mac, time] of every staged packet

vars == << phase, now, scheduled, nextTime, table, pendingLow, resolver, changedAt, changes, readHigh, staged >>

TypeOK ==
    /\ phase \in {"receive", "writelow", "fill"}
    /\ now \in 0..MaxTime
    /\ scheduled \in BOOLEAN
    /\ nextTime \in 0..(MaxTime + Period)
    /\ table \in Halves \X Halves
    /\ pendingLow \in Halves \cup {Nil}
    /\ resolver \in Macs
    /\ changedAt \in 0..MaxTime
    /\ changes \in 0..MaxChanges
    /\ readHigh \in Halves \cup {Nil}
    /\ staged \subseteq [mac : Halves \X Halves, time : 0..MaxTime]

\* create resolved the gateway; the first update's receive comes next
Init ==
    /\ phase = "receive"
    /\ now = 0
    /\ scheduled = FALSE
    /\ nextTime = 0
    /\ table = <<1, 1>>
    /\ pendingLow = Nil
    /\ resolver = <<1, 1>>
    /\ changedAt = 0
    /\ changes = 0
    /\ readHigh = Nil
    /\ staged = {}

\* the phase in which the refresh's stores run
RefreshPhase == IF RefreshInFill THEN "fill" ELSE "receive"

Join ==
    /\ phase = "fill"
    /\ readHigh = Nil
    /\ pendingLow = Nil
    /\ now < MaxTime
    /\ phase' = "receive"
    /\ now' = now + 1
    /\ UNCHANGED << scheduled, nextTime, table, pendingLow, resolver, changedAt, changes, readHigh, staged >>

\* the refresh, once per receive call (or once per fill phase under RefreshInFill)
Refresh ==
    /\ phase = RefreshPhase
    /\ pendingLow = Nil
    /\ \/ /\ ~scheduled
          /\ scheduled' = TRUE
          /\ nextTime' = now + Period
          /\ UNCHANGED << table, pendingLow >>
       \/ /\ scheduled
          /\ (RefreshNever \/ now < nextTime \/ resolver = table)
          /\ nextTime' = IF ~RefreshNever /\ now >= nextTime THEN now + Period ELSE nextTime
          /\ UNCHANGED << scheduled, table, pendingLow >>
       \/ /\ scheduled
          /\ ~RefreshNever
          /\ now >= nextTime
          /\ resolver /= table
          /\ nextTime' = now + Period
          /\ table' = << resolver[1], table[2] >>
          /\ pendingLow' = resolver[2]
          /\ UNCHANGED scheduled
    /\ phase' = IF RefreshInFill THEN "fill" ELSE IF pendingLow' /= Nil THEN "writelow" ELSE "fill"
    /\ UNCHANGED << now, resolver, changedAt, changes, readHigh, staged >>

WriteLow ==
    /\ pendingLow /= Nil
    /\ phase = IF RefreshInFill THEN "fill" ELSE "writelow"
    /\ table' = << table[1], pendingLow >>
    /\ pendingLow' = Nil
    /\ phase' = "fill"
    /\ UNCHANGED << now, scheduled, nextTime, resolver, changedAt, changes, readHigh, staged >>

ReadHigh ==
    /\ phase = "fill"
    /\ readHigh = Nil
    /\ readHigh' = table[1]
    /\ UNCHANGED << phase, now, scheduled, nextTime, table, pendingLow, resolver, changedAt, changes, staged >>

ReadLow ==
    /\ phase = "fill"
    /\ readHigh /= Nil
    /\ staged' = staged \cup { [mac |-> << readHigh, table[2] >>, time |-> now] }
    /\ readHigh' = Nil
    /\ UNCHANGED << phase, now, scheduled, nextTime, table, pendingLow, resolver, changedAt, changes >>

ChangeGateway ==
    /\ changes < MaxChanges
    /\ \E mac \in Macs \ {resolver} :
        /\ resolver' = mac
        /\ changedAt' = now
        /\ changes' = changes + 1
    /\ UNCHANGED << phase, now, scheduled, nextTime, table, pendingLow, readHigh, staged >>

Next ==
    \/ Join
    \/ Refresh
    \/ WriteLow
    \/ ReadHigh
    \/ ReadLow
    \/ ChangeGateway

Spec == Init /\ [][Next]_vars

NoTornMac == \A packet \in staged : packet.mac \in Macs

FreshAfterPeriod ==
    \A packet \in staged :
        packet.time >= changedAt + Period => packet.mac = resolver

=============================================================================
