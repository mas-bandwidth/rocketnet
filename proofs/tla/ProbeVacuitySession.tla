------------------------ MODULE ProbeVacuitySession ------------------------
EXTENDS ClientSession
\* probe: a start-side MessageEnd is only ever taken on a receive-exhausted region,
\* so the ghost sendEnded is never written
ProbeSendEndGuarded == [][\A to \in {"MESSAGE_MEMORY_EXHAUSTED"} : MessageEnd("send", to) => rxEx = cur]_vars
ProbeGhostNeverWritten == [][sendEnded' = sendEnded]_vars
=============================================================================
