---------------------------- MODULE RNUpdateClock ----------------------------
\* The update clock of one owner: the caller's raw time beside the clamped
\* time the owner hands out (rocketnet_update_clock_advance in
\* sdk/rocketnet_client_transport.cpp). The client transport, the server
\* transport and the message object each keep one, and the message object
\* hands its own resumed time to its transport.
\*
\* A forward raw step of d advances the clamped time by exactly d. A
\* backwards raw step returns the last good time, records the new raw time
\* and counts one misuse. A non-finite sample changes neither value and
\* counts one misuse. While no backwards step has happened the offset is
\* exactly 0 and the clamped time is the caller's time, bit for bit.
\*
\* BASE's rule stands still after a backwards step until the caller's clock
\* passes the last good time again; the broken variant "stall" models that
\* rule, and tla/MCRNUpdateClockBrokenStall.cfg reverses it.
EXTENDS Naturals, Integers

CONSTANTS MaxTime, MaxSteps, Broken

VARIABLES clamped, raw, offset, misuse, prevClamped, prevRaw, lastStep,
          lastDelta, steps

vars == <<clamped, raw, offset, misuse, prevClamped, prevRaw, lastStep,
          lastDelta, steps>>

Init ==
    /\ clamped = 0
    /\ raw = 0
    /\ offset = 0
    /\ misuse = 0
    /\ prevClamped = 0
    /\ prevRaw = 0
    /\ lastStep = "none"
    /\ lastDelta = 0
    /\ steps = 0

\* A forward raw step of d: the caller's clock moved forward by d. The
\* clamped time advances by exactly d. The broken variant "stall" is BASE's
\* rule: the clamped time stands until the caller's clock passes it again.
ForwardStep(d) ==
    /\ steps < MaxSteps
    /\ d \in 1..MaxTime
    /\ raw + d <= MaxTime
    /\ clamped' = IF "stall" \in Broken
                    THEN (IF raw + d >= clamped THEN raw + d ELSE clamped)
                    ELSE (IF offset = 0 THEN raw + d ELSE clamped + d)
    /\ raw' = raw + d
    /\ offset' = offset
    /\ misuse' = misuse
    /\ prevClamped' = clamped
    /\ prevRaw' = raw
    /\ lastStep' = "forward"
    /\ lastDelta' = d
    /\ steps' = steps + 1

\* A backwards raw step: the caller's clock stepped back. The clamped time
\* stands, the raw time and the offset record the step, and the misuse is
\* counted.
BackwardStep(t) ==
    /\ steps < MaxSteps
    /\ t \in 0..(raw - 1)
    /\ clamped' = clamped
    /\ raw' = t
    /\ offset' = offset + (raw - t)
    /\ misuse' = misuse + 1
    /\ prevClamped' = clamped
    /\ prevRaw' = raw
    /\ lastStep' = "backward"
    /\ lastDelta' = 0
    /\ steps' = steps + 1

\* A non-finite sample: neither the clamped time nor the raw time nor the
\* offset changes, and the misuse is counted.
NonFiniteStep ==
    /\ steps < MaxSteps
    /\ UNCHANGED <<clamped, raw, offset>>
    /\ misuse' = misuse + 1
    /\ prevClamped' = clamped
    /\ prevRaw' = raw
    /\ lastStep' = "nonfinite"
    /\ lastDelta' = 0
    /\ steps' = steps + 1

\* The step budget is spent: the model stops.
Done ==
    /\ steps = MaxSteps
    /\ UNCHANGED vars

Next ==
    \/ \E d \in 1..MaxTime : ForwardStep(d)
    \/ \E t \in 0..MaxTime : BackwardStep(t)
    \/ NonFiniteStep
    \/ Done

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ clamped \in 0..(MaxTime * (MaxSteps + 1))
    /\ raw \in 0..MaxTime
    /\ offset \in 0..(MaxTime * MaxSteps)
    /\ misuse \in 0..MaxSteps
    /\ prevClamped \in 0..(MaxTime * (MaxSteps + 1))
    /\ prevRaw \in 0..MaxTime
    /\ lastStep \in {"none", "forward", "backward", "nonfinite"}
    /\ lastDelta \in 0..MaxTime
    /\ steps \in 0..MaxSteps

\* The clamped time never decreases: no sample moves it back.
ClampedNeverDecreases == clamped >= prevClamped

\* After a backwards step, each later forward raw step of d advances the
\* clamped time by exactly d: the clock resumes with the caller.
ResumeAdvances ==
    (lastStep = "forward" /\ offset > 0) =>
        (/\ clamped = prevClamped + lastDelta
         /\ lastDelta = raw - prevRaw)

\* A non-finite sample leaves both values alone.
NonFiniteChangesNothing ==
    lastStep = "nonfinite" => (/\ clamped = prevClamped /\ raw = prevRaw)
=============================================================================
