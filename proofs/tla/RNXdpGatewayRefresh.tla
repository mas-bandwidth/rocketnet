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
(* are joined before the next update starts. The clock is the platform     *)
(* monotonic clock the receive call samples (rocketnet_platform_time);     *)
(* the platform may advance by any step between updates (MaxStep), so the  *)
(* model does not tie time to one update tick. The resolve is the netlink  *)
(* query the refresh makes (rocketnet_gateway_netlink_refresh) on the main *)
(* thread: the default route, then its gateway's neighbour entry, each     *)
(* time, and the kernel's answer (resolver) changes at any moment, at most *)
(* MaxChanges times. The resolve may also fail (an error, no default      *)
(* route, or an unusable MAC: zero or multicast), at most MaxFailures      *)
(* times: the code then keeps the table's MAC, its deadline already moved  *)
(* (resolveFailing: the last resolve gave no usable MAC, until one does;   *)
(* the socket's gateway_resolve_failing tracks the error and no-route part *)
(* of it). The xdp threads send the staged copies and never read           *)
(* the table, so the staged set is what goes on the wire.                  *)
(*                                                                         *)
(* Actions:                                                                *)
(*   Join          fills joined; the next update starts, the platform      *)
(*                 clock advances by up to MaxStep                         *)
(*   Refresh       the receive call's refresh: schedules on its first      *)
(*                 call, then at or past the deadline sets the next        *)
(*                 deadline and resolves: a failed resolve keeps the       *)
(*                 table, an answer equal to the table changes nothing,    *)
(*                 and a changed answer starts the write                   *)
(*   Resolve       the netlink query's answer is written: the first half   *)
(*                 of a changed MAC                                        *)
(*   WriteLow      the refresh writes the second half                      *)
(*   ReceiveWithoutRefresh  the receive call when the refresh runs in the  *)
(*                 fill phase instead (RefreshInFill only)                 *)
(*   ReadHigh      a fill copies the first half of the table's MAC         *)
(*   ReadLow       the fill copies the second half and stages the packet   *)
(*   ChangeGateway the router is replaced (environment)                    *)
(*                                                                         *)
(* Invariants: NoTornMac (every staged MAC is one some resolve gave, never *)
(* a mix of two), StagedMacUsable (no staged MAC is an unusable answer)    *)
(* and                                                                     *)
(* FreshAfterPeriod (a packet staged Period or more clock units after the  *)
(* last change carries the new MAC, unless the last resolve before it      *)
(* failed: that resolve ran after the change, since the deadline it moved  *)
(* lies past the packet's time, and the table keeps the old MAC until a    *)
(* later resolve succeeds). The first receive only schedules, at the time  *)
(* create resolved in.                                                     *)
(*                                                                         *)
(* Rule switches for the reversed witnesses:                               *)
(*   FailureKeepsTable FALSE: a failed resolve writes its answer whatever  *)
(*                  it is (no rocketnet_xdp_reply_mac_usable check):       *)
(*                  StagedMacUsable fails                                  *)
(*   RefreshInFill  the refresh runs during the fill phase, as on a worker *)
(*                  or a timer thread, and its writes interleave with the  *)
(*                  fills' reads: NoTornMac fails                          *)
(*   RefreshNever   the gateway is resolved at create only, the defect     *)
(*                  before the refresh: FreshAfterPeriod fails             *)
(***************************************************************************)
EXTENDS Naturals

CONSTANTS Period, MaxTime, MaxStep, MaxChanges, MaxFailures, RefreshInFill, RefreshNever, FailureKeepsTable

Halves == {1, 2}
Macs == {<<1, 1>>, <<2, 2>>}
Bad == 3        \* a half of the unusable answer (zero or multicast)
Nil == 0

VARIABLES
    phase,          \* "receive", "resolve", "writelow" or "fill"
    now,            \* the platform monotonic clock, sampled each update
    scheduled,      \* the refresh has set its first deadline
    nextTime,       \* the refresh deadline
    table,          \* the table's gateway MAC, two halves
    pendingLow,     \* the second half the refresh has yet to store, Nil for none
    resolver,       \* the kernel's neighbour entry for the gateway
    changedAt,      \* the clock at the last change
    changes,        \* changes so far
    readHigh,       \* the half a fill has copied, Nil when no fill is mid-copy
    staged,         \* [mac, time, failing] of every staged packet
    failures,       \* failed resolves so far
    resolveFailing  \* the last resolve failed (gateway_resolve_failing)

vars == << phase, now, scheduled, nextTime, table, pendingLow, resolver, changedAt, changes, readHigh, staged,
           failures, resolveFailing >>

AnyHalf == Halves \cup {Bad}

TypeOK ==
    /\ phase \in {"receive", "resolve", "writelow", "fill"}
    /\ now \in 0..MaxTime
    /\ scheduled \in BOOLEAN
    /\ nextTime \in 0..(MaxTime + Period)
    /\ table \in AnyHalf \X AnyHalf
    /\ pendingLow \in Halves \cup {Nil}
    /\ resolver \in Macs
    /\ changedAt \in 0..MaxTime
    /\ changes \in 0..MaxChanges
    /\ readHigh \in AnyHalf \cup {Nil}
    /\ staged \subseteq [mac : AnyHalf \X AnyHalf, time : 0..MaxTime, failing : BOOLEAN]
    /\ failures \in 0..MaxFailures
    /\ resolveFailing \in BOOLEAN

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
    /\ failures = 0
    /\ resolveFailing = FALSE

\* the phase in which the refresh's stores run
RefreshPhase == IF RefreshInFill THEN "fill" ELSE "receive"

Join ==
    /\ phase = "fill"
    /\ readHigh = Nil
    /\ pendingLow = Nil
    /\ now < MaxTime
    /\ \E delta \in 1..MaxStep :
        /\ now + delta <= MaxTime
        /\ now' = now + delta
    /\ phase' = "receive"
    /\ UNCHANGED << scheduled, nextTime, table, pendingLow, resolver, changedAt, changes, readHigh, staged,
                    failures, resolveFailing >>

\* the refresh, once per receive call (or once per fill phase under
\* RefreshInFill): the first call schedules; before the deadline nothing; at
\* or past it the deadline moves and the resolve runs: it fails (the table
\* is kept, or under FailureKeepsTable = FALSE the unusable answer written),
\* or answers the table's MAC (nothing changes), or answers another MAC
\* (the write starts)
Refresh ==
    /\ phase = RefreshPhase
    /\ pendingLow = Nil
    /\ \/ /\ ~scheduled
          /\ scheduled' = TRUE
          /\ nextTime' = now + Period
          /\ phase' = "fill"
          /\ UNCHANGED << table, pendingLow, failures, resolveFailing >>
       \/ /\ scheduled
          /\ (RefreshNever \/ now < nextTime \/ resolver = table)
          /\ nextTime' = IF ~RefreshNever /\ now >= nextTime THEN now + Period ELSE nextTime
          /\ resolveFailing' = IF ~RefreshNever /\ now >= nextTime THEN FALSE ELSE resolveFailing
          /\ phase' = "fill"
          /\ UNCHANGED << scheduled, table, pendingLow, failures >>
       \/ /\ scheduled
          /\ ~RefreshNever
          /\ now >= nextTime
          /\ resolver /= table
          /\ nextTime' = now + Period
          /\ resolveFailing' = FALSE
          /\ phase' = "resolve"
          /\ UNCHANGED << scheduled, table, pendingLow, failures >>
       \/ /\ scheduled
          /\ ~RefreshNever
          /\ now >= nextTime
          /\ failures < MaxFailures
          /\ nextTime' = now + Period
          /\ failures' = failures + 1
          /\ resolveFailing' = TRUE
          /\ table' = IF FailureKeepsTable THEN table ELSE << Bad, Bad >>
          /\ phase' = "fill"
          /\ UNCHANGED << scheduled, pendingLow >>
    /\ UNCHANGED << now, resolver, changedAt, changes, readHigh, staged >>

\* the netlink query's answer: the kernel's neighbour entry the resolve read
Resolve ==
    /\ phase = "resolve"
    /\ table' = << resolver[1], table[2] >>
    /\ pendingLow' = resolver[2]
    /\ phase' = IF RefreshInFill THEN "fill" ELSE "writelow"
    /\ UNCHANGED << now, scheduled, nextTime, resolver, changedAt, changes, readHigh, staged, failures, resolveFailing >>

WriteLow ==
    /\ pendingLow /= Nil
    /\ phase = IF RefreshInFill THEN "fill" ELSE "writelow"
    /\ table' = << table[1], pendingLow >>
    /\ pendingLow' = Nil
    /\ phase' = "fill"
    /\ UNCHANGED << now, scheduled, nextTime, resolver, changedAt, changes, readHigh, staged, failures, resolveFailing >>

\* the receive call with the refresh moved out of it (RefreshInFill only)
ReceiveWithoutRefresh ==
    /\ RefreshInFill
    /\ phase = "receive"
    /\ phase' = "fill"
    /\ UNCHANGED << now, scheduled, nextTime, table, pendingLow, resolver, changedAt, changes, readHigh, staged,
                    failures, resolveFailing >>

ReadHigh ==
    /\ phase = "fill"
    /\ readHigh = Nil
    /\ readHigh' = table[1]
    /\ UNCHANGED << phase, now, scheduled, nextTime, table, pendingLow, resolver, changedAt, changes, staged,
                    failures, resolveFailing >>

ReadLow ==
    /\ phase = "fill"
    /\ readHigh /= Nil
    /\ staged' = staged \cup { [mac |-> << readHigh, table[2] >>, time |-> now, failing |-> resolveFailing] }
    /\ readHigh' = Nil
    /\ UNCHANGED << phase, now, scheduled, nextTime, table, pendingLow, resolver, changedAt, changes,
                    failures, resolveFailing >>

ChangeGateway ==
    /\ changes < MaxChanges
    /\ \E mac \in Macs \ {resolver} :
        /\ resolver' = mac
        /\ changedAt' = now
        /\ changes' = changes + 1
    /\ UNCHANGED << phase, now, scheduled, nextTime, table, pendingLow, readHigh, staged, failures, resolveFailing >>

Next ==
    \/ Join
    \/ Refresh
    \/ Resolve
    \/ ReceiveWithoutRefresh
    \/ WriteLow
    \/ ReadHigh
    \/ ReadLow
    \/ ChangeGateway

Spec == Init /\ [][Next]_vars

NoTornMac == \A packet \in staged : packet.mac \in Macs

StagedMacUsable == \A packet \in staged : packet.mac[1] # Bad /\ packet.mac[2] # Bad

FreshAfterPeriod ==
    \A packet \in staged :
        (packet.time >= changedAt + Period /\ ~packet.failing) => packet.mac = resolver

=============================================================================
