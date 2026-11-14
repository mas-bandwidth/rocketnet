---------------------------- MODULE RNUpdateClock ----------------------------
\* The update clock: the caller's raw time beside the clamped time the owner
\* hands out (rocketnet_update_clock_advance in
\* sdk/rocketnet_client_transport.cpp). The client transport, the server
\* transport and the message object each keep one, and the message object
\* hands its own resumed time to its transport (the two layers, below).
\*
\* A forward raw step of d advances the clamped time by exactly d while the
\* caller's clock is still behind the clamped one. A backwards raw step
\* returns the last good time, records the new raw time and counts one
\* misuse. A non-finite or negative sample is refused before the clock reads
\* it: it changes neither value and counts one misuse (NonFiniteStep stands
\* for both). Raw times are therefore never negative, which is why
\* BackwardStep draws its target from 0..raw-1: every negative sample, the
\* first and each one after it, is a NonFiniteStep, never a backwards step.
\* A sample above the ceiling MaxTime stands for is refused the same way
\* (AboveCeilingStep), so the raw time never passes it.
\* Once the caller's clock has caught up with the clamped one, the clock is
\* the caller's clock again.
\*
\* BASE's rule stands still after a backwards step until the caller's clock
\* passes the last good time again; the broken variant "stall" models that
\* rule, and tla/MCRNUpdateClockBrokenStall.cfg reverses it.
\*
\* Two layers: the message object's clock is the upper one, and after each of
\* its samples the object hands its clamped time to its transport, whose own
\* clock (low, lowRaw) follows it (LowerFollows; rocketnet_update_clock_follow,
\* the transport updates the links call). The ceiling bounds the caller's raw
\* samples only: the upper clamped time passes it after a backwards step, and
\* the transport takes it, so the transport's clock keeps pace with the
\* object's and counts nothing (LowerKeepsPace, LowerCountsNothing). The
\* broken variant "lowceiling" holds the handed-down time to the ceiling as
\* well, the code before the repair, and tla/MCRNUpdateClockBrokenLowCeiling.cfg
\* reverses it: the transport stands still and counts a misuse no caller
\* made. The object hands down its clamped time less a fixed origin (0 for a
\* server, its first update for a client); the model takes origin 0. The
\* object skips the hand-down on a refused sample before its clock starts,
\* when the hand-down would change nothing.
EXTENDS Naturals, Integers

CONSTANTS MaxTime, MaxSteps, Broken

VARIABLES clamped, raw, offset, misuse, prevClamped, prevRaw, lastStep,
          lastDelta, steps, low, lowRaw, lowMisuse

vars == <<clamped, raw, offset, misuse, prevClamped, prevRaw, lastStep,
          lastDelta, steps, low, lowRaw, lowMisuse>>

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
    /\ low = 0
    /\ lowRaw = 0
    /\ lowMisuse = 0

\* The transport's clock takes the time t its message object hands down,
\* by the same rule as the object's clock but not held to the ceiling: a
\* time behind its raw time is a backwards step (counted), and otherwise its
\* clamped time follows. Under "lowceiling" a time above MaxTime is refused
\* as a caller's sample would be.
LowerFollows(t) ==
    IF "lowceiling" \in Broken /\ t > MaxTime
    THEN /\ UNCHANGED <<low, lowRaw>>
         /\ lowMisuse' = lowMisuse + 1
    ELSE IF t < lowRaw
    THEN /\ low' = low
         /\ lowRaw' = t
         /\ lowMisuse' = lowMisuse + 1
    ELSE /\ low' = IF t >= low THEN t ELSE low + (t - lowRaw)
         /\ lowRaw' = t
         /\ lowMisuse' = lowMisuse

