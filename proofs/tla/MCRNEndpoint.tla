--------------------------- MODULE MCRNEndpoint ---------------------------
(***************************************************************************)
(* The small instance that checks RNEndpoint: N messages per side, the    *)
(* first N - 1 reliable ordered on channel 0 and the last unreliable on    *)
(* channel 1 (N = 3 for one-way safety, 2 for two-way safety and          *)
(* liveness), W = 2,                                                       *)
(* and two configurations, equal (num_channels 2 at both ends) and         *)
(* differing in num_channels (2 and 1).  The passing cases and           *)
(* BrokenOrderIgnored run Link capacity 1; the two CONFIG witnesses run    *)
(* capacity 2.                                                             *)
(*                                                                         *)
(*   MCRNEndpoint.cfg          safety, "arbitrary" in-order loss, failed   *)
(*                             sends, ack silence, timeouts at any time,   *)
(*                             under MCOneTimeout and MCOneWay             *)
(*   MCRNEndpointTwoWay.cfg    safety with both ends sending, N = 2, under *)
(*                             MCOneTimeout only                           *)
(*   MCRNEndpointPartial.cfg   the safety case with WriteAll = FALSE, so a *)
(*                             later ordered message can arrive first      *)
(*   MCRNEndpointLive.cfg      liveness under LiveSpec, each link losing   *)
(*                             its first packet                            *)
(*   MCRNEndpointBrokenConfigUnreliable.cfg  ConfigReliable = FALSE:       *)
(*                             ConfigVerifiedDespiteFirstLoss fails        *)
(*   MCRNEndpointBrokenConfigNotCompared.cfg ConfigCompared = FALSE:       *)
(*                             MismatchErrorsBothEnds fails                *)
(*   MCRNEndpointBrokenOrderIgnored.cfg      the Partial case with         *)
(*                             OrderChecked = FALSE: OrderedInOrder fails  *)
(*   MCRNEndpointTimeoutOff.cfg  the safety case with the reliable timeout *)
(*                             off (the default) and Clocked, T = 3: every *)
(*                             safety invariant, NoEndBeforeSilenceT and   *)
(*                             SilenceEndsAtT hold                         *)
(*   MCRNEndpointBrokenConfigWaitOnTimeout.cfg  the same with the CONFIG   *)
(*                             wait on a zero timeout (before v1.1.0):     *)
(*                             NoEndBeforeSilenceT fails                   *)
(*   MCRNEndpointBrokenSilenceEarly.cfg  the same with the silence end at  *)
(*                             T - 1: NoEndBeforeSilenceT fails            *)
(*   MCRNEndpointBrokenOptedInInsideT.cfg  the timeout configured,         *)
(*                             reliable timeout 2 < T = 3: the reliable    *)
(*                             clock ends a connection inside T            *)
(*                             (EndsNeedSilenceT fails; opted in carries   *)
(*                             no tolerance guarantee)                     *)
(* Every other case runs TimeoutConfigured = TRUE.  The four passing      *)
(* safety cases run under MCSafetyView (below).  Only the clocked cases   *)
(* set Clocked = TRUE.                                                     *)
(* README-RNEndpoint.md has the traces and the action-to-section map.      *)
(***************************************************************************)
EXTENDS RNEndpoint

\* the last message is the unreliable one, on channel 1
MCMsgRel  == [k \in 1..N |-> IF k = N THEN "unreliable" ELSE "ordered"]
MCMsgChan == [k \in 1..N |-> IF k = N THEN 1 ELSE 0]
MCCfgPairs == {[a |-> 2, b |-> 2], [a |-> 2, b |-> 1]}

\* The safety case's state constraint bounding Tick: the reliable timeout
\* fires at any moment, at one end per behaviour.  An end in error is inert
\* (it opens and discards, writes nothing), which under "arbitrary" loss is
\* the same as a live end that stays silent while the link drops every
\* packet into it; so a second timed-out end adds no reachable delivery
\* state, only its err value, which no invariant reads for this error.
MCOneTimeout == Cardinality({e \in E : err[e] = "RELIABLE_TIMEOUT"}) <= 1

\* The safety case's one-way constraint: only a's application sends; b
\* still resets, writes CONFIG and acks, and opens a's packets.  The model
\* is symmetric in the two ends except for cfg, so b's data to a is a's
\* data to b with the ends renamed; the differing pair puts the larger
\* num_channels at a, so a's channel-1 message is the data record outside
\* b's bounds (the VIOLATION path), and b's own refusal of that channel at
\* Send is the guard Send states.  The renaming covers each direction on
\* its own, not interference between them: a rule that confuses the
\* records an end sent with the records it received passes under MCOneWay,
\* so MCRNEndpointTwoWay.cfg checks both ends sending, at N = 2.
MCOneWay == nextMsg["b"] = 1

\* The safety cases' view: every variable but the links' send counters
\* (abSent, baSent) and delivery pulses (abPulse, baPulse).  Exact for
\* invariant checking when both links run "arbitrary" loss: Link reads sent
\* only in NextCount (to compute sent') and LostAtSend(NextCount), which is
\* FALSE under "arbitrary" whatever the count; pulse is read only by Deliver
\* to compute pulse', and exists for fairness (a Deliver step is never a
\* Drop step), which Spec does not have.  No RNEndpoint action and no
\* invariant reads either, so two states equal in the view have the same
\* enabled steps, the same successors in the view, and the same verdict on
\* every invariant: the quotient is a bisimulation.  Under any other
\* pattern the counter decides losses, so the view is then every variable.
MCSafetyView ==
    IF ABPattern = "arbitrary" /\ BAPattern = "arbitrary"
      THEN <<epVars, clockVars, abWire, baWire>>
      ELSE vars

=============================================================================
