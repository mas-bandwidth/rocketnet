------------------------ MODULE ProbeVacuityFlows ------------------------
EXTENDS RNFlows
\* probe: the only application-phase step that changes owner is never enabled
ProbeKickDisabled == \A s \in Slots : ~ENABLED Kick(s)
=============================================================================