\* A forward raw step of d: the caller's clock moved forward by d. While the
\* caller's clock is still behind the clamped one, the clamped time advances
\* by exactly d. Once the caller's clock has caught up, the clamped time is
\* the caller's time. The broken variant "stall" is BASE's rule: the clamped
\* time stands still until the caller's clock passes it again.
ForwardStep(d) ==
    /\ steps < MaxSteps
    /\ d \in 1..MaxTime
    /\ raw + d <= MaxTime
    /\ clamped' = IF raw + d >= clamped THEN raw + d
                    ELSE (IF "stall" \in Broken THEN clamped ELSE clamped + d)
    /\ raw' = raw + d
    /\ offset' = offset
    /\ misuse' = misuse
    /\ prevClamped' = clamped
    /\ prevRaw' = raw
    /\ lastStep' = "forward"
    /\ lastDelta' = d
    /\ steps' = steps + 1
    /\ LowerFollows(clamped')

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
    /\ LowerFollows(clamped')

\* A non-finite or negative sample, refused before the clock reads it:
\* neither the clamped time nor the raw time nor the offset changes, and the
\* misuse is counted.
NonFiniteStep ==
    /\ steps < MaxSteps
    /\ UNCHANGED <<clamped, raw, offset>>
    /\ misuse' = misuse + 1
    /\ prevClamped' = clamped
    /\ prevRaw' = raw
    /\ lastStep' = "nonfinite"
    /\ lastDelta' = 0
    /\ steps' = steps + 1
    /\ LowerFollows(clamped')

\* A finite sample above the ceiling (ROCKETNET_UPDATE_TIME_MAX_SECONDS,
\* which MaxTime stands for), refused before the clock reads it like a
\* NonFiniteStep: nothing changes but the misuse count, so the raw time
\* stays at or below the ceiling and no call moves the clamped time by more
\* than it (ClampedStepBounded). The broken variant "noceiling" accepts it as a forward step to
\* MaxTime + 1, the code before the ceiling, where a step of any finite size
\* was taken and the sum could reach +inf.
AboveCeilingStep ==
    /\ steps < MaxSteps
    /\ IF "noceiling" \in Broken
       THEN /\ clamped' = IF MaxTime + 1 >= clamped THEN MaxTime + 1
                         ELSE clamped + (MaxTime + 1 - raw)
            /\ raw' = MaxTime + 1
            /\ misuse' = misuse
            /\ lastStep' = "forward"
            /\ lastDelta' = MaxTime + 1 - raw
       ELSE /\ UNCHANGED <<clamped, raw>>
            /\ misuse' = misuse + 1
            /\ lastStep' = "aboveceiling"
            /\ lastDelta' = 0
    /\ offset' = offset
    /\ prevClamped' = clamped
    /\ prevRaw' = raw
    /\ steps' = steps + 1
    /\ LowerFollows(clamped')

\* The step budget is spent: the model stops.
Done ==
    /\ steps = MaxSteps
    /\ UNCHANGED vars

Next ==
    \/ \E d \in 1..MaxTime : ForwardStep(d)
    \/ \E t \in 0..MaxTime : BackwardStep(t)
    \/ NonFiniteStep
    \/ AboveCeilingStep
    \/ Done

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ clamped \in 0..(MaxTime * (MaxSteps + 1))
    /\ raw \in 0..MaxTime
    /\ offset \in 0..(MaxTime * MaxSteps)
    /\ misuse \in 0..MaxSteps
    /\ prevClamped \in 0..(MaxTime * (MaxSteps + 1))
    /\ prevRaw \in 0..MaxTime
    /\ lastStep \in {"none", "forward", "backward", "nonfinite", "aboveceiling"}
    /\ lastDelta \in 0..MaxTime
    /\ steps \in 0..MaxSteps
    /\ low \in 0..(MaxTime * (MaxSteps + 1))
    /\ lowRaw \in 0..(MaxTime * (MaxSteps + 1))
    /\ lowMisuse \in 0..MaxSteps

\* The clamped time never decreases: no sample moves it back.
ClampedNeverDecreases == clamped >= prevClamped

\* After a backwards step, each later forward raw step of d advances the
\* clamped time by exactly d while the caller's clock is still behind; the
\* step that catches up with the clamped time makes the clock the caller's
\* clock again.
ResumeAdvances ==
    (lastStep = "forward" /\ offset > 0) =>
        (/\ (raw >= prevClamped => clamped = raw)
         /\ (raw < prevClamped => clamped = prevClamped + lastDelta))

\* A non-finite sample leaves both values alone.
NonFiniteChangesNothing ==
    lastStep = "nonfinite" => (/\ clamped = prevClamped /\ raw = prevRaw)

\* A sample above the ceiling leaves both values alone.
AboveCeilingChangesNothing ==
    lastStep = "aboveceiling" => (/\ clamped = prevClamped /\ raw = prevRaw)

\* No call moves the clamped time by more than the ceiling. In the code's
\* doubles the step is rounded once, so a call moves it by at most about
\* 2^52 seconds and the clock stays finite for any number of calls.
ClampedStepBounded == clamped - prevClamped <= MaxTime

\* The transport's clock is the object's clock: every time the object hands
\* down is taken, above the ceiling too.
LowerKeepsPace == low = clamped

\* The transport counts no misuse: every misuse is a caller's sample, counted
\* once, by the object.
LowerCountsNothing == lowMisuse = 0
=============================================================================
