------------------------ MODULE ProbeVacuityConnMem ------------------------
EXTENDS RNConnectionMemory
\* probe: in every reachable state the refusal changes no variable at all
ProbeRefuseStutter == [][\A s \in Slots : Refuse(s) => UNCHANGED vars]_vars
\* probe: the ghost cause = "send" is written on no reachable step
ProbeCauseNeverSend == [][\A s \in Slots : cause'[s] # "send"]_vars
=============================================================================
