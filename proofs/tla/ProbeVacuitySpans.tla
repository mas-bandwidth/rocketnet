------------------------ MODULE ProbeVacuitySpans ------------------------
EXTENDS RNSendSpans
ProbeRefuseStutter == [][\A l \in Lengths : Refuse(l) => UNCHANGED vars]_vars
=============================================================================
