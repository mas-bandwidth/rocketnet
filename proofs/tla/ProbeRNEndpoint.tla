------------------------- MODULE ProbeRNEndpoint -------------------------
\* Probe (temporary, not a case): mutations that must make each revision 1.1
\* property of RNEndpoint fail, applied by cfg overrides (Op <- ProbeOp).
EXTENDS MCRNEndpoint

\* v1.1.1: the one action that can falsify NoEndBeforeT is never enabled.
ProbeTickNeverEnabled == \A e \in E : ~ENABLED Tick(e)

\* the time bound of NoEndBeforeSilenceT replaced by FALSE
ProbeBoundFalse == ~TimeoutConfigured => \A e \in E : TimingEnd(e) => FALSE

\* the silence end one update late, and never
ProbeSilenceLate == SilenceT + 1
ProbeSilenceNever == SilenceT + 100

\* the window half before v1.1.0 on a zero timeout: an unacked record that has
\* been sent ends the connection at the first check (has_oldest and
\* now - oldest_first_send >= 0)
ProbeTick(e) ==
    /\ Live(e)
    /\ \/ TimeoutConfigured /\ ReliableDue(e)
       \/ ~TimeoutConfigured /\ firstWritten[e] /\ Unacked(e) # {}
    /\ TimeoutAnyTime \/ ~Live(Remote(e))
    /\ err' = [err EXCEPT ![e] = "RELIABLE_TIMEOUT"]
    /\ UNCHANGED <<cfg, nextMsg, relQ, acked, unrelQ, firstWritten, pendAck, remoteCfg, clockVars,
                   rcvd, held, deliv, opened, abVars, baVars>>

=============================================================================
